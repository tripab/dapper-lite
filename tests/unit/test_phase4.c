/**
 * test_phase4.c - Unit tests for Phase 4 (Asynchronous Span Reporting)
 *
 * Tests:
 * - Wire format serialization/deserialization round-trip
 * - Ring buffer correctness (push, pop, full, empty, wrap-around)
 * - Ring buffer concurrent SPSC correctness
 * - File sink write and read-back
 * - UDP sink send and receive
 * - Exporter end-to-end (submit -> background thread -> file)
 * - Backpressure (drop counting when ring buffer is full)
 */

#define _POSIX_C_SOURCE 200809L
#include "dapper/exporter.h"
#include "dapper/span.h"
#include "dapper/trace.h"
#include "export/export_internal.h" /* white-box: sink_t internals */
#include "minunit.h"
#include <arpa/inet.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

int tests_run = 0;
int tests_failed = 0;

/* ========== Serialization Tests ========== */

static const char *test_serialize_roundtrip() {
  trace_t *trace = trace_create();
  span_t *span = span_create(trace, NULL, "roundtrip_op");
  span_annotate(span, "http.method", "GET");
  span_annotate(span, "http.url", "/api/users");
  span_finish(span);

  uint8_t buf[SPAN_WIRE_MAX_SIZE];
  int len = span_serialize(buf, sizeof(buf), span, true);
  mu_assert("serialize should succeed", len > 0);
  mu_assert("serialize should include header + name + annotations",
            len > SPAN_WIRE_HEADER_SIZE);

  span_t decoded;
  bool sampled;
  int consumed = span_deserialize(buf, (size_t)len, &decoded, &sampled);
  mu_assert("deserialize should succeed", consumed > 0);
  mu_assert_eq("consumed should equal serialized", (unsigned long)len,
               (unsigned long)consumed);

  mu_assert_eq("trace_id preserved", span->trace_id, decoded.trace_id);
  mu_assert_eq("span_id preserved", span->span_id, decoded.span_id);
  mu_assert_eq("parent_span_id preserved", span->parent_span_id,
               decoded.parent_span_id);
  mu_assert_str_eq("name preserved", span->name, decoded.name);
  mu_assert("sampled preserved", sampled == true);
  mu_assert_eq("annotation count preserved", 2,
               (unsigned long)decoded.annotation_count);
  mu_assert_str_eq("annotation key 0", "http.method",
                   decoded.annotations[0].key);
  mu_assert_str_eq("annotation val 0", "GET", decoded.annotations[0].value);

  /* Duration preserved (within microsecond rounding) */
  uint64_t orig_us = (span->monotonic_end_ns - span->monotonic_start_ns) / 1000;
  uint64_t decoded_us = decoded.monotonic_end_ns / 1000;
  mu_assert_eq("duration_us preserved", orig_us, decoded_us);

  trace_destroy(trace);
  return NULL;
}

static const char *test_serialize_null_safety() {
  uint8_t buf[SPAN_WIRE_MAX_SIZE];
  span_t span = {0};
  bool sampled;

  mu_assert("NULL buffer returns -1",
            span_serialize(NULL, sizeof(buf), &span, false) == -1);
  mu_assert("NULL span returns -1",
            span_serialize(buf, sizeof(buf), NULL, false) == -1);
  mu_assert("buffer too small returns -1",
            span_serialize(buf, 10, &span, false) == -1);
  mu_assert("deserialize NULL buffer returns -1",
            span_deserialize(NULL, sizeof(buf), &span, &sampled) == -1);
  mu_assert("deserialize NULL span returns -1",
            span_deserialize(buf, sizeof(buf), NULL, &sampled) == -1);
  mu_assert("deserialize NULL sampled returns -1",
            span_deserialize(buf, sizeof(buf), &span, NULL) == -1);

  return NULL;
}

static const char *test_serialize_annotation_truncation() {
  trace_t *trace = trace_create();
  span_t *span = span_create(trace, NULL, "trunc_test");
  for (int i = 0; i < 16; i++) {
    char key[32], val[64];
    snprintf(key, sizeof(key), "key_%d", i);
    snprintf(val, sizeof(val), "value_%d", i);
    span_annotate(span, key, val);
  }
  span_finish(span);

  /* Serialize into a tight buffer — some annotations should be truncated */
  uint8_t buf[120];
  int len = span_serialize(buf, sizeof(buf), span, true);
  mu_assert("serialize with truncation should succeed", len > 0);
  mu_assert("serialized fits in buffer", len <= 120);

  span_t decoded;
  bool sampled;
  int consumed = span_deserialize(buf, (size_t)len, &decoded, &sampled);
  mu_assert("deserialize truncated should succeed", consumed > 0);
  mu_assert("fewer annotations after truncation",
            decoded.annotation_count < 16);
  mu_assert("at least some annotations preserved",
            decoded.annotation_count > 0);

  trace_destroy(trace);
  return NULL;
}

/* ========== Ring Buffer Tests ========== */

static const char *test_ring_buffer_push_pop() {
  ring_buffer_t *rb = ring_buffer_create(4);
  mu_assert("ring buffer create should succeed", rb != NULL);
  mu_assert("new ring buffer should be empty", ring_buffer_is_empty(rb));
  mu_assert("new ring buffer should not be full", !ring_buffer_is_full(rb));

  trace_t *trace = trace_create();
  span_t *span = span_create(trace, NULL, "rb_test");
  span_annotate(span, "k", "v");
  span_finish(span);

  /* Push and pop */
  mu_assert("push should succeed", ring_buffer_push(rb, span, true));
  mu_assert("should not be empty after push", !ring_buffer_is_empty(rb));

  uint8_t buf[RING_BUFFER_ENTRY_SIZE];
  size_t len;
  mu_assert("pop should succeed", ring_buffer_pop(rb, buf, &len));
  mu_assert("pop length should be positive", len > 0);
  mu_assert("should be empty after pop", ring_buffer_is_empty(rb));

  /* Verify data round-trip */
  span_t decoded;
  bool sampled;
  mu_assert("deserialize from ring buffer should succeed",
            span_deserialize(buf, len, &decoded, &sampled) > 0);
  mu_assert_str_eq("name preserved through ring buffer", "rb_test",
                   decoded.name);

  ring_buffer_destroy(rb);
  trace_destroy(trace);
  return NULL;
}

static const char *test_ring_buffer_full_and_backpressure() {
  ring_buffer_t *rb = ring_buffer_create(4); /* 3 usable slots */
  mu_assert("create should succeed", rb != NULL);

  trace_t *trace = trace_create();
  span_t *span = span_create(trace, NULL, "full_test");
  span_finish(span);

  /* Fill all usable slots */
  mu_assert("push 1 should succeed", ring_buffer_push(rb, span, true));
  mu_assert("push 2 should succeed", ring_buffer_push(rb, span, true));
  mu_assert("push 3 should succeed", ring_buffer_push(rb, span, true));
  mu_assert("should be full", ring_buffer_is_full(rb));
  mu_assert("push 4 should fail (full)", !ring_buffer_push(rb, span, true));

  /* Pop one, push one (wrap-around) */
  uint8_t buf[RING_BUFFER_ENTRY_SIZE];
  size_t len;
  mu_assert("pop should succeed", ring_buffer_pop(rb, buf, &len));
  mu_assert("should not be full after pop", !ring_buffer_is_full(rb));
  mu_assert("push after pop should succeed", ring_buffer_push(rb, span, true));
  mu_assert("should be full again", ring_buffer_is_full(rb));

  ring_buffer_destroy(rb);
  trace_destroy(trace);
  return NULL;
}

static const char *test_ring_buffer_empty_pop() {
  ring_buffer_t *rb = ring_buffer_create(4);
  mu_assert("create should succeed", rb != NULL);

  uint8_t buf[RING_BUFFER_ENTRY_SIZE];
  size_t len;
  mu_assert("pop from empty should fail", !ring_buffer_pop(rb, buf, &len));

  ring_buffer_destroy(rb);
  return NULL;
}

/* Concurrent SPSC test data */
#define CONCURRENT_SPANS 10000
#define CONCURRENT_RB_CAP 256

static ring_buffer_t *g_crb;
static _Atomic int g_produced;
static _Atomic int g_consumed;
static _Atomic int g_dropped;

static void *concurrent_producer(void *arg) {
  trace_t *trace = (trace_t *)arg;
  for (int i = 0; i < CONCURRENT_SPANS; i++) {
    span_t *s = span_create(trace, NULL, "concurrent");
    span_finish(s);
    if (ring_buffer_push(g_crb, s, true)) {
      atomic_fetch_add(&g_produced, 1);
    } else {
      atomic_fetch_add(&g_dropped, 1);
    }
  }
  return NULL;
}

static void *concurrent_consumer(void *arg) {
  (void)arg;
  uint8_t buf[RING_BUFFER_ENTRY_SIZE];
  size_t len;
  while (1) {
    if (ring_buffer_pop(g_crb, buf, &len)) {
      span_t decoded;
      bool sampled;
      if (span_deserialize(buf, len, &decoded, &sampled) > 0) {
        atomic_fetch_add(&g_consumed, 1);
      }
    } else {
      int total = atomic_load(&g_produced) + atomic_load(&g_dropped);
      if (total >= CONCURRENT_SPANS && ring_buffer_is_empty(g_crb)) {
        break;
      }
      usleep(10);
    }
  }
  return NULL;
}

static const char *test_ring_buffer_concurrent_spsc() {
  g_crb = ring_buffer_create(CONCURRENT_RB_CAP);
  mu_assert("concurrent rb create should succeed", g_crb != NULL);
  atomic_store(&g_produced, 0);
  atomic_store(&g_consumed, 0);
  atomic_store(&g_dropped, 0);

  trace_t *trace = trace_create();
  pthread_t prod, cons;
  pthread_create(&cons, NULL, concurrent_consumer, NULL);
  pthread_create(&prod, NULL, concurrent_producer, trace);

  pthread_join(prod, NULL);
  pthread_join(cons, NULL);

  int produced = atomic_load(&g_produced);
  int consumed = atomic_load(&g_consumed);
  int dropped = atomic_load(&g_dropped);

  mu_assert_eq("total should match", (unsigned long)CONCURRENT_SPANS,
               (unsigned long)(produced + dropped));
  mu_assert_eq("consumed should match produced", (unsigned long)produced,
               (unsigned long)consumed);

  ring_buffer_destroy(g_crb);
  trace_destroy(trace);
  return NULL;
}

/* ========== File Sink Tests ========== */

static const char *test_file_sink_write_read() {
  const char *path = "/tmp/dapper_test_filesink.bin";
  unlink(path);

  sink_t *sink = sink_create_file(path);
  mu_assert("file sink create should succeed", sink != NULL);

  trace_t *trace = trace_create();
  span_t *s = span_create(trace, NULL, "filesink_op");
  span_annotate(s, "env", "test");
  span_finish(s);

  uint8_t buf[SPAN_WIRE_MAX_SIZE];
  int len = span_serialize(buf, sizeof(buf), s, true);
  mu_assert("serialize should succeed", len > 0);
  mu_assert("write should succeed", sink->write(sink, buf, (size_t)len) == 0);

  sink_destroy(sink);

  /* Read back */
  FILE *fp = fopen(path, "rb");
  mu_assert("file should exist", fp != NULL);
  uint32_t payload_len;
  mu_assert("read length should succeed", fread(&payload_len, 4, 1, fp) == 1);
  mu_assert_eq("payload length should match", (unsigned long)len,
               (unsigned long)payload_len);

  uint8_t readbuf[SPAN_WIRE_MAX_SIZE];
  mu_assert("read payload should succeed",
            fread(readbuf, 1, payload_len, fp) == payload_len);
  fclose(fp);

  span_t decoded;
  bool sampled;
  mu_assert("deserialize should succeed",
            span_deserialize(readbuf, payload_len, &decoded, &sampled) > 0);
  mu_assert_str_eq("name preserved", "filesink_op", decoded.name);
  mu_assert("sampled preserved", sampled == true);

  unlink(path);
  trace_destroy(trace);
  return NULL;
}

/* ========== UDP Sink Tests ========== */

#define UDP_TEST_PORT 19877

static uint8_t g_udp_recv_buf[SPAN_WIRE_MAX_SIZE];
static size_t g_udp_recv_len;

static void *udp_receiver(void *arg) {
  int sockfd = *(int *)arg;
  struct sockaddr_in from;
  socklen_t fromlen = sizeof(from);
  ssize_t n = recvfrom(sockfd, g_udp_recv_buf, sizeof(g_udp_recv_buf), 0,
                       (struct sockaddr *)&from, &fromlen);
  if (n > 0) {
    g_udp_recv_len = (size_t)n;
  }
  return NULL;
}

static const char *test_udp_sink_send_receive() {
  /* Set up receiver */
  int recv_sock = socket(AF_INET, SOCK_DGRAM, 0);
  mu_assert("receiver socket should succeed", recv_sock >= 0);

  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = inet_addr("127.0.0.1");
  addr.sin_port = htons(UDP_TEST_PORT);
  mu_assert("bind should succeed",
            bind(recv_sock, (struct sockaddr *)&addr, sizeof(addr)) == 0);

  pthread_t recv_thread;
  pthread_create(&recv_thread, NULL, udp_receiver, &recv_sock);
  usleep(10000);

  /* Send via UDP sink */
  sink_t *sink = sink_create_udp("127.0.0.1", UDP_TEST_PORT);
  mu_assert("udp sink create should succeed", sink != NULL);

  trace_t *trace = trace_create();
  span_t *s = span_create(trace, NULL, "udp_op");
  span_finish(s);

  uint8_t buf[SPAN_WIRE_MAX_SIZE];
  int len = span_serialize(buf, sizeof(buf), s, false);
  mu_assert("serialize should succeed", len > 0);
  mu_assert("udp write should succeed",
            sink->write(sink, buf, (size_t)len) == 0);

  pthread_join(recv_thread, NULL);
  close(recv_sock);

  mu_assert_eq("received length should match", (unsigned long)len,
               (unsigned long)g_udp_recv_len);

  span_t decoded;
  bool sampled;
  mu_assert(
      "deserialize received should succeed",
      span_deserialize(g_udp_recv_buf, g_udp_recv_len, &decoded, &sampled) > 0);
  mu_assert_str_eq("name preserved over UDP", "udp_op", decoded.name);
  mu_assert("sampled=false preserved", sampled == false);

  sink_destroy(sink);
  trace_destroy(trace);
  return NULL;
}

/* ========== Exporter End-to-End Tests ========== */

static const char *test_exporter_end_to_end() {
  const char *path = "/tmp/dapper_test_exporter_e2e.bin";
  unlink(path);

  exporter_t *exp = exporter_create_file(path);
  mu_assert("exporter create should succeed", exp != NULL);
  mu_assert("exporter start should succeed", exporter_start(exp) == 0);
  mu_assert("double start should fail", exporter_start(exp) == -1);

  trace_t *trace = trace_create();
  for (int i = 0; i < 50; i++) {
    char name[32];
    snprintf(name, sizeof(name), "e2e_%d", i);
    span_t *s = span_create(trace, NULL, name);
    span_finish(s);
    exporter_submit(exp, s);
  }

  /* Wait for export */
  usleep(200000);

  exporter_stats_t stats;
  exporter_get_stats(exp, &stats);
  mu_assert_eq("all submitted", 50, (unsigned long)stats.spans_submitted);
  mu_assert_eq("all exported", 50, (unsigned long)stats.spans_exported);
  mu_assert_eq("none dropped", 0, (unsigned long)stats.spans_dropped);

  exporter_destroy(exp);

  /* Verify file */
  FILE *fp = fopen(path, "rb");
  mu_assert("output file should exist", fp != NULL);
  int count = 0;
  while (1) {
    uint32_t payload_len;
    if (fread(&payload_len, 4, 1, fp) != 1)
      break;
    uint8_t buf[SPAN_WIRE_MAX_SIZE];
    if (fread(buf, 1, payload_len, fp) != payload_len)
      break;
    count++;
  }
  fclose(fp);
  mu_assert_eq("file should contain all spans", 50, (unsigned long)count);

  unlink(path);
  trace_destroy(trace);
  return NULL;
}

static const char *test_exporter_reflects_sampling() {
  /* exporter_submit() must encode the span's real sampling decision,
   * not a hardcoded "true". */
  const char *path = "/tmp/dapper_test_exporter_sampled.bin";
  unlink(path);

  exporter_t *exp = exporter_create_file(path);
  mu_assert("exporter create should succeed", exp != NULL);
  mu_assert("exporter start should succeed", exporter_start(exp) == 0);

  trace_t *trace = trace_create();
  trace->sampled = false; /* unsampled trace */
  span_t *s = span_create(trace, NULL, "unsampled");
  span_finish(s);
  mu_assert("span inherits unsampled decision", s->sampled == false);
  exporter_submit(exp, s);

  usleep(200000);
  exporter_destroy(exp);

  FILE *fp = fopen(path, "rb");
  mu_assert("output file should exist", fp != NULL);
  uint32_t payload_len;
  mu_assert_eq("read length prefix", 1UL,
               (unsigned long)fread(&payload_len, 4, 1, fp));
  uint8_t buf[SPAN_WIRE_MAX_SIZE];
  mu_assert_eq("read payload", (unsigned long)payload_len,
               (unsigned long)fread(buf, 1, payload_len, fp));
  fclose(fp);

  span_t decoded;
  bool sampled = true;
  mu_assert("deserialize ok",
            span_deserialize(buf, payload_len, &decoded, &sampled) > 0);
  mu_assert("wire reflects unsampled decision", sampled == false);
  mu_assert("decoded span carries unsampled", decoded.sampled == false);

  unlink(path);
  trace_destroy(trace);
  return NULL;
}

static const char *test_exporter_backpressure() {
  const char *path = "/tmp/dapper_test_exporter_bp.bin";
  unlink(path);

  exporter_t *exp = exporter_create_file(path);
  mu_assert("exporter create should succeed", exp != NULL);
  /* Do NOT start — ring buffer will fill up */

  trace_t *trace = trace_create();
  span_t *s = span_create(trace, NULL, "bp");
  span_finish(s);

  for (int i = 0; i < 5000; i++) {
    exporter_submit(exp, s);
  }

  exporter_stats_t stats;
  exporter_get_stats(exp, &stats);
  mu_assert_eq("all submitted", 5000, (unsigned long)stats.spans_submitted);
  mu_assert("some dropped", stats.spans_dropped > 0);
  mu_assert_eq("dropped count correct", 905,
               (unsigned long)stats.spans_dropped);

  /* Start and drain */
  mu_assert("start should succeed", exporter_start(exp) == 0);
  usleep(500000);

  exporter_get_stats(exp, &stats);
  mu_assert_eq("exported matches buffered", 4095,
               (unsigned long)stats.spans_exported);

  exporter_destroy(exp);
  unlink(path);
  trace_destroy(trace);
  return NULL;
}

static const char *test_exporter_null_safety() {
  exporter_stats_t stats;
  exporter_submit(NULL, NULL);
  exporter_stop(NULL);
  exporter_destroy(NULL);
  exporter_get_stats(NULL, &stats);
  mu_assert("null safety should not crash", 1);
  return NULL;
}

/* ========== Test Suite ========== */

static const char *all_tests() {
  printf("Running Phase 4 Unit Tests (Async Export)\n");
  printf("==========================================\n\n");

  printf("Serialization Tests:\n");
  mu_run_test(test_serialize_roundtrip);
  mu_run_test(test_serialize_null_safety);
  mu_run_test(test_serialize_annotation_truncation);

  printf("Ring Buffer Tests:\n");
  mu_run_test(test_ring_buffer_push_pop);
  mu_run_test(test_ring_buffer_full_and_backpressure);
  mu_run_test(test_ring_buffer_empty_pop);
  mu_run_test(test_ring_buffer_concurrent_spsc);

  printf("Sink Tests:\n");
  mu_run_test(test_file_sink_write_read);
  mu_run_test(test_udp_sink_send_receive);

  printf("Exporter Tests:\n");
  mu_run_test(test_exporter_end_to_end);
  mu_run_test(test_exporter_reflects_sampling);
  mu_run_test(test_exporter_backpressure);
  mu_run_test(test_exporter_null_safety);

  return NULL;
}

int main(void) {
  const char *result = all_tests();
  if (result != 0) {
    printf("\nFinal failure: %s\n", result);
  }
  mu_report();
}
