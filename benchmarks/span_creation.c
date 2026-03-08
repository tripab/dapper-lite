/**
 * span_creation.c - Benchmark span_create() + span_finish() latency
 *
 * Measures the pure instrumentation overhead of creating and finishing
 * a span (no sampling, no export). This is the core hot-path cost
 * that every instrumented operation pays.
 */

#define _POSIX_C_SOURCE 200809L
#include "common.h"
#include "dapper/span.h"
#include "dapper/trace.h"

#define ITERATIONS BENCHMARK_ITERATIONS_1M

static void benchmark_span_create(void *arg) {
  trace_t *trace = (trace_t *)arg;
  span_t *span = span_create(trace, NULL, "test_span");
  span_finish(span);
}

int main(void) {
  printf("=== Span Creation Latency Benchmark ===\n\n");

  trace_t *trace = trace_create();
  if (!trace) {
    fprintf(stderr, "Failed to create trace\n");
    return 1;
  }

  printf("Configuration:\n");
  printf("  Iterations: %d\n", ITERATIONS);
  printf("  Operation: span_create() + span_finish()\n\n");

  printf("Running benchmark...\n");

  benchmark_result_t r = run_benchmark(benchmark_span_create, trace, ITERATIONS);

  printf("\nResults:\n");
  bench_print_results(&r);

  trace_destroy(trace);
  return 0;
}
