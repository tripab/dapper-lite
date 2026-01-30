/**
 * test_phase2.c - Unit tests for Phase 2 functionality
 *
 * Tests:
 * - Thread-local current span
 * - Context serialization/deserialization
 * - Cross-process span creation
 * - Wall-clock timestamps
 */

#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L
#include "dapper/context.h"
#include "dapper/span.h"
#include "dapper/trace.h"
#include "minunit.h"
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

int tests_run = 0;
int tests_failed = 0;

/* ========== Thread-Local Tests ========== */

static const char *test_thread_local_set_get() {
  trace_t *trace = trace_create();
  span_t *span = span_create(trace, NULL, "test");

  span_set_current(span);
  span_t *current = span_get_current();

  mu_assert("set_current then get_current should return same span",
            current == span);

  span_set_current(NULL);
  current = span_get_current();

  mu_assert("set_current(NULL) should clear current span", current == NULL);

  trace_destroy(trace);
  return NULL;
}

static void *thread_local_isolation_worker(void *arg) {
  int thread_id = *(int *)arg;

  trace_t *trace = trace_create();
  span_t *span = span_create(trace, NULL, "thread_span");

  /* Set current in this thread */
  span_set_current(span);

  usleep(1000); /* Let other threads run */

  /* Verify we still get our own span */
  span_t *current = span_get_current();
  if (current != span) {
    fprintf(stderr, "Thread %d: isolation violated!\n", thread_id);
    trace_destroy(trace);
    return (void *)1;
  }

  span_finish(span);
  trace_destroy(trace);
  return (void *)0;
}

static const char *test_thread_local_isolation() {
  pthread_t threads[4];
  int thread_ids[4] = {0, 1, 2, 3};

  /* Create multiple threads, each with its own current span */
  for (int i = 0; i < 4; i++) {
    pthread_create(&threads[i], NULL, thread_local_isolation_worker,
                   &thread_ids[i]);
  }

  /* Wait for all threads */
  for (int i = 0; i < 4; i++) {
    void *result;
    pthread_join(threads[i], &result);
    mu_assert("thread-local span should be isolated per thread",
              result == (void *)0);
  }

  return NULL;
}

/* ========== Context Serialization Tests ========== */

static const char *test_context_inject_extract() {
  trace_t *trace = trace_create();
  span_t *span = span_create(trace, NULL, "test_span");

  /* Inject */
  uint8_t buffer[TRACE_CONTEXT_WIRE_SIZE];
  int len = context_inject(span, buffer, sizeof(buffer));

  mu_assert_eq("inject should return correct size", TRACE_CONTEXT_WIRE_SIZE,
               len);

  /* Extract */
  trace_context_t ctx;
  int result = context_extract(&ctx, buffer, sizeof(buffer));

  mu_assert_eq("extract should succeed", 0, result);
  mu_assert_eq("extracted trace_id should match", span->trace_id, ctx.trace_id);
  mu_assert_eq("extracted span_id should match", span->span_id, ctx.span_id);

  trace_destroy(trace);
  return NULL;
}

static const char *test_context_malformed() {
  trace_context_t ctx;
  uint8_t buffer[8]; /* Too small */

  int result = context_extract(&ctx, buffer, sizeof(buffer));
  mu_assert_eq("extract with small buffer should fail", -1, result);

  result = context_extract(&ctx, NULL, 16);
  mu_assert_eq("extract with NULL buffer should fail", -1, result);

  result = context_extract(NULL, buffer, 16);
  mu_assert_eq("extract with NULL context should fail", -1, result);

  return NULL;
}

static const char *test_context_endianness() {
  trace_t *trace = trace_create_with_id(0x0123456789ABCDEFULL);
  span_t *span = span_create(trace, NULL, "test");

  uint8_t buffer[TRACE_CONTEXT_WIRE_SIZE];
  context_inject(span, buffer, sizeof(buffer));

  /* Verify network byte order (big-endian) */
  /* First byte should be 0x01 (most significant byte of trace_id) */
  mu_assert_eq("first byte should be MSB of trace_id", 0x01, buffer[0]);

  /* Extract and verify round-trip */
  trace_context_t ctx;
  context_extract(&ctx, buffer, sizeof(buffer));

  mu_assert_eq("round-trip should preserve trace_id", 0x0123456789ABCDEFULL,
               ctx.trace_id);

  trace_destroy(trace);
  return NULL;
}

/* ========== Cross-Process Span Tests ========== */

static const char *test_span_create_from_context() {
  /* Simulate receiving context from another process */
  trace_context_t ctx;
  ctx.trace_id = 0xAAAAAAAAAAAAAAAAULL;
  ctx.span_id = 0xBBBBBBBBBBBBBBBBULL;

  trace_t *trace = trace_create(); /* Will be updated */
  span_t *span = span_create_from_context(trace, &ctx, "received_span");

  mu_assert("span_create_from_context should succeed", span != NULL);
  mu_assert_eq("span should inherit trace_id", ctx.trace_id, span->trace_id);
  mu_assert_eq("span should have remote parent_span_id", ctx.span_id,
               span->parent_span_id);
  mu_assert_eq("trace should be updated with extracted trace_id", ctx.trace_id,
               trace->id);

  trace_destroy(trace);
  return NULL;
}

static const char *test_cross_process_trace_continuity() {
  /* Simulate full cross-process flow */

  /* Process 1: Frontend creates trace */
  trace_t *trace1 = trace_create();
  span_t *frontend_span = span_create(trace1, NULL, "frontend");

  trace_id_t original_trace_id = trace1->id;
  span_id_t frontend_span_id = frontend_span->span_id;

  /* Serialize and send */
  uint8_t buffer[TRACE_CONTEXT_WIRE_SIZE];
  context_inject(frontend_span, buffer, sizeof(buffer));

  /* Process 2: Backend receives and continues trace */
  trace_context_t ctx;
  context_extract(&ctx, buffer, sizeof(buffer));

  trace_t *trace2 = trace_create();
  span_t *backend_span = span_create_from_context(trace2, &ctx, "backend");

  /* Verify continuity */
  mu_assert_eq("backend trace_id should match frontend", original_trace_id,
               backend_span->trace_id);
  mu_assert_eq("backend parent should be frontend span", frontend_span_id,
               backend_span->parent_span_id);

  trace_destroy(trace1);
  trace_destroy(trace2);
  return NULL;
}

/* ========== Wall-Clock Timestamp Tests ========== */

static const char *test_wall_clock_timestamp() {
  trace_t *trace = trace_create();
  span_t *span = span_create(trace, NULL, "test");

  mu_assert("wall_start_us should be non-zero", span->wall_start_us != 0);

  /* Wall-clock should be reasonable (within last 10 years and next 1 year) */
  uint64_t min_time = 1420070400000000ULL; /* 2015-01-01 */
  uint64_t max_time = 2000000000000000ULL; /* 2033-05-18 */

  mu_assert("wall_start_us should be reasonable (> 2015)",
            span->wall_start_us > min_time);
  mu_assert("wall_start_us should be reasonable (< 2033)",
            span->wall_start_us < max_time);

  trace_destroy(trace);
  return NULL;
}

static const char *test_dual_timestamps() {
  trace_t *trace = trace_create();
  span_t *span = span_create(trace, NULL, "test");

  uint64_t mono_start = span->monotonic_start_ns;
  uint64_t wall_start = span->wall_start_us;

  usleep(5000); /* 5ms */
  span_finish(span);

  uint64_t mono_end = span->monotonic_end_ns;

  /* Both timestamps should be captured */
  mu_assert("monotonic_start_ns should be set", mono_start != 0);
  mu_assert("wall_start_us should be set", wall_start != 0);
  mu_assert("monotonic_end_ns should be set", mono_end != 0);

  /* Duration should be reasonable */
  uint64_t duration_ns = mono_end - mono_start;
  mu_assert("duration should be > 4ms", duration_ns > 4000000);
  mu_assert("duration should be < 10ms", duration_ns < 10000000);

  trace_destroy(trace);
  return NULL;
}

/* ========== Test Suite ========== */

static const char *all_tests() {
  printf("Running Phase 2 Unit Tests\n");
  printf("===========================\n\n");

  printf("Thread-Local Tests:\n");
  mu_run_test(test_thread_local_set_get);
  mu_run_test(test_thread_local_isolation);

  printf("Context Serialization Tests:\n");
  mu_run_test(test_context_inject_extract);
  mu_run_test(test_context_malformed);
  mu_run_test(test_context_endianness);

  printf("Cross-Process Span Tests:\n");
  mu_run_test(test_span_create_from_context);
  mu_run_test(test_cross_process_trace_continuity);

  printf("Wall-Clock Timestamp Tests:\n");
  mu_run_test(test_wall_clock_timestamp);
  mu_run_test(test_dual_timestamps);

  return NULL;
}

int main(void) {
  const char *result = all_tests();
  if (result != 0) {
    printf("\nFinal failure: %s\n", result);
  }
  mu_report();
}
