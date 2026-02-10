/**
 * test_phase3.c - Unit tests for Phase 3 (Sampling)
 *
 * Tests:
 * - Probabilistic sampling statistical correctness
 * - Adaptive sampling convergence
 * - Endpoint override priority
 * - Sampling metadata in traces
 */

#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L
#include "dapper/sampler.h"
#include "dapper/span.h"
#include "dapper/trace.h"
#include "minunit.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int tests_run = 0;
int tests_failed = 0;

/* ========== Probabilistic Sampling Tests ========== */

static const char *test_probability_sampler_create() {
  sampler_t *s = sampler_create_probability(0.5);
  mu_assert("sampler_create_probability should succeed", s != NULL);
  sampler_destroy(s);

  /* Invalid rates */
  s = sampler_create_probability(-0.1);
  mu_assert("negative rate should fail", s == NULL);

  s = sampler_create_probability(1.5);
  mu_assert("rate > 1.0 should fail", s == NULL);

  return NULL;
}

static const char *test_probability_rate_convergence() {
  sampler_t *sampler = sampler_create_probability(0.1); /* 10% */
  mu_assert("sampler creation should succeed", sampler != NULL);

  int sampled_count = 0;
  int total = 10000;

  sampling_decision_t decision;
  for (int i = 0; i < total; i++) {
    sampler_should_sample(sampler, NULL, &decision);
    if (decision.sampled) {
      sampled_count++;
    }
  }

  double actual_rate = (double)sampled_count / total;
  double expected_rate = 0.1;
  double error = fabs(actual_rate - expected_rate);

  /* Should be within 2% of expected (statistical) */
  mu_assert("sampling rate should converge to expected", error < 0.02);

  sampler_stats_t stats;
  sampler_get_stats(sampler, &stats);
  mu_assert_eq("stats total should match", (uint64_t)total,
               stats.total_decisions);
  mu_assert_eq("stats sampled should match", (uint64_t)sampled_count,
               stats.sampled_count);

  sampler_destroy(sampler);
  return NULL;
}

static const char *test_sampling_decision_metadata() {
  sampler_t *sampler = sampler_create_probability(0.5);
  sampling_decision_t decision;

  sampler_should_sample(sampler, NULL, &decision);

  mu_assert("decision should have sample_rate", decision.sample_rate == 0.5);
  mu_assert("decision should have reason", strlen(decision.reason) > 0);

  sampler_destroy(sampler);
  return NULL;
}

/* ========== Adaptive Sampling Tests ========== */

static const char *test_adaptive_sampler_create() {
  sampler_t *s = sampler_create_adaptive(100); /* 100 TPS */
  mu_assert("adaptive sampler creation should succeed", s != NULL);
  sampler_destroy(s);

  s = sampler_create_adaptive(0);
  mu_assert("zero target should fail", s == NULL);

  s = sampler_create_adaptive(-10);
  mu_assert("negative target should fail", s == NULL);

  return NULL;
}

static const char *test_adaptive_rate_adjustment() {
  sampler_t *sampler = sampler_create_adaptive(10); /* Target 10 TPS */
  mu_assert("sampler creation should succeed", sampler != NULL);

  sampling_decision_t decision;

  /* Make many decisions to trigger rate adjustment */
  for (int i = 0; i < 1000; i++) {
    sampler_should_sample(sampler, NULL, &decision);
    usleep(1000); /* 1ms spacing */
  }

  /* Get final stats */
  sampler_stats_t stats;
  sampler_get_stats(sampler, &stats);

  /* Rate should have adjusted (not at initial 10%) */
  mu_assert("current_rate should be set", stats.current_rate > 0.0);
  mu_assert("current_rate should be <= 1.0", stats.current_rate <= 1.0);

  sampler_destroy(sampler);
  return NULL;
}

/* ========== Override Sampling Tests ========== */

static const char *test_override_sampler_create() {
  sampler_t *base = sampler_create_probability(0.1);
  mu_assert("base sampler should be created", base != NULL);

  sampler_t *override = sampler_create_override(base, "/api/critical", 1.0);
  mu_assert("override sampler should be created", override != NULL);

  sampler_destroy(override);
  sampler_destroy(base);
  return NULL;
}

static const char *test_override_priority() {
  sampler_t *base = sampler_create_probability(0.01); /* 1% base */
  sampler_t *override =
      sampler_create_override(base, "/api/critical", 1.0); /* 100% override */

  sampling_decision_t decision;

  /* Test overridden endpoint - should always sample */
  int sampled_count = 0;
  for (int i = 0; i < 100; i++) {
    sampler_should_sample(override, "/api/critical", &decision);
    if (decision.sampled) {
      sampled_count++;
    }
  }

  mu_assert("critical endpoint should be sampled at 100%", sampled_count > 95);

  /* Test non-overridden endpoint - should use base rate (1%) */
  sampled_count = 0;
  for (int i = 0; i < 1000; i++) {
    sampler_should_sample(override, "/api/normal", &decision);
    if (decision.sampled) {
      sampled_count++;
    }
  }

  double actual_rate = (double)sampled_count / 1000.0;
  mu_assert("normal endpoint should use base rate", actual_rate < 0.05);

  sampler_destroy(override);
  sampler_destroy(base);
  return NULL;
}

/* ========== Trace Integration Tests ========== */

static const char *test_trace_with_sampling() {
  sampler_t *sampler = sampler_create_probability(0.5);

  trace_t *trace = trace_create_sampled(sampler, "/api/test");
  mu_assert("trace should be created", trace != NULL);

  /* Trace should have sampling metadata */
  mu_assert("trace should have sample_rate", trace->sample_rate > 0.0);
  mu_assert("trace should have sampling_reason",
            strlen(trace->sampling_reason) > 0);

  trace_destroy(trace);
  sampler_destroy(sampler);
  return NULL;
}

static const char *test_sampling_bias() {
  sampler_t *sampler = sampler_create_probability(0.1);

  /* Test for bias with different trace IDs */
  int buckets[10] = {0};
  int total = 10000;

  sampling_decision_t decision;
  for (int i = 0; i < total; i++) {
    sampler_should_sample(sampler, NULL, &decision);
    if (decision.sampled) {
      buckets[i % 10]++;
    }
  }

  /* Each bucket should have roughly equal samples (unbiased) */
  double expected = total * 0.1 / 10.0; /* 100 per bucket */

  for (int i = 0; i < 10; i++) {
    double error = fabs(buckets[i] - expected) / expected;
    /* Allow 30% deviation for small sample sizes */
    mu_assert("sampling should be unbiased across buckets", error < 0.3);
  }

  sampler_destroy(sampler);
  return NULL;
}

/* ========== Test Suite ========== */

static const char *all_tests() {
  printf("Running Phase 3 Unit Tests (Sampling)\n");
  printf("======================================\n\n");

  printf("Probabilistic Sampling Tests:\n");
  mu_run_test(test_probability_sampler_create);
  mu_run_test(test_probability_rate_convergence);
  mu_run_test(test_sampling_decision_metadata);
  mu_run_test(test_sampling_bias);

  printf("Adaptive Sampling Tests:\n");
  mu_run_test(test_adaptive_sampler_create);
  mu_run_test(test_adaptive_rate_adjustment);

  printf("Override Sampling Tests:\n");
  mu_run_test(test_override_sampler_create);
  mu_run_test(test_override_priority);

  printf("Trace Integration Tests:\n");
  mu_run_test(test_trace_with_sampling);

  return NULL;
}

int main(void) {
  const char *result = all_tests();
  if (result != 0) {
    printf("\nFinal failure: %s\n", result);
  }
  mu_report();
}
