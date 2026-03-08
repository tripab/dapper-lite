/**
 * sampling_decision.c - Benchmark sampling decision latency
 *
 * Target: < 200ns per decision (p50)
 */

#define _POSIX_C_SOURCE 200809L
#include "common.h"
#include "dapper/sampler.h"

#define ITERATIONS BENCHMARK_ITERATIONS_10M

typedef struct {
  sampler_t *sampler;
  sampling_decision_t decision;
} sample_ctx_t;

static void benchmark_sample(void *arg) {
  sample_ctx_t *ctx = (sample_ctx_t *)arg;
  sampler_should_sample(ctx->sampler, NULL, &ctx->decision);
}

int main(void) {
  printf("=== Sampling Decision Latency Benchmark ===\n\n");

  sampler_t *sampler = sampler_create_probability(0.01); /* 1% */
  if (!sampler) {
    fprintf(stderr, "Failed to create sampler\n");
    return 1;
  }

  printf("Configuration:\n");
  printf("  Iterations: %d\n", ITERATIONS);
  printf("  Sample rate: 1%%\n\n");

  printf("Running benchmark...\n");

  sample_ctx_t ctx;
  ctx.sampler = sampler;

  benchmark_result_t r = run_benchmark(benchmark_sample, &ctx, ITERATIONS);

  printf("\nResults:\n");
  bench_print_results(&r);

  /* Get sampler statistics */
  sampler_stats_t stats;
  if (sampler_get_stats(sampler, &stats) == 0) {
    printf("\nSampler Statistics:\n");
    printf("  Total decisions: %llu\n",
           (unsigned long long)stats.total_decisions);
    printf("  Sampled: %llu\n", (unsigned long long)stats.sampled_count);
    printf("  Dropped: %llu\n", (unsigned long long)stats.dropped_count);
    printf("  Actual rate: %.4f%%\n",
           100.0 * stats.sampled_count / stats.total_decisions);
  }

  int rc = bench_check_latency(&r, 200, "sampling decision");

  sampler_destroy(sampler);
  return rc;
}
