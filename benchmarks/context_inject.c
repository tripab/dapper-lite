/**
 * context_inject.c - Benchmark context_inject() latency
 *
 * Measures the cost of serializing trace context into a wire-format
 * buffer, as would happen on every outgoing RPC.
 * Target: < 50ns per injection (p50)
 */

#define _POSIX_C_SOURCE 200809L
#include "common.h"
#include "dapper/context.h"
#include "dapper/span.h"
#include "dapper/trace.h"

#define ITERATIONS BENCHMARK_ITERATIONS_10M

typedef struct {
  span_t *span;
  uint8_t buf[TRACE_CONTEXT_WIRE_SIZE];
} inject_ctx_t;

static void benchmark_inject(void *arg) {
  inject_ctx_t *ctx = (inject_ctx_t *)arg;
  context_inject(ctx->span, ctx->buf, sizeof(ctx->buf));
}

int main(void) {
  printf("=== Context Injection Latency Benchmark ===\n\n");

  trace_t *trace = trace_create();
  if (!trace) {
    fprintf(stderr, "Failed to create trace\n");
    return 1;
  }

  span_t *span = span_create(trace, NULL, "bench_inject");
  if (!span) {
    fprintf(stderr, "Failed to create span\n");
    trace_destroy(trace);
    return 1;
  }

  inject_ctx_t ctx;
  ctx.span = span;

  printf("Configuration:\n");
  printf("  Iterations: %d\n", ITERATIONS);
  printf("  Operation: context_inject() (serialize trace context)\n\n");

  printf("Running benchmark...\n");

  benchmark_result_t r = run_benchmark(benchmark_inject, &ctx, ITERATIONS);

  printf("\nResults:\n");
  bench_print_results(&r);

  int rc = bench_check_latency(&r, 50, "context_inject");

  trace_destroy(trace);
  return rc;
}
