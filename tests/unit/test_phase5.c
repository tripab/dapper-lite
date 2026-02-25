/**
 * test_phase5.c - Unit tests for Phase 5 (Central Collector)
 *
 * Tests:
 * - Protocol decode (valid, invalid, edge cases)
 * - Partial trace lifecycle (create, add spans, destroy)
 * - Trace map (insert, find, flush timeout)
 * - Out-of-order span assembly
 * - Storage write and read-back
 * - Collector end-to-end (UDP send -> collector -> storage file)
 */

#define _POSIX_C_SOURCE 200809L
#include "dapper/collector.h"
#include "dapper/exporter.h"
#include "dapper/span.h"
#include "dapper/trace.h"
#include "minunit.h"
#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

int tests_run = 0;
int tests_failed = 0;

/* ============================================================
 * Helper: create a serialized span packet for testing
 * ============================================================ */

static int make_test_packet(uint8_t *buf, size_t bufsize, trace_id_t trace_id,
                            span_id_t span_id, span_id_t parent_span_id,
                            const char *name, bool sampled) {
  trace_t *trace = trace_create_with_id(trace_id);
  span_t *parent_sentinel = NULL;

  span_t *span = span_create(trace, parent_sentinel, name);
  /* Override IDs to desired values */
  span->span_id = span_id;
  span->parent_span_id = parent_span_id;
  span_finish(span);

  int len = span_serialize(buf, bufsize, span, sampled);
  trace_destroy(trace);
  return len;
}

/* ============================================================
 * Protocol Decode Tests
 * ============================================================ */

static const char *test_decode_valid_packet() {
  uint8_t buf[SPAN_WIRE_MAX_SIZE];
  int len = make_test_packet(buf, sizeof(buf), 1001, 2001, 0, "root_op", true);
  mu_assert("serialize should succeed", len > 0);

  span_t span;
  bool sampled;
  int rc = collector_decode_span(buf, (size_t)len, &span, &sampled);
  mu_assert("decode should succeed", rc == 0);
  mu_assert_eq("trace_id", 1001UL, (unsigned long)span.trace_id);
  mu_assert_eq("span_id", 2001UL, (unsigned long)span.span_id);
  mu_assert_eq("parent_span_id", 0UL, (unsigned long)span.parent_span_id);
  mu_assert_str_eq("name", "root_op", span.name);
  mu_assert("sampled", sampled == true);
  return NULL;
}

static const char *test_decode_too_short() {
  uint8_t buf[10] = {0};
  span_t span;
  bool sampled;
  int rc = collector_decode_span(buf, sizeof(buf), &span, &sampled);
  mu_assert("too-short packet should fail", rc < 0);
  return NULL;
}

static const char *test_decode_null_args() {
  uint8_t buf[SPAN_WIRE_MAX_SIZE];
  span_t span;
  bool sampled;

  mu_assert("null data fails",
            collector_decode_span(NULL, 48, &span, &sampled) < 0);
  mu_assert("null span fails",
            collector_decode_span(buf, 48, NULL, &sampled) < 0);
  mu_assert("null sampled fails",
            collector_decode_span(buf, 48, &span, NULL) < 0);
  return NULL;
}

static const char *test_decode_zero_trace_id() {
  uint8_t buf[SPAN_WIRE_MAX_SIZE];
  /* Create a valid packet, then zero out the trace_id field */
  int len = make_test_packet(buf, sizeof(buf), 999, 2001, 0, "op", true);
  mu_assert("serialize should succeed", len > 0);
  /* Zero out trace_id at offset 0 (8 bytes) */
  memset(buf, 0, 8);

  span_t span;
  bool sampled;
  int rc = collector_decode_span(buf, (size_t)len, &span, &sampled);
  mu_assert("zero trace_id should fail", rc < 0);
  return NULL;
}

static const char *test_decode_zero_span_id() {
  uint8_t buf[SPAN_WIRE_MAX_SIZE];
  int len = make_test_packet(buf, sizeof(buf), 1001, 999, 0, "op", true);
  mu_assert("serialize should succeed", len > 0);
  /* Zero out span_id at offset 8 (8 bytes) */
  memset(buf + 8, 0, 8);

  span_t span;
  bool sampled;
  int rc = collector_decode_span(buf, (size_t)len, &span, &sampled);
  mu_assert("zero span_id should fail", rc < 0);
  return NULL;
}

/* ============================================================
 * Partial Trace Tests
 * ============================================================ */

static const char *test_partial_trace_create_destroy() {
  partial_trace_t *pt = partial_trace_create(42);
  mu_assert("create should succeed", pt != NULL);
  mu_assert_eq("trace_id", 42UL, (unsigned long)pt->trace_id);
  mu_assert_eq("span_count", 0UL, (unsigned long)pt->span_count);
  mu_assert("no root initially", pt->has_root == false);
  mu_assert("spans is NULL", pt->spans == NULL);
  partial_trace_destroy(pt);
  return NULL;
}

static const char *test_partial_trace_add_spans() {
  partial_trace_t *pt = partial_trace_create(100);

  /* Add a root span */
  span_t root;
  memset(&root, 0, sizeof(root));
  root.trace_id = 100;
  root.span_id = 1;
  root.parent_span_id = 0;
  snprintf(root.name, sizeof(root.name), "root");

  int rc = partial_trace_add_span(pt, &root, true);
  mu_assert("add root should succeed", rc == 0);
  mu_assert_eq("span_count after root", 1UL, (unsigned long)pt->span_count);
  mu_assert("has_root after root", pt->has_root == true);
  mu_assert("sampled from first span", pt->sampled == true);

  /* Add a child span */
  span_t child;
  memset(&child, 0, sizeof(child));
  child.trace_id = 100;
  child.span_id = 2;
  child.parent_span_id = 1;
  snprintf(child.name, sizeof(child.name), "child");

  rc = partial_trace_add_span(pt, &child, true);
  mu_assert("add child should succeed", rc == 0);
  mu_assert_eq("span_count after child", 2UL, (unsigned long)pt->span_count);

  /* Verify the list is newest-first */
  mu_assert_eq("head is child", 2UL, (unsigned long)pt->spans->span_id);
  mu_assert_eq("second is root", 1UL,
               (unsigned long)pt->spans->next_sibling->span_id);

  partial_trace_destroy(pt);
  return NULL;
}

static const char *test_partial_trace_span_is_copied() {
  partial_trace_t *pt = partial_trace_create(200);

  span_t original;
  memset(&original, 0, sizeof(original));
  original.trace_id = 200;
  original.span_id = 10;
  snprintf(original.name, sizeof(original.name), "original");

  partial_trace_add_span(pt, &original, false);

  /* Modify original — should not affect the copy */
  snprintf(original.name, sizeof(original.name), "modified");

  mu_assert_str_eq("copy is independent", "original", pt->spans->name);

  partial_trace_destroy(pt);
  return NULL;
}

/* ============================================================
 * Trace Map Tests
 * ============================================================ */

static const char *test_trace_map_create_destroy() {
  trace_map_t *tm = trace_map_create(16);
  mu_assert("create should succeed", tm != NULL);
  mu_assert_eq("count initially 0", 0UL, (unsigned long)tm->count);
  trace_map_destroy(tm);
  return NULL;
}

static const char *test_trace_map_insert_and_find() {
  trace_map_t *tm = trace_map_create(16);

  span_t span;
  memset(&span, 0, sizeof(span));
  span.trace_id = 500;
  span.span_id = 1;
  snprintf(span.name, sizeof(span.name), "op1");

  int rc = trace_map_insert(tm, &span, true);
  mu_assert("insert should succeed", rc == 0);
  mu_assert_eq("count is 1", 1UL, (unsigned long)tm->count);

  partial_trace_t *pt = trace_map_find(tm, 500);
  mu_assert("find should succeed", pt != NULL);
  mu_assert_eq("trace_id matches", 500UL, (unsigned long)pt->trace_id);
  mu_assert_eq("span_count is 1", 1UL, (unsigned long)pt->span_count);

  /* Insert another span for the same trace */
  span.span_id = 2;
  span.parent_span_id = 1;
  snprintf(span.name, sizeof(span.name), "op2");
  rc = trace_map_insert(tm, &span, true);
  mu_assert("second insert should succeed", rc == 0);
  mu_assert_eq("count still 1 (same trace)", 1UL, (unsigned long)tm->count);

  pt = trace_map_find(tm, 500);
  mu_assert_eq("span_count is 2", 2UL, (unsigned long)pt->span_count);

  /* Insert span for a different trace */
  span.trace_id = 600;
  span.span_id = 10;
  span.parent_span_id = 0;
  rc = trace_map_insert(tm, &span, false);
  mu_assert("insert new trace should succeed", rc == 0);
  mu_assert_eq("count is 2", 2UL, (unsigned long)tm->count);

  mu_assert("find 600", trace_map_find(tm, 600) != NULL);
  mu_assert("find 500 still there", trace_map_find(tm, 500) != NULL);
  mu_assert("find 999 not found", trace_map_find(tm, 999) == NULL);

  trace_map_destroy(tm);
  return NULL;
}

static const char *test_trace_map_flush_timeout() {
  trace_map_t *tm = trace_map_create(16);

  /* Insert spans for two traces */
  span_t span;
  memset(&span, 0, sizeof(span));
  span.trace_id = 1;
  span.span_id = 1;
  span.parent_span_id = 0;
  trace_map_insert(tm, &span, true);

  span.trace_id = 2;
  span.span_id = 2;
  trace_map_insert(tm, &span, true);

  mu_assert_eq("count is 2", 2UL, (unsigned long)tm->count);

  /* Flush with timeout=0 should drain everything */
  partial_trace_t *out[10];
  int flushed = trace_map_flush(tm, out, 10, 0);
  mu_assert_eq("flush all with timeout=0", 2UL, (unsigned long)flushed);
  mu_assert_eq("count is 0 after flush", 0UL, (unsigned long)tm->count);

  /* Clean up flushed traces */
  for (int i = 0; i < flushed; i++) {
    partial_trace_destroy(out[i]);
  }

  trace_map_destroy(tm);
  return NULL;
}

static const char *test_trace_map_flush_respects_timeout() {
  trace_map_t *tm = trace_map_create(16);

  span_t span;
  memset(&span, 0, sizeof(span));
  span.trace_id = 1;
  span.span_id = 1;
  trace_map_insert(tm, &span, true);

  /* Flush with large timeout — nothing should be flushed since
     the trace was just inserted */
  partial_trace_t *out[10];
  int flushed = trace_map_flush(tm, out, 10, 60);
  mu_assert_eq("no flush with large timeout", 0UL, (unsigned long)flushed);
  mu_assert_eq("count still 1", 1UL, (unsigned long)tm->count);

  trace_map_destroy(tm);
  return NULL;
}

/* ============================================================
 * Out-of-order Assembly Tests
 * ============================================================ */

static const char *test_out_of_order_assembly() {
  trace_map_t *tm = trace_map_create(16);

  /* Insert child before root */
  span_t child;
  memset(&child, 0, sizeof(child));
  child.trace_id = 300;
  child.span_id = 2;
  child.parent_span_id = 1;
  snprintf(child.name, sizeof(child.name), "child_op");
  trace_map_insert(tm, &child, true);

  partial_trace_t *pt = trace_map_find(tm, 300);
  mu_assert("found after child", pt != NULL);
  mu_assert("no root yet", pt->has_root == false);

  /* Now insert root */
  span_t root;
  memset(&root, 0, sizeof(root));
  root.trace_id = 300;
  root.span_id = 1;
  root.parent_span_id = 0;
  snprintf(root.name, sizeof(root.name), "root_op");
  trace_map_insert(tm, &root, true);

  pt = trace_map_find(tm, 300);
  mu_assert("has_root after root arrives", pt->has_root == true);
  mu_assert_eq("span_count is 2", 2UL, (unsigned long)pt->span_count);

  trace_map_destroy(tm);
  return NULL;
}

/* ============================================================
 * Storage Tests
 * ============================================================ */

static const char *test_storage_write_and_readback() {
  const char *path = "/tmp/test_phase5_storage.bin";
  unlink(path); /* Remove if exists */

  trace_storage_t *ts = storage_open(path);
  mu_assert("storage open should succeed", ts != NULL);

  /* Build a partial trace with 2 spans */
  partial_trace_t *pt = partial_trace_create(777);

  span_t s1;
  memset(&s1, 0, sizeof(s1));
  s1.trace_id = 777;
  s1.span_id = 1;
  s1.parent_span_id = 0;
  s1.wall_start_us = 1000000;
  s1.monotonic_start_ns = 0;
  s1.monotonic_end_ns = 5000000; /* 5ms */
  snprintf(s1.name, sizeof(s1.name), "root");
  partial_trace_add_span(pt, &s1, true);

  span_t s2;
  memset(&s2, 0, sizeof(s2));
  s2.trace_id = 777;
  s2.span_id = 2;
  s2.parent_span_id = 1;
  s2.wall_start_us = 1001000;
  s2.monotonic_start_ns = 0;
  s2.monotonic_end_ns = 3000000; /* 3ms */
  snprintf(s2.name, sizeof(s2.name), "child");
  partial_trace_add_span(pt, &s2, true);

  int rc = storage_write_trace(ts, pt);
  mu_assert("write should succeed", rc == 0);
  rc = storage_flush(ts);
  mu_assert("flush should succeed", rc == 0);
  storage_close(ts);

  /* Read back and verify structure */
  FILE *fp = fopen(path, "rb");
  mu_assert("file should exist", fp != NULL);

  /* Read trace_id (8 bytes BE) */
  uint64_t tid_be;
  mu_assert_eq("read trace_id", 1UL, (unsigned long)fread(&tid_be, 8, 1, fp));

  /* Read num_spans (4 bytes BE) */
  uint32_t nspans_be;
  mu_assert_eq("read num_spans", 1UL,
               (unsigned long)fread(&nspans_be, 4, 1, fp));
  uint32_t nspans = ntohl(nspans_be);
  mu_assert_eq("num_spans is 2", 2UL, (unsigned long)nspans);

  /* Read each span: [4B len][N bytes wire data] */
  for (uint32_t i = 0; i < nspans; i++) {
    uint32_t slen_be;
    mu_assert_eq("read span len", 1UL,
                 (unsigned long)fread(&slen_be, 4, 1, fp));
    uint32_t slen = ntohl(slen_be);
    mu_assert("span len > 0", slen > 0);
    mu_assert("span len reasonable", slen <= SPAN_WIRE_MAX_SIZE);

    uint8_t wire[SPAN_WIRE_MAX_SIZE];
    mu_assert_eq("read span data", (unsigned long)slen,
                 (unsigned long)fread(wire, 1, slen, fp));

    /* Deserialize and verify */
    span_t decoded;
    bool sampled;
    int consumed = span_deserialize(wire, slen, &decoded, &sampled);
    mu_assert("deserialize should succeed", consumed > 0);
    mu_assert_eq("trace_id is 777", 777UL, (unsigned long)decoded.trace_id);
    mu_assert("sampled", sampled == true);
  }

  fclose(fp);
  unlink(path);
  partial_trace_destroy(pt);
  return NULL;
}

static const char *test_storage_null_args() {
  mu_assert("null path fails", storage_open(NULL) == NULL);

  trace_storage_t *ts = storage_open("/tmp/test_phase5_null.bin");
  mu_assert("open succeeds", ts != NULL);
  mu_assert("null trace fails", storage_write_trace(ts, NULL) < 0);
  storage_close(ts);
  unlink("/tmp/test_phase5_null.bin");
  return NULL;
}

/* ============================================================
 * Collector End-to-End Test
 * ============================================================ */

static const char *test_collector_end_to_end() {
  const char *storage_path = "/tmp/test_phase5_e2e.bin";
  unlink(storage_path);

  /* Use a high port to avoid conflicts */
  collector_config_t config = collector_default_config();
  config.port = 19831;
  config.timeout_sec = 1;
  config.flush_interval_sec = 1;
  config.storage_path = storage_path;

  collector_t *c = collector_create(&config);
  mu_assert("collector create should succeed", c != NULL);

  int rc = collector_start(c);
  mu_assert("collector start should succeed", rc == 0);

  /* Give collector time to bind */
  usleep(50000);

  /* Send spans via UDP */
  int sockfd = socket(AF_INET, SOCK_DGRAM, 0);
  mu_assert("socket should succeed", sockfd >= 0);

  struct sockaddr_in dest;
  memset(&dest, 0, sizeof(dest));
  dest.sin_family = AF_INET;
  dest.sin_port = htons(19831);
  dest.sin_addr.s_addr = htonl(0x7f000001); /* 127.0.0.1 */

  /* Send 3 spans for 1 trace */
  uint8_t buf[SPAN_WIRE_MAX_SIZE];
  int len;

  len = make_test_packet(buf, sizeof(buf), 8888, 1, 0, "root", true);
  mu_assert("serialize root", len > 0);
  sendto(sockfd, buf, (size_t)len, 0, (struct sockaddr *)&dest, sizeof(dest));

  len = make_test_packet(buf, sizeof(buf), 8888, 2, 1, "child1", true);
  mu_assert("serialize child1", len > 0);
  sendto(sockfd, buf, (size_t)len, 0, (struct sockaddr *)&dest, sizeof(dest));

  len = make_test_packet(buf, sizeof(buf), 8888, 3, 1, "child2", true);
  mu_assert("serialize child2", len > 0);
  sendto(sockfd, buf, (size_t)len, 0, (struct sockaddr *)&dest, sizeof(dest));

  close(sockfd);

  /* Wait for flush (timeout=1s + flush_interval=1s + margin) */
  sleep(3);

  /* Check stats */
  collector_stats_t stats;
  collector_get_stats(c, &stats);
  mu_assert("at least 3 packets received", stats.packets_received >= 3);
  mu_assert("at least 3 spans processed", stats.spans_processed >= 3);

  collector_stop(c);

  /* Verify stats after final drain */
  collector_get_stats(c, &stats);
  mu_assert("storage writes >= 1", stats.storage_writes >= 1);

  collector_destroy(c);

  /* Verify the storage file exists and has data */
  FILE *fp = fopen(storage_path, "rb");
  mu_assert("storage file should exist", fp != NULL);
  fseek(fp, 0, SEEK_END);
  long size = ftell(fp);
  mu_assert("storage file should have data", size > 0);
  fclose(fp);

  unlink(storage_path);
  return NULL;
}

/* ============================================================
 * Test Runner
 * ============================================================ */

static const char *all_tests() {
  /* Protocol decode */
  mu_run_test(test_decode_valid_packet);
  mu_run_test(test_decode_too_short);
  mu_run_test(test_decode_null_args);
  mu_run_test(test_decode_zero_trace_id);
  mu_run_test(test_decode_zero_span_id);

  /* Partial trace */
  mu_run_test(test_partial_trace_create_destroy);
  mu_run_test(test_partial_trace_add_spans);
  mu_run_test(test_partial_trace_span_is_copied);

  /* Trace map */
  mu_run_test(test_trace_map_create_destroy);
  mu_run_test(test_trace_map_insert_and_find);
  mu_run_test(test_trace_map_flush_timeout);
  mu_run_test(test_trace_map_flush_respects_timeout);

  /* Out-of-order assembly */
  mu_run_test(test_out_of_order_assembly);

  /* Storage */
  mu_run_test(test_storage_write_and_readback);
  mu_run_test(test_storage_null_args);

  /* End-to-end */
  mu_run_test(test_collector_end_to_end);

  return NULL;
}

int main(void) {
  printf("Phase 5: Central Collector Tests\n");
  printf("================================\n\n");
  all_tests();
  mu_report();
}
