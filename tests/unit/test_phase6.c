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

static const char *test_query_null_args() {
  int count;
  mu_assert("null path returns NULL", query_load_all(NULL, &count) == NULL);
  mu_assert("null count returns NULL",
            query_load_all("/nonexistent", NULL) == NULL);
  mu_assert("null path by_id returns NULL", query_trace_by_id(NULL, 1) == NULL);
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

static const char *test_critical_path_null() {
  int path_len;
  mu_assert("null trace returns NULL",
            compute_critical_path(NULL, &path_len) == NULL);
  mu_assert("path_len is 0", path_len == 0);
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

static const char *test_aggregate_null_args() {
  int count;
  mu_assert("null traces returns NULL",
            aggregate_by_service(NULL, 0, &count) == NULL);
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

  /* Read back and verify */
  fp = fopen(json_path, "r");
  mu_assert("json file exists", fp != NULL);
  fseek(fp, 0, SEEK_END);
  long size = ftell(fp);
  mu_assert("json file has content", size > 0);
  fclose(fp);

  trace_destroy(t);
  unlink(storage_path);
  unlink(json_path);
  return NULL;
}

static const char *test_json_export_null() {
  mu_assert("null trace returns NULL", export_trace_json_string(NULL) == NULL);
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
  mu_run_test(test_query_null_args);

  /* Critical path */
  mu_run_test(test_critical_path_simple);
  mu_run_test(test_critical_path_single_span);
  mu_run_test(test_critical_path_null);
  mu_run_test(test_critical_path_duration);

  /* Aggregation */
  mu_run_test(test_aggregate_single_trace);
  mu_run_test(test_aggregate_null_args);

  /* JSON export */
  mu_run_test(test_json_export_string);
  mu_run_test(test_json_export_file);
  mu_run_test(test_json_export_null);

  return NULL;
}

int main(void) {
  printf("Phase 6: Trace Reconstruction & Query Tests\n");
  printf("============================================\n\n");
  all_tests();
  mu_report();
}
