/**
 * trace_creation.c - Benchmark trace creation with sampling
 * 
 * Target: < 150ns per trace (base 100ns + 20ns sampling + overhead)
 */

#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <stdint.h>
#include "dapper/trace.h"
#include "dapper/sampler.h"

#define ITERATIONS 1000000 /* 1M traces */

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
    printf("=== Trace Creation with Sampling Benchmark ===\n\n");

    sampler_t *sampler = sampler_create_probability(0.01);
    if (!sampler) {
        fprintf(stderr, "Failed to create sampler\n");
        return 1;
    }

    printf("Configuration:\n");
    printf("  Iterations: %d\n", ITERATIONS);
    printf("  Sample rate: 1%%\n\n");

    uint64_t *timings = malloc(ITERATIONS * sizeof(uint64_t));
    if (!timings) {
        fprintf(stderr, "Failed to allocate timings array\n");
        sampler_destroy(sampler);
        return 1;
    }

    printf("Running benchmark...\n");

    uint64_t total_time = 0;

    for (int i = 0; i < ITERATIONS; i++) {
        uint64_t start = get_time_ns();
        trace_t *trace = trace_create_sampled(sampler, "/api/test");
        trace_destroy(trace);
        uint64_t end = get_time_ns();

        uint64_t elapsed = end - start;
        timings[i] = elapsed;
        total_time += elapsed;
    }

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

    printf("\nTarget: < 300ns (p50)\n");
    if (p50 < 300) {
        printf("Status: PASS ✓\n");
    } else {
        printf("Status: FAIL (p50 = %llu ns)\n", (unsigned long long)p50);
    }

    free(timings);
    sampler_destroy(sampler);

    return (p50 < 300) ? 0 : 1;
}
