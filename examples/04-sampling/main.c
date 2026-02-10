/**
 * 04-sampling - Demonstrates various sampling strategies
 */

#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L
#include "dapper/sampler.h"
#include "dapper/span.h"
#include "dapper/trace.h"
#include <stdio.h>
#include <unistd.h>

void demo_probability_sampling(void) {
  printf("=== Probabilistic Sampling (10%%) ===\n\n");

  sampler_t *sampler = sampler_create_probability(0.1);

  int sampled = 0, dropped = 0;
  for (int i = 0; i < 1000; i++) {
    trace_t *trace = trace_create_sampled(sampler, "/api/users");
    if (trace->sampled) {
      sampled++;
    } else {
      dropped++;
    }
    trace_destroy(trace);
  }

  sampler_stats_t stats;
  sampler_get_stats(sampler, &stats);

  printf("Created 1000 traces:\n");
  printf("  Sampled: %d (%.1f%%)\n", sampled, 100.0 * sampled / 1000);
  printf("  Dropped: %d (%.1f%%)\n", dropped, 100.0 * dropped / 1000);
  printf("\nSampler statistics:\n");
  printf("  Total decisions: %llu\n", stats.total_decisions);
  printf("  Sampled count: %llu\n", stats.sampled_count);
  printf("  Current rate: %.4f\n\n", stats.current_rate);

  sampler_destroy(sampler);
}

void demo_adaptive_sampling(void) {
  printf("=== Adaptive Sampling (target 50 traces/sec) ===\n\n");

  sampler_t *sampler = sampler_create_adaptive(50);

  printf("Simulating traffic over 5 seconds...\n");

  for (int sec = 0; sec < 5; sec++) {
    int sampled = 0;

    /* Simulate 200 requests per second */
    for (int i = 0; i < 200; i++) {
      trace_t *trace = trace_create_sampled(sampler, "/api/search");
      if (trace->sampled) {
        sampled++;
      }
      trace_destroy(trace);
      usleep(5000); /* 5ms between requests */
    }

    sampler_stats_t stats;
    sampler_get_stats(sampler, &stats);

    printf("Second %d: sampled %d requests (rate: %.2f%%)\n", sec + 1, sampled,
           100.0 * stats.current_rate);
  }

  printf("\n");
  sampler_destroy(sampler);
}

void demo_override_sampling(void) {
  printf("=== Endpoint Override Sampling ===\n\n");

  /* Base: 1% sampling */
  sampler_t *base = sampler_create_probability(0.01);

  /* Override: /api/critical at 100% */
  sampler_t *sampler = sampler_create_override(base, "/api/critical", 1.0);

  printf("Base sampling rate: 1%%\n");
  printf("Override for /api/critical: 100%%\n\n");

  /* Test normal endpoint */
  int normal_sampled = 0;
  for (int i = 0; i < 1000; i++) {
    trace_t *trace = trace_create_sampled(sampler, "/api/normal");
    if (trace->sampled) {
      normal_sampled++;
    }
    trace_destroy(trace);
  }

  /* Test critical endpoint */
  int critical_sampled = 0;
  for (int i = 0; i < 100; i++) {
    trace_t *trace = trace_create_sampled(sampler, "/api/critical");
    if (trace->sampled) {
      critical_sampled++;
      printf("  Critical trace sampled: ID=%016llx, reason='%s'\n", trace->id,
             trace->sampling_reason);
    }
    trace_destroy(trace);
  }

  printf("\nResults:\n");
  printf("  /api/normal: %d/1000 sampled (%.1f%%)\n", normal_sampled,
         100.0 * normal_sampled / 1000);
  printf("  /api/critical: %d/100 sampled (%.1f%%)\n\n", critical_sampled,
         100.0 * critical_sampled / 100);

  sampler_destroy(sampler);
  sampler_destroy(base);
}

int main(void) {
  printf("=== Sampling Strategies Demonstration ===\n\n");

  demo_probability_sampling();
  demo_adaptive_sampling();
  demo_override_sampling();

  printf("All sampling strategies demonstrated successfully!\n");

  return 0;
}
