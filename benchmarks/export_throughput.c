/**
 * export_throughput.c - Benchmark exporter_submit() hot-path latency
 *
 * Measures the cost of exporter_submit() which is called from
 * instrumented code. This is the critical hot-path metric.
 * Target: < 500ns per submit (p50)
 */

#define _POSIX_C_SOURCE 200809L
#include "common.h"
#include "dapper/exporter.h"
#include "dapper/span.h"
#include "dapper/trace.h"
#include <unistd.h>

#define ITERATIONS BENCHMARK_ITERATIONS_1M

typedef struct {
  exporter_t *exp;
  span_t *span;
} export_ctx_t;

static void benchmark_export_submit(void *arg) {
  export_ctx_t *ctx = (export_ctx_t *)arg;
  exporter_submit(ctx->exp, ctx->span);
}

int main(void) {
  printf("=== Export Submit Latency Benchmark ===\n\n");

  const char *path = "/tmp/dapper_bench_export.bin";
  exporter_t *exp = exporter_create_file(path);
  if (!exp) {
    fprintf(stderr, "Failed to create exporter\n");
    return 1;
  }

  if (exporter_start(exp) != 0) {
    fprintf(stderr, "Failed to start exporter\n");
    exporter_destroy(exp);
    return 1;
  }

  trace_t *trace = trace_create();
  span_t *span = span_create(trace, NULL, "bench_submit");
  span_annotate(span, "k", "v");
  span_finish(span);

  printf("Configuration:\n");
  printf("  Iterations: %d\n", ITERATIONS);
  printf("  Exporter: file sink (background thread running)\n\n");

  printf("Running benchmark...\n");

  export_ctx_t ctx;
  ctx.exp = exp;
  ctx.span = span;

  benchmark_result_t r =
      run_benchmark(benchmark_export_submit, &ctx, ITERATIONS);

  printf("\nResults:\n");
  bench_print_results(&r);

  exporter_stats_t stats;
  exporter_get_stats(exp, &stats);
  printf("\nExporter Statistics:\n");
  printf("  Submitted: %llu\n", (unsigned long long)stats.spans_submitted);
  printf("  Exported:  %llu\n", (unsigned long long)stats.spans_exported);
  printf("  Dropped:   %llu\n", (unsigned long long)stats.spans_dropped);

  int rc = bench_check_latency(&r, 500, "export submit");

  exporter_destroy(exp);
  unlink(path);
  trace_destroy(trace);

  return rc;
}
