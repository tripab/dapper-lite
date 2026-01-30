/**
 * test_phase1.c - Unit tests for Phase 1 functionality
 *
 * Tests semantic correctness:
 * - Trace creation and destruction
 * - Span lifecycle
 * - Parent-child hierarchy
 * - Timestamp ordering
 * - Annotation storage
 */

#include "dapper/span.h"
#include "dapper/trace.h"
#include "minunit.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>

int tests_run = 0;
int tests_failed = 0;

/* ========== Trace Tests ========== */

static const char *test_trace_create_destroy() {
  trace_t *trace = trace_create();
  mu_assert("trace_create should return non-NULL", trace != NULL);
  mu_assert("trace should have non-zero ID", trace->id != 0);
  mu_assert("new trace should have NULL root_span", trace->root_span == NULL);

  trace_destroy(trace);
  return NULL;
}

static const char *test_trace_with_id() {
  trace_id_t custom_id = 0x123456789ABCDEF0ULL;
  trace_t *trace = trace_create_with_id(custom_id);

  mu_assert("trace_create_with_id should return non-NULL", trace != NULL);
  mu_assert_eq("trace should have custom ID", custom_id, trace->id);

  trace_destroy(trace);
  return NULL;
}

static const char *test_trace_destroy_null() {
  /* Should not crash */
  trace_destroy(NULL);
  return NULL;
}

/* ========== Span Tests ========== */

static const char *test_span_create_finish() {
  trace_t *trace = trace_create();
  span_t *span = span_create(trace, NULL, "test_span");

  mu_assert("span_create should return non-NULL", span != NULL);
  mu_assert("span should have non-zero ID", span->span_id != 0);
  mu_assert_eq("span should have trace's ID", trace->id, span->trace_id);
  mu_assert_eq("root span should have parent_span_id = 0", 0,
               span->parent_span_id);
  mu_assert_str_eq("span should have correct name", "test_span", span->name);
  mu_assert("span should have non-zero start time",
            span->monotonic_start_ns != 0);
  mu_assert_eq("unfinished span should have end time = 0", 0,
               span->monotonic_end_ns);

  span_finish(span);

  mu_assert("finished span should have non-zero end time",
            span->monotonic_end_ns != 0);
  mu_assert("end time should be >= start time",
            span->monotonic_end_ns >= span->monotonic_start_ns);

  trace_destroy(trace);
  return NULL;
}

static const char *test_span_name_truncation() {
  trace_t *trace = trace_create();
  char long_name[256];
  memset(long_name, 'A', sizeof(long_name) - 1);
  long_name[sizeof(long_name) - 1] = '\0';

  span_t *span = span_create(trace, NULL, long_name);

  mu_assert("span name should be truncated",
            strlen(span->name) < strlen(long_name));
  mu_assert("span name should be null-terminated",
            span->name[SPAN_NAME_MAX_LENGTH - 1] == '\0');

  trace_destroy(trace);
  return NULL;
}

static const char *test_span_finish_idempotent() {
  trace_t *trace = trace_create();
  span_t *span = span_create(trace, NULL, "test");

  span_finish(span);
  uint64_t first_end = span->monotonic_end_ns;

  usleep(1000); /* Wait a bit */
  span_finish(span);
  uint64_t second_end = span->monotonic_end_ns;

  mu_assert_eq("finish should be idempotent", first_end, second_end);

  trace_destroy(trace);
  return NULL;
}

static const char *test_span_duration() {
  trace_t *trace = trace_create();
  span_t *span = span_create(trace, NULL, "test");

  mu_assert_eq("unfinished span should have 0 duration", 0,
               span_duration_ns(span));

  usleep(5000); /* 5ms */
  span_finish(span);

  uint64_t duration = span_duration_ns(span);
  mu_assert("finished span should have non-zero duration", duration > 0);
  mu_assert("duration should be reasonable (> 4ms)", duration > 4000000);
  mu_assert("duration should be reasonable (< 10ms)", duration < 10000000);

  trace_destroy(trace);
  return NULL;
}

/* ========== Hierarchy Tests ========== */

static const char *test_parent_child_hierarchy() {
  trace_t *trace = trace_create();

  span_t *parent = span_create(trace, NULL, "parent");
  mu_assert("parent should be root_span", trace->root_span == parent);
  mu_assert("parent should have NULL parent", parent->parent == NULL);
  mu_assert_eq("parent should have parent_span_id = 0", 0,
               parent->parent_span_id);

  span_t *child1 = span_create(trace, parent, "child1");
  mu_assert("child1 should have parent", child1->parent == parent);
  mu_assert_eq("child1 should have parent's span_id", parent->span_id,
               child1->parent_span_id);
  mu_assert("parent should have child1 as first_child",
            parent->first_child == child1);

  span_t *child2 = span_create(trace, parent, "child2");
  mu_assert("child2 should have parent", child2->parent == parent);
  mu_assert("child1 should have child2 as next_sibling",
            child1->next_sibling == child2);
  mu_assert("child2 should have NULL next_sibling",
            child2->next_sibling == NULL);

  trace_destroy(trace);
  return NULL;
}

static const char *test_nested_hierarchy() {
  trace_t *trace = trace_create();

  span_t *root = span_create(trace, NULL, "root");
  span_t *child = span_create(trace, root, "child");
  span_t *grandchild = span_create(trace, child, "grandchild");

  mu_assert("grandchild's parent should be child", grandchild->parent == child);
  mu_assert("child's first_child should be grandchild",
            child->first_child == grandchild);
  mu_assert_eq("grandchild should have child's span_id as parent",
               child->span_id, grandchild->parent_span_id);

  trace_destroy(trace);
  return NULL;
}

static const char *test_timestamp_ordering() {
  trace_t *trace = trace_create();

  span_t *parent = span_create(trace, NULL, "parent");
  usleep(100);

  span_t *child = span_create(trace, parent, "child");
  usleep(100);
  span_finish(child);

  usleep(100);
  span_finish(parent);

  /* Parent should encompass child */
  mu_assert("parent start <= child start",
            parent->monotonic_start_ns <= child->monotonic_start_ns);
  mu_assert("child end <= parent end",
            child->monotonic_end_ns <= parent->monotonic_end_ns);

  /* Child times should be ordered */
  mu_assert("child start < child end",
            child->monotonic_start_ns < child->monotonic_end_ns);

  trace_destroy(trace);
  return NULL;
}

/* ========== Annotation Tests ========== */

static const char *test_annotation_storage() {
  trace_t *trace = trace_create();
  span_t *span = span_create(trace, NULL, "test");

  mu_assert_eq("new span should have 0 annotations", 0, span->annotation_count);

  span_annotate(span, "key1", "value1");
  mu_assert_eq("should have 1 annotation", 1, span->annotation_count);
  mu_assert_str_eq("first annotation key should match", "key1",
                   span->annotations[0].key);
  mu_assert_str_eq("first annotation value should match", "value1",
                   span->annotations[0].value);

  span_annotate(span, "key2", "value2");
  mu_assert_eq("should have 2 annotations", 2, span->annotation_count);

  trace_destroy(trace);
  return NULL;
}

static const char *test_annotation_overflow() {
  trace_t *trace = trace_create();
  span_t *span = span_create(trace, NULL, "test");

  /* Add maximum annotations */
  for (int i = 0; i < MAX_ANNOTATIONS; i++) {
    char key[32], value[32];
    snprintf(key, sizeof(key), "key%d", i);
    snprintf(value, sizeof(value), "value%d", i);
    span_annotate(span, key, value);
  }

  mu_assert_eq("should have max annotations", MAX_ANNOTATIONS,
               span->annotation_count);

  /* Try to add one more (should be silently ignored) */
  span_annotate(span, "overflow", "ignored");
  mu_assert_eq("should still have max annotations", MAX_ANNOTATIONS,
               span->annotation_count);

  trace_destroy(trace);
  return NULL;
}

static const char *test_annotation_truncation() {
  trace_t *trace = trace_create();
  span_t *span = span_create(trace, NULL, "test");

  char long_key[128], long_value[256];
  memset(long_key, 'K', sizeof(long_key) - 1);
  long_key[sizeof(long_key) - 1] = '\0';
  memset(long_value, 'V', sizeof(long_value) - 1);
  long_value[sizeof(long_value) - 1] = '\0';

  span_annotate(span, long_key, long_value);

  mu_assert("key should be truncated",
            strlen(span->annotations[0].key) < strlen(long_key));
  mu_assert("value should be truncated",
            strlen(span->annotations[0].value) < strlen(long_value));

  trace_destroy(trace);
  return NULL;
}

static const char *test_annotation_null_safety() {
  trace_t *trace = trace_create();
  span_t *span = span_create(trace, NULL, "test");

  /* Should not crash with NULL inputs */
  span_annotate(NULL, "key", "value");
  span_annotate(span, NULL, "value");
  span_annotate(span, "key", NULL);

  mu_assert_eq("should have 0 annotations after NULL calls", 0,
               span->annotation_count);

  trace_destroy(trace);
  return NULL;
}

/* ========== Test Suite ========== */

static const char *all_tests() {
  printf("Running Phase 1 Unit Tests\n");
  printf("===========================\n\n");

  printf("Trace Tests:\n");
  mu_run_test(test_trace_create_destroy);
  mu_run_test(test_trace_with_id);
  mu_run_test(test_trace_destroy_null);

  printf("Span Tests:\n");
  mu_run_test(test_span_create_finish);
  mu_run_test(test_span_name_truncation);
  mu_run_test(test_span_finish_idempotent);
  mu_run_test(test_span_duration);

  printf("Hierarchy Tests:\n");
  mu_run_test(test_parent_child_hierarchy);
  mu_run_test(test_nested_hierarchy);
  mu_run_test(test_timestamp_ordering);

  printf("Annotation Tests:\n");
  mu_run_test(test_annotation_storage);
  mu_run_test(test_annotation_overflow);
  mu_run_test(test_annotation_truncation);
  mu_run_test(test_annotation_null_safety);

  return NULL;
}

int main(void) {
  const char *result = all_tests();
  if (result != 0) {
    printf("\nFinal failure: %s\n", result);
  }
  mu_report();
}
