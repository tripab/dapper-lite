/**
 * overhead_analysis.c - End-to-end overhead analysis
 *
 * Runs the core microbenchmarks inline and computes the total
 * instrumentation overhead for a realistic workload:
 *
 *   Service: 10k requests/sec, 5 spans/request, 1% sampling
 *   => 50k spans/sec created, 500 spans/sec exported
 *
 * Prints a summary table with per-component and total CPU overhead.
 */

#define _POSIX_C_SOURCE 200809L
#include "common.h"
#include "dapper/context.h"
#include "dapper/exporter.h"
#include "dapper/sampler.h"
#include "dapper/span.h"
#include "dapper/trace.h"
#include <unistd.h>

/* Workload parameters */
#define REQUESTS_PER_SEC 10000
#define SPANS_PER_REQUEST 5
#define SAMPLE_RATE 0.01

/* Benchmark iterations (smaller for quick inline runs) */
#define QUICK_ITERS 100000

/* --- Span creation --- */

static void bench_span(void *arg) {
  trace_t *trace = (trace_t *)arg;
  span_t *span = span_create(trace, NULL, "op");
  span_finish(span);
}

/* --- Sampling decision --- */

typedef struct {
  sampler_t *sampler;
  sampling_decision_t decision;
} sample_ctx_t;

static void bench_sample(void *arg) {
  sample_ctx_t *ctx = (sample_ctx_t *)arg;
  sampler_should_sample(ctx->sampler, NULL, &ctx->decision);
}

/* --- Context injection --- */

typedef struct {
  span_t *span;
  uint8_t buf[TRACE_CONTEXT_WIRE_SIZE];
} inject_ctx_t;

static void bench_inject(void *arg) {
  inject_ctx_t *ctx = (inject_ctx_t *)arg;
  context_inject(ctx->span, ctx->buf, sizeof(ctx->buf));
}

/* --- Export submit --- */

typedef struct {
  exporter_t *exp;
  span_t *span;
} export_ctx_t;

static void bench_export(void *arg) {
  export_ctx_t *ctx = (export_ctx_t *)arg;
  exporter_submit(ctx->exp, ctx->span);
}

int main(void) {
  printf("=== Dapper-Lite Overhead Analysis ===\n\n");

  printf("Workload assumptions:\n");
  printf("  Requests/sec:     %d\n", REQUESTS_PER_SEC);
  printf("  Spans/request:    %d\n", SPANS_PER_REQUEST);
  printf("  Total spans/sec:  %d\n", REQUESTS_PER_SEC * SPANS_PER_REQUEST);
  printf("  Sampling rate:    %.0f%%\n", SAMPLE_RATE * 100);
  printf("  Exported/sec:     %d\n",
         (int)(REQUESTS_PER_SEC * SPANS_PER_REQUEST * SAMPLE_RATE));
  printf("  Bench iterations: %d\n\n", QUICK_ITERS);

  /* Setup */
  trace_t *trace = trace_create();
  sampler_t *sampler = sampler_create_probability(SAMPLE_RATE);
  span_t *span = span_create(trace, NULL, "setup");
  span_finish(span);

  const char *export_path = "/tmp/dapper_bench_overhead.bin";
  exporter_t *exp = exporter_create_file(export_path);
  exporter_start(exp);

  /* 1. Span creation */
  printf("Measuring span creation...\n");
  benchmark_result_t r_span = run_benchmark(bench_span, trace, QUICK_ITERS);

  /* 2. Sampling decision */
  printf("Measuring sampling decision...\n");
  sample_ctx_t sctx;
  sctx.sampler = sampler;
  benchmark_result_t r_sample =
      run_benchmark(bench_sample, &sctx, QUICK_ITERS);

  /* 3. Context injection */
  printf("Measuring context injection...\n");
  inject_ctx_t ictx;
  ictx.span = span;
  benchmark_result_t r_inject =
      run_benchmark(bench_inject, &ictx, QUICK_ITERS);

  /* 4. Export submit */
  printf("Measuring export submit...\n");
  export_ctx_t ectx;
  ectx.exp = exp;
  ectx.span = span;
  benchmark_result_t r_export =
      run_benchmark(bench_export, &ectx, QUICK_ITERS);

  /* Compute overhead */
  int total_spans_sec = REQUESTS_PER_SEC * SPANS_PER_REQUEST;
  int exported_sec = (int)(total_spans_sec * SAMPLE_RATE);

  double span_cpu_ms =
      (double)total_spans_sec * (double)r_span.mean_ns / 1e6;
  double sample_cpu_ms =
      (double)REQUESTS_PER_SEC * (double)r_sample.mean_ns / 1e6;
  double inject_cpu_ms =
      (double)REQUESTS_PER_SEC * (double)r_inject.mean_ns / 1e6;
  double export_cpu_ms =
      (double)exported_sec * (double)r_export.mean_ns / 1e6;
  double total_cpu_ms = span_cpu_ms + sample_cpu_ms + inject_cpu_ms +
                        export_cpu_ms;
  double total_cpu_pct = total_cpu_ms / 10.0; /* 1000ms/sec, /1000*100 */

  /* Print summary table */
  printf("\n");
  printf(
      "+----------------------+-----------+-----------+----------+---------+\n");
  printf(
      "| Component            | Mean (ns) | Calls/sec | CPU (ms) | CPU (%%) |\n");
  printf(
      "+----------------------+-----------+-----------+----------+---------+\n");
  printf("| Span create+finish   | %9llu | %9d | %8.2f | %6.3f%% |\n",
         (unsigned long long)r_span.mean_ns, total_spans_sec, span_cpu_ms,
         span_cpu_ms / 10.0);
  printf("| Sampling decision    | %9llu | %9d | %8.2f | %6.3f%% |\n",
         (unsigned long long)r_sample.mean_ns, REQUESTS_PER_SEC,
         sample_cpu_ms, sample_cpu_ms / 10.0);
  printf("| Context injection    | %9llu | %9d | %8.2f | %6.3f%% |\n",
         (unsigned long long)r_inject.mean_ns, REQUESTS_PER_SEC,
         inject_cpu_ms, inject_cpu_ms / 10.0);
  printf("| Export submit         | %9llu | %9d | %8.2f | %6.3f%% |\n",
         (unsigned long long)r_export.mean_ns, exported_sec, export_cpu_ms,
         export_cpu_ms / 10.0);
  printf(
      "+----------------------+-----------+-----------+----------+---------+\n");
  printf("| TOTAL                |           |           | %8.2f | %6.3f%% |\n",
         total_cpu_ms, total_cpu_pct);
  printf(
      "+----------------------+-----------+-----------+----------+---------+\n");

  printf("\nConclusion: Total overhead = %.2f ms/sec = %.3f%% CPU per core\n",
         total_cpu_ms, total_cpu_pct);

  if (total_cpu_pct < 1.0) {
    printf("Target: < 1%% CPU => PASS\n");
  } else {
    printf("Target: < 1%% CPU => FAIL (%.3f%%)\n", total_cpu_pct);
  }

  /* Cleanup */
  exporter_destroy(exp);
  unlink(export_path);
  sampler_destroy(sampler);
  trace_destroy(trace);

  return (total_cpu_pct < 1.0) ? 0 : 1;
}
