/**
 * test_phase6.c - Unit tests for Phase 6 (Trace Reconstruction & Query)
 *
 * Tests:
 * - Storage read-back and trace reconstruction
 * - Span hierarchy rebuild (parent/child/sibling pointers)
 * - Query by trace ID
 * - Query slowest traces
 * - Critical path computation
 * - Per-service aggregation
 * - JSON export
 */

#define _POSIX_C_SOURCE 200809L
#include "dapper/analysis.h"
#include "dapper/collector.h"
#include "dapper/exporter.h"
#include "dapper/span.h"
#include "dapper/trace.h"
#include "minunit.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int tests_run = 0;
int tests_failed = 0;

/* ============================================================
 * Helper: write a test storage file with known traces
 * ============================================================ */

/**
 * Create a storage file with a single trace containing:
 *   root (span_id=1, parent=0, duration=10ms, wall_start=1000000)
 *     child1 (span_id=2, parent=1, duration=6ms, wall_start=1001000)
 *     child2 (span_id=3, parent=1, duration=3ms, wall_start=1005000)
 */
static int write_test_storage_single(const char *path) {
  unlink(path);

  trace_storage_t *ts = storage_open(path);
  if (!ts) {
    return -1;
  }

  partial_trace_t *pt = partial_trace_create(1000);
  if (!pt) {
    storage_close(ts);
    return -1;
  }

  /* Root span */
  span_t root;
  memset(&root, 0, sizeof(root));
  root.trace_id = 1000;
  root.span_id = 1;
  root.parent_span_id = 0;
  root.wall_start_us = 1000000;
  root.monotonic_start_ns = 0;
  root.monotonic_end_ns = 10000000; /* 10ms = 10000us */
  snprintf(root.name, sizeof(root.name), "root_op");
  partial_trace_add_span(pt, &root, true);

  /* Child 1 (longer) */
  span_t c1;
  memset(&c1, 0, sizeof(c1));
  c1.trace_id = 1000;
  c1.span_id = 2;
  c1.parent_span_id = 1;
  c1.wall_start_us = 1001000;
  c1.monotonic_start_ns = 0;
  c1.monotonic_end_ns = 6000000; /* 6ms */
  snprintf(c1.name, sizeof(c1.name), "child_op_1");
  c1.annotation_count = 1;
  snprintf(c1.annotations[0].key, sizeof(c1.annotations[0].key), "db.system");
  snprintf(c1.annotations[0].value, sizeof(c1.annotations[0].value),
           "postgresql");
  partial_trace_add_span(pt, &c1, true);

  /* Child 2 (shorter) */
  span_t c2;
  memset(&c2, 0, sizeof(c2));
  c2.trace_id = 1000;
  c2.span_id = 3;
  c2.parent_span_id = 1;
  c2.wall_start_us = 1005000;
  c2.monotonic_start_ns = 0;
  c2.monotonic_end_ns = 3000000; /* 3ms */
  snprintf(c2.name, sizeof(c2.name), "child_op_2");
  partial_trace_add_span(pt, &c2, true);

  int rc = storage_write_trace(ts, pt);
  storage_flush(ts);
  storage_close(ts);
  partial_trace_destroy(pt);
  return rc;
}

/**
 * Create a storage file with multiple traces of varying duration.
 */
static int write_test_storage_multi(const char *path) {
  unlink(path);

  trace_storage_t *ts = storage_open(path);
  if (!ts) {
    return -1;
  }

  /* Trace A: 5ms */
  partial_trace_t *ptA = partial_trace_create(100);
  span_t sA;
  memset(&sA, 0, sizeof(sA));
  sA.trace_id = 100;
  sA.span_id = 1;
  sA.parent_span_id = 0;
  sA.wall_start_us = 2000000;
  sA.monotonic_start_ns = 0;
  sA.monotonic_end_ns = 5000000; /* 5ms */
  snprintf(sA.name, sizeof(sA.name), "fast_op");
  partial_trace_add_span(ptA, &sA, true);
  storage_write_trace(ts, ptA);
  partial_trace_destroy(ptA);

  /* Trace B: 20ms (slowest) */
  partial_trace_t *ptB = partial_trace_create(200);
  span_t sB;
  memset(&sB, 0, sizeof(sB));
  sB.trace_id = 200;
  sB.span_id = 1;
  sB.parent_span_id = 0;
  sB.wall_start_us = 3000000;
  sB.monotonic_start_ns = 0;
  sB.monotonic_end_ns = 20000000; /* 20ms */
  snprintf(sB.name, sizeof(sB.name), "slow_op");
  partial_trace_add_span(ptB, &sB, true);
  storage_write_trace(ts, ptB);
  partial_trace_destroy(ptB);

  /* Trace C: 10ms */
  partial_trace_t *ptC = partial_trace_create(300);
  span_t sC;
  memset(&sC, 0, sizeof(sC));
  sC.trace_id = 300;
  sC.span_id = 1;
  sC.parent_span_id = 0;
  sC.wall_start_us = 4000000;
  sC.monotonic_start_ns = 0;
  sC.monotonic_end_ns = 10000000; /* 10ms */
  snprintf(sC.name, sizeof(sC.name), "medium_op");
  partial_trace_add_span(ptC, &sC, true);
  storage_write_trace(ts, ptC);
  partial_trace_destroy(ptC);

  storage_flush(ts);
  storage_close(ts);
  return 0;
}

/* Append raw bytes to a file. */
static int append_bytes(const char *path, const void *data, size_t len) {
  FILE *fp = fopen(path, "ab");
  if (!fp) {
    return -1;
  }
  size_t n = fwrite(data, 1, len, fp);
  fclose(fp);
  return n == len ? 0 : -1;
}

/* Write a big-endian trace header (trace_id + num_spans) to a fresh file. */
static int write_trace_header(const char *path, uint64_t trace_id,
                              uint32_t num_spans) {
  unlink(path);
  uint8_t hdr[12];
  for (int i = 0; i < 8; i++) {
    hdr[i] = (uint8_t)(trace_id >> (56 - 8 * i));
  }
  for (int i = 0; i < 4; i++) {
    hdr[8 + i] = (uint8_t)(num_spans >> (24 - 8 * i));
  }
  return append_bytes(path, hdr, sizeof(hdr));
}

/* Write a storage file from caller-described spans (no collector
 * helpers), so tests can craft rootless / orphan / multi-root traces.
 * Each span is (span_id, parent_span_id, duration_ms). */
typedef struct {
  uint64_t span_id;
  uint64_t parent_span_id;
  uint64_t duration_ms;
} span_desc_t;

static int write_custom_trace(const char *path, uint64_t trace_id,
                              const span_desc_t *descs, int n) {
  unlink(path);
  trace_storage_t *ts = storage_open(path);
  if (!ts) {
    return -1;
  }
  partial_trace_t *pt = partial_trace_create(trace_id);
  for (int i = 0; i < n; i++) {
    span_t s;
    memset(&s, 0, sizeof(s));
    s.trace_id = trace_id;
    s.span_id = descs[i].span_id;
    s.parent_span_id = descs[i].parent_span_id;
    s.wall_start_us = 1000000 + i * 1000;
    s.monotonic_start_ns = 0;
    s.monotonic_end_ns = descs[i].duration_ms * 1000000ULL;
    snprintf(s.name, sizeof(s.name), "span_%llu",
             (unsigned long long)descs[i].span_id);
    partial_trace_add_span(pt, &s, true);
  }
  int rc = storage_write_trace(ts, pt);
  storage_flush(ts);
  storage_close(ts);
  partial_trace_destroy(pt);
  return rc;
}

/* ============================================================
 * Storage Reader / Query Tests
 * ============================================================ */

static const char *test_load_all_traces() {
  const char *path = "/tmp/test_phase6_load_all.bin";
  mu_assert("write test storage", write_test_storage_single(path) == 0);

  int count;
  trace_t **traces = query_load_all(path, &count);
  mu_assert("load_all should succeed", traces != NULL);
  mu_assert_eq("count is 1", 1UL, (unsigned long)count);
  mu_assert_eq("trace_id is 1000", 1000UL, (unsigned long)traces[0]->id);
  mu_assert("sampled", traces[0]->sampled == true);

  trace_destroy(traces[0]);
  free(traces);
  unlink(path);
  return NULL;
}

static const char *test_hierarchy_reconstruction() {
  const char *path = "/tmp/test_phase6_hierarchy.bin";
  mu_assert("write test storage", write_test_storage_single(path) == 0);

  int count;
  trace_t **traces = query_load_all(path, &count);
  mu_assert("load should succeed", traces != NULL);
  mu_assert_eq("count is 1", 1UL, (unsigned long)count);

  trace_t *t = traces[0];

  /* Verify root */
  mu_assert("root_span exists", t->root_span != NULL);
  mu_assert_str_eq("root name", "root_op", t->root_span->name);
  mu_assert_eq("root span_id", 1UL, (unsigned long)t->root_span->span_id);
  mu_assert_eq("root parent is 0", 0UL,
               (unsigned long)t->root_span->parent_span_id);
  mu_assert("root has no parent ptr", t->root_span->parent == NULL);

  /* Verify children */
  mu_assert("root has children", t->root_span->first_child != NULL);
  span_t *c1 = t->root_span->first_child;
  mu_assert("child1 parent ptr correct", c1->parent == t->root_span);

  span_t *c2 = c1->next_sibling;
  mu_assert("child2 exists", c2 != NULL);
  mu_assert("child2 parent ptr correct", c2->parent == t->root_span);
  mu_assert("child2 has no more siblings", c2->next_sibling == NULL);

  /* Verify child names (order may depend on storage linked list order) */
  /* partial_trace stores newest first, so c2 was added last → stored first */
  /* But storage iterates via next_sibling, so order depends on reconstruction
   */

  trace_destroy(t);
  free(traces);
  unlink(path);
  return NULL;
}

static const char *test_query_by_id_found() {
  const char *path = "/tmp/test_phase6_by_id.bin";
  mu_assert("write test storage", write_test_storage_multi(path) == 0);

  trace_t *t = query_trace_by_id(path, 200);
  mu_assert("should find trace 200", t != NULL);
  mu_assert_eq("trace_id is 200", 200UL, (unsigned long)t->id);
  mu_assert("root exists", t->root_span != NULL);
  mu_assert_str_eq("root name", "slow_op", t->root_span->name);

  trace_destroy(t);
  unlink(path);
  return NULL;
}

static const char *test_query_by_id_not_found() {
  const char *path = "/tmp/test_phase6_by_id_nf.bin";
  mu_assert("write test storage", write_test_storage_multi(path) == 0);

  trace_t *t = query_trace_by_id(path, 999);
  mu_assert("should not find trace 999", t == NULL);

  unlink(path);
  return NULL;
}

static const char *test_query_slowest() {
  const char *path = "/tmp/test_phase6_slowest.bin";
  mu_assert("write test storage", write_test_storage_multi(path) == 0);

  int count;
  trace_t **slowest = query_slowest_traces(path, 2, &count);
  mu_assert("slowest should succeed", slowest != NULL);
  mu_assert_eq("count is 2", 2UL, (unsigned long)count);

  /* First should be trace 200 (20ms), second trace 300 (10ms) */
  mu_assert_eq("first is 200", 200UL, (unsigned long)slowest[0]->id);
  mu_assert_eq("second is 300", 300UL, (unsigned long)slowest[1]->id);

  for (int i = 0; i < count; i++) {
    trace_destroy(slowest[i]);
  }
  free(slowest);
  unlink(path);
  return NULL;
}

static const char *test_query_slowest_nonpositive_limit() {
  const char *path = "/tmp/test_phase6_slowest_limit.bin";
  mu_assert("write test storage", write_test_storage_multi(path) == 0);

  int count = -1;
  trace_t **result = query_slowest_traces(path, 0, &count);
  mu_assert("limit == 0 returns NULL", result == NULL);
  mu_assert_eq("limit == 0 yields count 0", 0UL, (unsigned long)count);

  count = -1;
  result = query_slowest_traces(path, -5, &count);
  mu_assert("limit < 0 returns NULL", result == NULL);
  mu_assert_eq("limit < 0 yields count 0", 0UL, (unsigned long)count);

  /* Storage must remain readable after the rejected queries */
  count = 0;
  result = query_slowest_traces(path, 1, &count);
  mu_assert("limit == 1 succeeds", result != NULL);
  mu_assert_eq("limit == 1 yields count 1", 1UL, (unsigned long)count);
  mu_assert_eq("slowest is trace 200", 200UL, (unsigned long)result[0]->id);
  trace_destroy(result[0]);
  free(result);

  unlink(path);
  return NULL;
}

/* ============================================================
 * Reconstructed Trace Ownership Tests (B5)
 *
 * These exercise loads that leave spans unreachable from root so
 * that, under a leak sanitizer, trace_destroy() must still free every
 * span. They run clean under -fsanitize=address,leak.
 * ============================================================ */

/* count spans reachable from root via the hierarchy */
static int count_hierarchy(const span_t *span) {
  if (!span) {
    return 0;
  }
  int n = 1;
  for (const span_t *c = span->first_child; c; c = c->next_sibling) {
    n += count_hierarchy(c);
  }
  return n;
}

/* count spans owned by the trace via the ownership chain */
static int count_owned(const trace_t *t) {
  int n = 0;
  for (const span_t *s = t->all_spans; s; s = s->owner_next) {
    n++;
  }
  return n;
}

static const char *test_reconstruct_orphan_spans_owned() {
  /* span 3's parent (99) does not exist -> orphan, unreachable from
   * root but still owned by the trace. */
  const char *path = "/tmp/test_phase6_orphan.bin";
  span_desc_t descs[] = {
      {1, 0, 10}, /* root */
      {2, 1, 5},  /* child of root */
      {3, 99, 4}, /* orphan: parent missing */
  };
  mu_assert("write custom trace", write_custom_trace(path, 555, descs, 3) == 0);

  trace_t *t = query_trace_by_id(path, 555);
  mu_assert("trace loaded", t != NULL);
  mu_assert("has root", t->root_span != NULL);
  mu_assert_eq("all 3 spans owned", 3, count_owned(t));
  mu_assert_eq("only 2 reachable from root", 2, count_hierarchy(t->root_span));

  trace_destroy(t); /* must free all 3 spans, not just the reachable 2 */
  unlink(path);
  return NULL;
}

static const char *test_reconstruct_rootless_trace_owned() {
  /* No span has parent_span_id 0 -> no root, but spans still owned. */
  const char *path = "/tmp/test_phase6_rootless.bin";
  span_desc_t descs[] = {
      {1, 7, 10},
      {2, 7, 5},
  };
  mu_assert("write custom trace", write_custom_trace(path, 556, descs, 2) == 0);

  trace_t *t = query_trace_by_id(path, 556);
  mu_assert("trace loaded", t != NULL);
  mu_assert("no root span", t->root_span == NULL);
  mu_assert_eq("both spans owned", 2, count_owned(t));

  trace_destroy(t); /* must free both despite NULL root */
  unlink(path);
  return NULL;
}

static const char *test_reconstruct_multiple_roots_owned() {
  /* Two spans claim root: first kept as root_span, both owned. */
  const char *path = "/tmp/test_phase6_multiroot.bin";
  span_desc_t descs[] = {
      {1, 0, 10}, /* root A */
      {2, 0, 8},  /* root B */
      {3, 1, 4},  /* child of A */
  };
  mu_assert("write custom trace", write_custom_trace(path, 557, descs, 3) == 0);

  trace_t *t = query_trace_by_id(path, 557);
  mu_assert("trace loaded", t != NULL);
  mu_assert("has a root", t->root_span != NULL);
  mu_assert("root_span is an actual root", t->root_span->parent_span_id == 0);
  mu_assert_eq("all 3 spans owned", 3, count_owned(t));

  trace_destroy(t); /* must free the second root too */
  unlink(path);
  return NULL;
}

/* ============================================================
 * Corrupt / Truncated Storage Tests (B4)
 * ============================================================ */

static const char *test_load_clean_reports_eof() {
  const char *path = "/tmp/test_phase6_clean_eof.bin";
  mu_assert("write test storage", write_test_storage_multi(path) == 0);

  int count = 0;
  trace_read_status_t status = TRACE_READ_OK;
  trace_t **traces = query_load_all_status(path, &count, &status);
  mu_assert("clean load succeeds", traces != NULL);
  mu_assert_eq("loaded 3 traces", 3UL, (unsigned long)count);
  mu_assert("status is EOF", status == TRACE_READ_EOF);

  for (int i = 0; i < count; i++) {
    trace_destroy(traces[i]);
  }
  free(traces);
  unlink(path);
  return NULL;
}

static const char *test_load_truncated_header() {
  /* trace_id present but num_spans missing -> corrupt, not clean EOF. */
  const char *path = "/tmp/test_phase6_trunc_hdr.bin";
  unlink(path);
  uint8_t tid[8] = {0, 0, 0, 0, 0, 0, 0, 5};
  mu_assert("write partial header", append_bytes(path, tid, sizeof(tid)) == 0);

  int count = -1;
  trace_read_status_t status = TRACE_READ_OK;
  trace_t **traces = query_load_all_status(path, &count, &status);
  mu_assert("no traces loaded", traces == NULL);
  mu_assert_eq("count is 0", 0UL, (unsigned long)count);
  mu_assert("status is corrupt", status == TRACE_READ_CORRUPT);

  unlink(path);
  return NULL;
}

static const char *test_load_zero_spans() {
  const char *path = "/tmp/test_phase6_zero_spans.bin";
  mu_assert("write zero-span header", write_trace_header(path, 7, 0) == 0);

  int count = -1;
  trace_read_status_t status = TRACE_READ_OK;
  trace_t **traces = query_load_all_status(path, &count, &status);
  mu_assert("no traces loaded", traces == NULL);
  mu_assert("status is corrupt", status == TRACE_READ_CORRUPT);

  unlink(path);
  return NULL;
}

static const char *test_load_overlarge_num_spans() {
  /* num_spans beyond STORAGE_MAX_SPANS_PER_TRACE must not allocate. */
  const char *path = "/tmp/test_phase6_huge_spans.bin";
  mu_assert("write huge-span header",
            write_trace_header(path, 7, 0xFFFFFFFFu) == 0);

  int count = -1;
  trace_read_status_t status = TRACE_READ_OK;
  trace_t **traces = query_load_all_status(path, &count, &status);
  mu_assert("no traces loaded", traces == NULL);
  mu_assert("status is corrupt", status == TRACE_READ_CORRUPT);

  unlink(path);
  return NULL;
}

static const char *test_load_overlarge_span_len() {
  /* Valid header claiming 1 span, then an absurd span length prefix. */
  const char *path = "/tmp/test_phase6_huge_slen.bin";
  mu_assert("write header", write_trace_header(path, 7, 1) == 0);
  uint8_t slen[4] = {0xFF, 0xFF, 0xFF, 0xFF};
  mu_assert("write bad span len", append_bytes(path, slen, sizeof(slen)) == 0);

  int count = -1;
  trace_read_status_t status = TRACE_READ_OK;
  trace_t **traces = query_load_all_status(path, &count, &status);
  mu_assert("no traces loaded", traces == NULL);
  mu_assert("status is corrupt", status == TRACE_READ_CORRUPT);

  unlink(path);
  return NULL;
}

static const char *test_load_truncated_payload() {
  /* Span length says 48 bytes but only 10 are present. */
  const char *path = "/tmp/test_phase6_trunc_payload.bin";
  mu_assert("write header", write_trace_header(path, 7, 1) == 0);
  uint8_t slen[4] = {0, 0, 0, 48};
  mu_assert("write span len", append_bytes(path, slen, sizeof(slen)) == 0);
  uint8_t partial[10] = {0};
  mu_assert("write partial payload",
            append_bytes(path, partial, sizeof(partial)) == 0);

  int count = -1;
  trace_read_status_t status = TRACE_READ_OK;
  trace_t **traces = query_load_all_status(path, &count, &status);
  mu_assert("no traces loaded", traces == NULL);
  mu_assert("status is corrupt", status == TRACE_READ_CORRUPT);

  unlink(path);
  return NULL;
}

static const char *test_load_malformed_inner_span() {
  /* Valid framing (1 span, 48-byte payload) but the span header claims
   * a name longer than the payload, so span_deserialize() rejects it. */
  const char *path = "/tmp/test_phase6_bad_span.bin";
  mu_assert("write header", write_trace_header(path, 7, 1) == 0);
  uint8_t slen[4] = {0, 0, 0, 48}; /* header-only span payload */
  mu_assert("write span len", append_bytes(path, slen, sizeof(slen)) == 0);

  uint8_t span_hdr[48] = {0};
  span_hdr[0] = 0;  /* trace_id high bytes... */
  span_hdr[7] = 7;  /* trace_id = 7 */
  span_hdr[15] = 1; /* span_id = 1 */
  /* name_len at offset 42 (big-endian) = 100, exceeds the 48-byte payload */
  span_hdr[42] = 0;
  span_hdr[43] = 100;
  mu_assert("write span header",
            append_bytes(path, span_hdr, sizeof(span_hdr)) == 0);

  int count = -1;
  trace_read_status_t status = TRACE_READ_OK;
  trace_t **traces = query_load_all_status(path, &count, &status);
  mu_assert("no traces loaded", traces == NULL);
  mu_assert("status is corrupt", status == TRACE_READ_CORRUPT);

  unlink(path);
  return NULL;
}

static const char *test_load_valid_prefix_then_corrupt() {
  /* A valid trace followed by a truncated record: the valid prefix is
   * returned and corruption is reported rather than silently dropped. */
  const char *path = "/tmp/test_phase6_prefix_corrupt.bin";
  mu_assert("write test storage", write_test_storage_single(path) == 0);
  uint8_t tid[8] = {0, 0, 0, 0, 0, 0, 0, 9};
  mu_assert("append partial record", append_bytes(path, tid, sizeof(tid)) == 0);

  int count = 0;
  trace_read_status_t status = TRACE_READ_OK;
  trace_t **traces = query_load_all_status(path, &count, &status);
  mu_assert("valid prefix returned", traces != NULL);
  mu_assert_eq("one valid trace", 1UL, (unsigned long)count);
  mu_assert_eq("trace id is 1000", 1000UL, (unsigned long)traces[0]->id);
  mu_assert("status is corrupt", status == TRACE_READ_CORRUPT);

  for (int i = 0; i < count; i++) {
    trace_destroy(traces[i]);
  }
  free(traces);
  unlink(path);
  return NULL;
}

/* ============================================================
 * Critical Path Tests
 * ============================================================ */

static const char *test_critical_path_simple() {
  const char *path = "/tmp/test_phase6_cpath.bin";
  mu_assert("write test storage", write_test_storage_single(path) == 0);

  trace_t *t = query_trace_by_id(path, 1000);
  mu_assert("load trace", t != NULL);

  int path_len;
  span_t **cpath = compute_critical_path(t, &path_len);
  mu_assert("critical path should succeed", cpath != NULL);
  mu_assert_eq("path length is 2", 2UL, (unsigned long)path_len);

  /* Root should be first */
  mu_assert_eq("first is root", 1UL, (unsigned long)cpath[0]->span_id);
  /* Critical child is the one with longer duration (child1 = 6ms > child2 =
   * 3ms) */
  mu_assert_eq("second is longer child", 2UL, (unsigned long)cpath[1]->span_id);

  free(cpath);
  trace_destroy(t);
  unlink(path);
  return NULL;
}

static const char *test_critical_path_single_span() {
  const char *path = "/tmp/test_phase6_cpath_single.bin";
  mu_assert("write test storage", write_test_storage_multi(path) == 0);

  /* Trace 100 has a single root span */
  trace_t *t = query_trace_by_id(path, 100);
  mu_assert("load trace", t != NULL);

  int path_len;
  span_t **cpath = compute_critical_path(t, &path_len);
  mu_assert("critical path should succeed", cpath != NULL);
  mu_assert_eq("path length is 1", 1UL, (unsigned long)path_len);
  mu_assert_eq("only span is root", 1UL, (unsigned long)cpath[0]->span_id);

  free(cpath);
  trace_destroy(t);
  unlink(path);
  return NULL;
}

static const char *test_critical_path_duration() {
  const char *path = "/tmp/test_phase6_cpath_dur.bin";
  mu_assert("write test storage", write_test_storage_single(path) == 0);

  trace_t *t = query_trace_by_id(path, 1000);
  mu_assert("load trace", t != NULL);

  uint64_t dur = critical_path_duration_us(t);
  mu_assert_eq("duration is 10000us (10ms)", 10000UL, (unsigned long)dur);

  trace_destroy(t);
  unlink(path);
  return NULL;
}

/* ============================================================
 * Aggregation Tests
 * ============================================================ */

static const char *test_aggregate_single_trace() {
  const char *path = "/tmp/test_phase6_agg.bin";
  mu_assert("write test storage", write_test_storage_single(path) == 0);

  int trace_count;
  trace_t **traces = query_load_all(path, &trace_count);
  mu_assert("load traces", traces != NULL);

  int stat_count;
  service_stats_t *stats =
      aggregate_by_service(traces, trace_count, &stat_count);
  mu_assert("aggregate should succeed", stats != NULL);
  mu_assert("at least 1 group", stat_count >= 1);

  /* Find the root_op stats */
  int found_root = 0;
  for (int i = 0; i < stat_count; i++) {
    if (strcmp(stats[i].name, "root_op") == 0) {
      found_root = 1;
      mu_assert_eq("root_op count is 1", 1UL, (unsigned long)stats[i].count);
      mu_assert("root_op mean ~10000us", stats[i].mean_latency_us > 9000 &&
                                             stats[i].mean_latency_us < 11000);
    }
  }
  mu_assert("found root_op in stats", found_root);

  free(stats);
  for (int i = 0; i < trace_count; i++) {
    trace_destroy(traces[i]);
  }
  free(traces);
  unlink(path);
  return NULL;
}

/* D3: aggregation must compute exact count/min/max/mean/p50/p99 for a
 * known set of durations grouped by span name. */
static const char *test_aggregate_exact_stats() {
  /* One root "svc" with four children "leaf" of 10/20/30/40 ms. */
  const char *path = "/tmp/test_phase6_agg_exact.bin";
  unlink(path);
  trace_storage_t *ts = storage_open(path);
  mu_assert("open storage", ts != NULL);
  partial_trace_t *pt = partial_trace_create(900);

  span_t root;
  memset(&root, 0, sizeof(root));
  root.trace_id = 900;
  root.span_id = 1;
  root.parent_span_id = 0;
  root.monotonic_end_ns = 100000000ULL; /* 100ms */
  snprintf(root.name, sizeof(root.name), "svc");
  partial_trace_add_span(pt, &root, true);

  const uint64_t ms[4] = {10, 20, 30, 40};
  for (int i = 0; i < 4; i++) {
    span_t leaf;
    memset(&leaf, 0, sizeof(leaf));
    leaf.trace_id = 900;
    leaf.span_id = (span_id_t)(2 + i);
    leaf.parent_span_id = 1;
    leaf.monotonic_end_ns = ms[i] * 1000000ULL;
    snprintf(leaf.name, sizeof(leaf.name), "leaf");
    partial_trace_add_span(pt, &leaf, true);
  }
  mu_assert("write trace", storage_write_trace(ts, pt) == 0);
  storage_flush(ts);
  storage_close(ts);
  partial_trace_destroy(pt);

  int trace_count;
  trace_t **traces = query_load_all(path, &trace_count);
  mu_assert("load traces", traces != NULL);

  int stat_count;
  service_stats_t *stats =
      aggregate_by_service(traces, trace_count, &stat_count);
  mu_assert("aggregate ok", stats != NULL);
  mu_assert_eq("two groups", 2UL, (unsigned long)stat_count);

  const service_stats_t *leaf = NULL;
  for (int i = 0; i < stat_count; i++) {
    if (strcmp(stats[i].name, "leaf") == 0) {
      leaf = &stats[i];
    }
  }
  mu_assert("leaf group present", leaf != NULL);
  mu_assert_eq("leaf count is 4", 4UL, (unsigned long)leaf->count);
  mu_assert("leaf min 10ms", leaf->min_latency_us == 10000.0);
  mu_assert("leaf max 40ms", leaf->max_latency_us == 40000.0);
  mu_assert("leaf mean 25ms", leaf->mean_latency_us == 25000.0);
  /* Linearly-interpolated percentiles over sorted [10,20,30,40] ms:
   * p50 rank=1.5 -> 25000; p99 rank=2.97 -> 30000 + 0.97*10000. */
  mu_assert("leaf p50 interpolated", leaf->p50_latency_us == 25000.0);
  mu_assert("leaf p99 interpolated",
            leaf->p99_latency_us > 39600.0 && leaf->p99_latency_us < 39800.0);

  free(stats);
  for (int i = 0; i < trace_count; i++) {
    trace_destroy(traces[i]);
  }
  free(traces);
  unlink(path);
  return NULL;
}

/* ============================================================
 * JSON Export Tests
 * ============================================================ */

static const char *test_json_export_string() {
  const char *path = "/tmp/test_phase6_json.bin";
  mu_assert("write test storage", write_test_storage_single(path) == 0);

  trace_t *t = query_trace_by_id(path, 1000);
  mu_assert("load trace", t != NULL);

  char *json = export_trace_json_string(t);
  mu_assert("json export should succeed", json != NULL);

  /* Check key fields are present */
  mu_assert("contains trace_id", strstr(json, "\"trace_id\"") != NULL);
  mu_assert("contains 1000", strstr(json, "1000") != NULL);
  mu_assert("contains spans", strstr(json, "\"spans\"") != NULL);
  mu_assert("contains root_op", strstr(json, "root_op") != NULL);
  mu_assert("contains critical_path",
            strstr(json, "\"critical_path\"") != NULL);
  mu_assert("contains sampled true", strstr(json, "\"sampled\": true") != NULL);
  mu_assert("contains duration_us", strstr(json, "\"duration_us\"") != NULL);

  /* Check annotation present */
  mu_assert("contains db.system", strstr(json, "db.system") != NULL);
  mu_assert("contains postgresql", strstr(json, "postgresql") != NULL);

  free(json);
  trace_destroy(t);
  unlink(path);
  return NULL;
}

/* D3: the JSON writer must escape quotes, backslashes, and control
 * characters in names and annotation values. */
static const char *test_json_escapes_special_chars() {
  trace_t *t = trace_create_with_id(77);
  t->sampled = true;
  span_t *root = span_create(t, NULL, "weird\"name");
  span_annotate(root, "k\\path", "a\"b\nc\td");
  span_finish(root);

  char *json = export_trace_json_string(t);
  mu_assert("json export ok", json != NULL);

  mu_assert("escaped quote in name", strstr(json, "weird\\\"name") != NULL);
  mu_assert("escaped backslash in key", strstr(json, "k\\\\path") != NULL);
  /* The full value "a\"b\nc\td" must appear with every special char
   * escaped (real quote/newline/tab never inside the value). */
  mu_assert("value fully escaped", strstr(json, "a\\\"b\\nc\\td") != NULL);

  free(json);
  trace_destroy(t);
  return NULL;
}

static const char *test_json_export_file() {
  const char *storage_path = "/tmp/test_phase6_json_file.bin";
  const char *json_path = "/tmp/test_phase6_output.json";
  mu_assert("write test storage", write_test_storage_single(storage_path) == 0);

  trace_t *t = query_trace_by_id(storage_path, 1000);
  mu_assert("load trace", t != NULL);

  FILE *fp = fopen(json_path, "w");
  mu_assert("open json file", fp != NULL);
  export_trace_json(t, fp);
  fclose(fp);

  /* Read the whole file and assert it contains the expected content,
   * not merely that it is non-empty. The file form must match the
   * string form for the same trace. */
  fp = fopen(json_path, "r");
  mu_assert("json file exists", fp != NULL);
  char buf[8192];
  size_t n = fread(buf, 1, sizeof(buf) - 1, fp);
  fclose(fp);
  buf[n] = '\0';

  mu_assert("file has trace_id field", strstr(buf, "\"trace_id\"") != NULL);
  mu_assert("file has the trace id value", strstr(buf, "1000") != NULL);
  mu_assert("file has root span name", strstr(buf, "root_op") != NULL);
  mu_assert("file has child span name", strstr(buf, "child_op_1") != NULL);
  mu_assert("file has annotation key", strstr(buf, "db.system") != NULL);
  mu_assert("file has annotation value", strstr(buf, "postgresql") != NULL);
  mu_assert("file has critical_path", strstr(buf, "\"critical_path\"") != NULL);
  mu_assert("file marks sampled true",
            strstr(buf, "\"sampled\": true") != NULL);

  trace_destroy(t);
  unlink(storage_path);
  unlink(json_path);
  return NULL;
}

/* E1: consolidated NULL/argument guards for the analysis API. */
static const char *test_analysis_null_guards() {
  int n = -1;

  /* query */
  mu_assert("query_load_all null path", query_load_all(NULL, &n) == NULL);
  mu_assert("query_load_all null count",
            query_load_all("/nonexistent", NULL) == NULL);
  mu_assert("query_trace_by_id null path", query_trace_by_id(NULL, 1) == NULL);

  /* critical path */
  int path_len = -1;
  mu_assert("critical_path null trace",
            compute_critical_path(NULL, &path_len) == NULL);
  mu_assert("critical_path len reset", path_len == 0);

  /* aggregation */
  mu_assert("aggregate null traces", aggregate_by_service(NULL, 0, &n) == NULL);

  /* JSON export */
  mu_assert("json string null trace", export_trace_json_string(NULL) == NULL);
  export_trace_json(NULL, stdout); /* must not crash */
  return NULL;
}

/* ============================================================
 * Full Pipeline Integration Test (D1)
 *
 * exporter -> UDP -> collector -> storage -> query, end to end on an
 * ephemeral port and a unique temp file. Asserts trace ID, hierarchy,
 * sampled state, annotations, and names survive the whole round trip.
 * ============================================================ */

static bool int_pred_two_spans(const collector_stats_t *s) {
  return s->spans_processed >= 2;
}

static int wait_collector(collector_t *c,
                          bool (*pred)(const collector_stats_t *),
                          int timeout_ms) {
  collector_stats_t st;
  for (int waited = 0; waited < timeout_ms; waited += 10) {
    collector_get_stats(c, &st);
    if (pred(&st)) {
      return 0;
    }
    usleep(10000);
  }
  collector_get_stats(c, &st);
  return pred(&st) ? 0 : -1;
}

static const char *test_full_pipeline_integration() {
  char storage_path[] = "/tmp/test_phase6_pipeline_XXXXXX";
  int fd = mkstemp(storage_path);
  mu_assert("mkstemp", fd >= 0);
  close(fd);
  unlink(storage_path); /* collector will recreate it */

  /* Collector on an ephemeral loopback port. */
  collector_config_t config = collector_default_config();
  config.port = 0; /* OS-assigned */
  config.timeout_sec = 1;
  config.flush_interval_sec = 1;
  config.storage_path = storage_path;

  collector_t *c = collector_create(&config);
  mu_assert("collector create", c != NULL);
  mu_assert("collector start", collector_start(c) == 0);

  int port = collector_port(c);
  mu_assert("ephemeral port assigned", port > 0);

  usleep(50000); /* let the receiver settle */

  /* Real UDP exporter pointed at the collector. */
  exporter_t *exp = exporter_create_udp("127.0.0.1", port);
  mu_assert("exporter create", exp != NULL);
  mu_assert("exporter start", exporter_start(exp) == 0);

  /* A sampled trace: root + annotated child. */
  const trace_id_t kTraceId = 0xC0FFEEULL;
  trace_t *trace = trace_create_with_id(kTraceId);
  trace->sampled = true;
  span_t *root = span_create(trace, NULL, "frontend");
  span_t *child = span_create(trace, root, "db_query");
  span_annotate(child, "db.system", "postgresql");
  span_finish(child);
  span_finish(root);
  exporter_submit(exp, root);
  exporter_submit(exp, child);

  /* Wait until both spans are ingested, then flush and stop. */
  mu_assert("spans ingested", wait_collector(c, int_pred_two_spans, 3000) == 0);

  exporter_destroy(exp);
  collector_stop(c);
  collector_destroy(c);

  /* Query the storage and assert the full reconstruction. */
  trace_t *loaded = query_trace_by_id(storage_path, kTraceId);
  mu_assert("trace loaded from storage", loaded != NULL);
  mu_assert_eq("trace id round-trips", (unsigned long)kTraceId,
               (unsigned long)loaded->id);
  mu_assert("sampled state round-trips", loaded->sampled == true);
  mu_assert("root reconstructed", loaded->root_span != NULL);
  mu_assert_str_eq("root name", "frontend", loaded->root_span->name);
  mu_assert("root has one child", loaded->root_span->first_child != NULL);

  span_t *lc = loaded->root_span->first_child;
  mu_assert_str_eq("child name", "db_query", lc->name);
  mu_assert_eq("child parent linkage",
               (unsigned long)loaded->root_span->span_id,
               (unsigned long)lc->parent_span_id);
  mu_assert_eq("child annotation count", 1UL,
               (unsigned long)lc->annotation_count);
  mu_assert_str_eq("annotation key", "db.system", lc->annotations[0].key);
  mu_assert_str_eq("annotation value", "postgresql", lc->annotations[0].value);

  trace_destroy(loaded);
  trace_destroy(trace);
  unlink(storage_path);
  return NULL;
}

/* ============================================================
 * Test Runner
 * ============================================================ */

static const char *all_tests() {
  /* Storage reader / query */
  mu_run_test(test_load_all_traces);
  mu_run_test(test_hierarchy_reconstruction);
  mu_run_test(test_query_by_id_found);
  mu_run_test(test_query_by_id_not_found);
  mu_run_test(test_query_slowest);
  mu_run_test(test_query_slowest_nonpositive_limit);

  /* Reconstructed trace ownership */
  mu_run_test(test_reconstruct_orphan_spans_owned);
  mu_run_test(test_reconstruct_rootless_trace_owned);
  mu_run_test(test_reconstruct_multiple_roots_owned);

  /* Corrupt / truncated storage */
  mu_run_test(test_load_clean_reports_eof);
  mu_run_test(test_load_truncated_header);
  mu_run_test(test_load_zero_spans);
  mu_run_test(test_load_overlarge_num_spans);
  mu_run_test(test_load_overlarge_span_len);
  mu_run_test(test_load_truncated_payload);
  mu_run_test(test_load_malformed_inner_span);
  mu_run_test(test_load_valid_prefix_then_corrupt);

  /* Critical path */
  mu_run_test(test_critical_path_simple);
  mu_run_test(test_critical_path_single_span);
  mu_run_test(test_critical_path_duration);

  /* Aggregation */
  mu_run_test(test_aggregate_single_trace);
  mu_run_test(test_aggregate_exact_stats);

  /* JSON export */
  mu_run_test(test_json_export_string);
  mu_run_test(test_json_escapes_special_chars);
  mu_run_test(test_json_export_file);

  /* Consolidated NULL/argument guards */
  mu_run_test(test_analysis_null_guards);

  /* Full pipeline integration */
  mu_run_test(test_full_pipeline_integration);

  return NULL;
}

int main(void) {
  printf("Phase 6: Trace Reconstruction & Query Tests\n");
  printf("============================================\n\n");
  all_tests();
  mu_report();
}
