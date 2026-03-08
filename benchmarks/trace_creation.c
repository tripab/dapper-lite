/**
 * trace_creation.c - Benchmark trace creation with sampling
 *
 * Target: < 300ns per trace (p50)
 */

#define _POSIX_C_SOURCE 200809L
#include "common.h"
#include "dapper/sampler.h"
#include "dapper/trace.h"

#define ITERATIONS BENCHMARK_ITERATIONS_1M

static void benchmark_trace_create(void *arg) {
  sampler_t *sampler = (sampler_t *)arg;
  trace_t *trace = trace_create_sampled(sampler, "/api/test");
  trace_destroy(trace);
}

int main(void) {
  printf("=== Trace Creation with Sampling Benchmark ===\n\n");

  sampler_t *sampler = sampler_create_probability(0.01);
  if (!sampler) {
    fprintf(stderr, "Failed to create sampler\n");
    return 1;
  }

  printf("Configuration:\n");
  printf("  Iterations: %d\n", ITERATIONS);
  printf("  Sample rate: 1%%\n\n");

  printf("Running benchmark...\n");

  benchmark_result_t r =
      run_benchmark(benchmark_trace_create, sampler, ITERATIONS);

  printf("\nResults:\n");
  bench_print_results(&r);

  int rc = bench_check_latency(&r, 300, "trace creation");

  sampler_destroy(sampler);
  return rc;
}
