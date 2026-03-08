/**
 * common.h - Shared benchmark harness for Dapper-Lite
 *
 * Provides timing utilities, percentile calculation, result printing,
 * and a generic run_benchmark() driver to DRY up benchmark code.
 */

#ifndef BENCHMARK_COMMON_H
#define BENCHMARK_COMMON_H

#define _POSIX_C_SOURCE 200809L
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#define BENCHMARK_ITERATIONS_1M 1000000
#define BENCHMARK_ITERATIONS_10M 10000000

/**
 * Benchmark result with latency statistics.
 */
typedef struct {
  uint64_t min_ns;
  uint64_t max_ns;
  uint64_t mean_ns;
  uint64_t p50_ns;
  uint64_t p99_ns;
  uint64_t p90_ns;
  uint64_t total_ns;
  int count;
} benchmark_result_t;

/**
 * Get current monotonic time in nanoseconds.
 */
static inline uint64_t bench_get_time_ns(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static inline int bench_compare_uint64(const void *a, const void *b) {
  uint64_t ua = *(const uint64_t *)a;
  uint64_t ub = *(const uint64_t *)b;
  if (ua < ub)
    return -1;
  if (ua > ub)
    return 1;
  return 0;
}

static inline uint64_t bench_percentile(uint64_t *sorted, int count,
                                        double p) {
  int index = (int)(count * p);
  if (index >= count)
    index = count - 1;
  return sorted[index];
}

/**
 * Compute statistics from a timings array. Sorts the array in place.
 */
static inline benchmark_result_t bench_compute_stats(uint64_t *timings,
                                                     int count) {
  benchmark_result_t r;
  r.count = count;

  uint64_t total = 0;
  for (int i = 0; i < count; i++) {
    total += timings[i];
  }
  r.total_ns = total;
  r.mean_ns = total / (uint64_t)count;

  qsort(timings, (size_t)count, sizeof(uint64_t), bench_compare_uint64);

  r.min_ns = timings[0];
  r.max_ns = timings[count - 1];
  r.p50_ns = bench_percentile(timings, count, 0.50);
  r.p90_ns = bench_percentile(timings, count, 0.90);
  r.p99_ns = bench_percentile(timings, count, 0.99);

  return r;
}

/**
 * Print benchmark results in a standard format.
 */
static inline void bench_print_results(const benchmark_result_t *r) {
  printf("  Mean:  %llu ns\n", (unsigned long long)r->mean_ns);
  printf("  p50:   %llu ns\n", (unsigned long long)r->p50_ns);
  printf("  p90:   %llu ns\n", (unsigned long long)r->p90_ns);
  printf("  p99:   %llu ns\n", (unsigned long long)r->p99_ns);
  printf("  Min:   %llu ns\n", (unsigned long long)r->min_ns);
  printf("  Max:   %llu ns\n", (unsigned long long)r->max_ns);
}

/**
 * Print pass/fail against a latency target (p50 < target_ns).
 */
static inline int bench_check_latency(const benchmark_result_t *r,
                                      uint64_t target_ns,
                                      const char *metric_name) {
  printf("\nTarget: %s p50 < %llu ns\n", metric_name,
         (unsigned long long)target_ns);
  if (r->p50_ns < target_ns) {
    printf("Status: PASS\n");
    return 0;
  } else {
    printf("Status: FAIL (p50 = %llu ns)\n", (unsigned long long)r->p50_ns);
    return 1;
  }
}

/**
 * Print pass/fail against a throughput target.
 */
static inline int bench_check_throughput(double actual_per_sec,
                                         double target_per_sec,
                                         const char *metric_name) {
  printf("\nTarget: %s > %.0f /sec\n", metric_name, target_per_sec);
  if (actual_per_sec > target_per_sec) {
    printf("Status: PASS\n");
    return 0;
  } else {
    printf("Status: FAIL (%.0f /sec)\n", actual_per_sec);
    return 1;
  }
}

/**
 * Allocate a timings array. Returns NULL on failure (prints error).
 */
static inline uint64_t *bench_alloc_timings(int count) {
  uint64_t *t = (uint64_t *)malloc((size_t)count * sizeof(uint64_t));
  if (!t) {
    fprintf(stderr, "Failed to allocate timings array (%d entries)\n", count);
  }
  return t;
}

/**
 * Run a benchmark function N times, collecting per-iteration timings.
 *
 * func: Called once per iteration with the provided arg
 * arg: Opaque argument passed to func
 * iterations: Number of iterations to run
 *
 * Returns: Computed benchmark_result_t (timings array is freed internally).
 *          On allocation failure, returns a result with all zeros.
 */
static inline benchmark_result_t
run_benchmark(void (*func)(void *), void *arg, int iterations) {
  uint64_t *timings = bench_alloc_timings(iterations);
  if (!timings) {
    benchmark_result_t empty = {0};
    return empty;
  }

  for (int i = 0; i < iterations; i++) {
    uint64_t start = bench_get_time_ns();
    func(arg);
    uint64_t end = bench_get_time_ns();
    timings[i] = end - start;
  }

  benchmark_result_t r = bench_compute_stats(timings, iterations);
  free(timings);
  return r;
}

#endif /* BENCHMARK_COMMON_H */
