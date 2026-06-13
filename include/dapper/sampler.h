/**
 * sampler.h - Sampling decision logic
 *
 * Provides various sampling strategies:
 * - Probabilistic (fixed rate)
 * - Adaptive (target traces/sec)
 * - Endpoint overrides
 */

#ifndef DAPPER_SAMPLER_H
#define DAPPER_SAMPLER_H

#include "types.h"
#include <stdbool.h>

/**
 * Sampler type - opaque structure
 */
typedef struct sampler sampler_t;

/**
 * Sampling decision - returned when sampling
 */
typedef struct {
  bool sampled;       /* Should this trace be sampled? */
  double sample_rate; /* Rate used for decision (0.0 - 1.0) */
  char reason[64];    /* Why sampled/dropped (for debugging) */
} sampling_decision_t;

/**
 * Create a probabilistic sampler (fixed rate)
 *
 * rate: Sampling rate between 0.0 and 1.0
 *       0.0 = sample nothing
 *       1.0 = sample everything
 *       0.01 = sample 1%
 *
 * Returns: New sampler, or NULL on error
 *
 * Example:
 *   sampler_t* s = sampler_create_probability(0.01);  // 1% sampling
 */
sampler_t *sampler_create_probability(double rate);

/**
 * Create an adaptive sampler (target traces/sec)
 *
 * Automatically adjusts sampling rate to achieve target throughput.
 * Uses exponential moving average for stability.
 *
 * target_tps: Target traces per second (e.g., 100)
 *
 * Returns: New sampler, or NULL on error
 *
 * Example:
 *   sampler_t* s = sampler_create_adaptive(100);  // Target 100 traces/sec
 */
sampler_t *sampler_create_adaptive(int target_tps);

/**
 * Create a sampler with endpoint-specific overrides
 *
 * Wraps a base sampler with per-endpoint rate overrides.
 *
 * base: Base sampler to use for non-overridden endpoints
 * endpoint: Endpoint name to override (copied)
 * rate: Override rate for this endpoint (0.0 - 1.0)
 *
 * Returns: New sampler, or NULL on error
 *
 * Example:
 *   sampler_t* base = sampler_create_probability(0.01);
 *   sampler_t* s = sampler_create_override(base, "/api/critical", 1.0);
 *   // Now /api/critical is sampled at 100%, others at 1%
 */
sampler_t *sampler_create_override(sampler_t *base, const char *endpoint,
                                   double rate);

/**
 * Make a sampling decision
 *
 * sampler: The sampler to use
 * endpoint: Endpoint name (or NULL for no endpoint)
 * decision: Output - filled with sampling decision
 *
 * Returns: 0 on success, -1 on error
 *
 * Thread-safe: Yes (uses atomic operations internally)
 */
int sampler_should_sample(sampler_t *sampler, const char *endpoint,
                          sampling_decision_t *decision);

/**
 * Destroy a sampler and free resources
 *
 * For override samplers, this does NOT destroy the base sampler.
 * Caller must destroy base sampler separately.
 *
 * sampler: Sampler to destroy (NULL is safe)
 */
void sampler_destroy(sampler_t *sampler);

/**
 * Get sampler statistics (for debugging/monitoring)
 */
typedef struct {
  uint64_t total_decisions; /* Total sampling decisions made */
  uint64_t sampled_count;   /* Number sampled */
  uint64_t dropped_count;   /* Number dropped */
  double current_rate;      /* Current sampling rate (adaptive only) */
} sampler_stats_t;

int sampler_get_stats(sampler_t *sampler, sampler_stats_t *stats);

/* The sampler type (probability / adaptive / override) is dispatched
 * internally by sampler_should_sample(), sampler_destroy(), and
 * sampler_get_stats(). There are intentionally no exported
 * type-specific entry points: earlier versions declared
 * sampler_should_sample_adaptive/_override and destroy/stats variants
 * that were never implemented, which broke external linkers. */

#endif /* DAPPER_SAMPLER_H */
