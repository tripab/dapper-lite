/**
 * export_throughput.c - Benchmark exporter_submit() hot-path latency
 *
 * Measures the cost of exporter_submit() which is called from
 * instrumented code. This is the critical hot-path metric.
 * Target: < 100ns per submit (p50)
 */

#define _POSIX_C_SOURCE 200809L
#include "dapper/exporter.h"
#include "dapper/span.h"
#include "dapper/trace.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

#define ITERATIONS 1000000

static uint64_t get_time_ns(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static uint64_t percentile(uint64_t *sorted, int count, double p) {
  int index = (int)(count * p);
  if (index >= count)
    index = count - 1;
  return sorted[index];
}

static int compare_uint64(const void *a, const void *b) {
  uint64_t ua = *(const uint64_t *)a;
  uint64_t ub = *(const uint64_t *)b;
  if (ua < ub)
    return -1;
  if (ua > ub)
    return 1;
  return 0;
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

  uint64_t *timings = malloc(ITERATIONS * sizeof(uint64_t));
  if (!timings) {
    fprintf(stderr, "Failed to allocate timings\n");
    exporter_destroy(exp);
    trace_destroy(trace);
    return 1;
  }

  printf("Running benchmark...\n");

  uint64_t total_time = 0;
  for (int i = 0; i < ITERATIONS; i++) {
    uint64_t start = get_time_ns();
    exporter_submit(exp, span);
    uint64_t end = get_time_ns();

    uint64_t elapsed = end - start;
    timings[i] = elapsed;
    total_time += elapsed;
  }

  /* Sort for percentiles */
  qsort(timings, ITERATIONS, sizeof(uint64_t), compare_uint64);

  uint64_t mean = total_time / ITERATIONS;
  uint64_t p50 = percentile(timings, ITERATIONS, 0.50);
  uint64_t p90 = percentile(timings, ITERATIONS, 0.90);
  uint64_t p99 = percentile(timings, ITERATIONS, 0.99);
  uint64_t min = timings[0];
  uint64_t max = timings[ITERATIONS - 1];

  printf("\nResults:\n");
  printf("  Mean:  %llu ns\n", (unsigned long long)mean);
  printf("  p50:   %llu ns\n", (unsigned long long)p50);
  printf("  p90:   %llu ns\n", (unsigned long long)p90);
  printf("  p99:   %llu ns\n", (unsigned long long)p99);
  printf("  Min:   %llu ns\n", (unsigned long long)min);
  printf("  Max:   %llu ns\n", (unsigned long long)max);

  exporter_stats_t stats;
  exporter_get_stats(exp, &stats);
  printf("\nExporter Statistics:\n");
  printf("  Submitted: %llu\n", (unsigned long long)stats.spans_submitted);
  printf("  Exported:  %llu\n", (unsigned long long)stats.spans_exported);
  printf("  Dropped:   %llu\n", (unsigned long long)stats.spans_dropped);

  printf("\nTarget: < 500ns (p50)\n");
  if (p50 < 500) {
    printf("Status: PASS\n");
  } else {
    printf("Status: FAIL (p50 = %llu ns)\n", (unsigned long long)p50);
  }

  free(timings);
  exporter_destroy(exp);
  unlink(path);
  trace_destroy(trace);

  return (p50 < 500) ? 0 : 1;
}
