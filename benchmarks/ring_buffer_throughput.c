/**
 * ring_buffer_throughput.c - Benchmark ring buffer push/pop throughput
 *
 * Measures sustained throughput with one producer and one consumer thread.
 * Target: > 1M spans/sec
 */

#define _POSIX_C_SOURCE 200809L
#include "dapper/exporter.h"
#include "dapper/span.h"
#include "dapper/trace.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#define NUM_SPANS 1000000
#define RING_CAP 4096

static ring_buffer_t *g_rb;
static _Atomic int g_consumed;

static uint64_t get_time_ns(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static void *producer_func(void *arg) {
  trace_t *trace = (trace_t *)arg;
  span_t *span = span_create(trace, NULL, "bench_op");
  span_annotate(span, "k", "v");
  span_finish(span);

  for (int i = 0; i < NUM_SPANS; i++) {
    while (!ring_buffer_push(g_rb, span, true)) {
      /* Spin until space available */
    }
  }
  return NULL;
}

static void *consumer_func(void *arg) {
  (void)arg;
  uint8_t buf[RING_BUFFER_ENTRY_SIZE];
  size_t len;

  while (atomic_load(&g_consumed) < NUM_SPANS) {
    if (ring_buffer_pop(g_rb, buf, &len)) {
      atomic_fetch_add(&g_consumed, 1);
    }
  }
  return NULL;
}

int main(void) {
  printf("=== Ring Buffer Throughput Benchmark ===\n\n");
  printf("Configuration:\n");
  printf("  Spans: %d\n", NUM_SPANS);
  printf("  Ring buffer capacity: %d\n", RING_CAP);
  printf("  Mode: 1 producer + 1 consumer (SPSC)\n\n");

  g_rb = ring_buffer_create(RING_CAP);
  if (!g_rb) {
    fprintf(stderr, "Failed to create ring buffer\n");
    return 1;
  }

  trace_t *trace = trace_create();
  atomic_store(&g_consumed, 0);

  printf("Running benchmark...\n");

  uint64_t start = get_time_ns();

  pthread_t prod, cons;
  pthread_create(&cons, NULL, consumer_func, NULL);
  pthread_create(&prod, NULL, producer_func, trace);

  pthread_join(prod, NULL);
  pthread_join(cons, NULL);

  uint64_t end = get_time_ns();
  uint64_t elapsed_ns = end - start;
  double elapsed_s = (double)elapsed_ns / 1e9;
  double throughput = (double)NUM_SPANS / elapsed_s;

  printf("\nResults:\n");
  printf("  Total spans: %d\n", NUM_SPANS);
  printf("  Elapsed: %.3f s\n", elapsed_s);
  printf("  Throughput: %.0f spans/sec\n", throughput);
  printf("  Per-span: %.0f ns\n", (double)elapsed_ns / NUM_SPANS);

  printf("\nTarget: > 1,000,000 spans/sec\n");
  if (throughput > 1000000.0) {
    printf("Status: PASS\n");
  } else {
    printf("Status: FAIL (%.0f spans/sec)\n", throughput);
  }

  ring_buffer_destroy(g_rb);
  trace_destroy(trace);

  return (throughput > 1000000.0) ? 0 : 1;
}
