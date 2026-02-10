/**
 * sampler.c - Unified sampler implementation with polymorphic dispatch
 */

#define _POSIX_C_SOURCE 200809L
#include "dapper/sampler.h"
#include <math.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ========== Sampler Type Tags ========== */

typedef enum {
  SAMPLER_TYPE_PROBABILITY,
  SAMPLER_TYPE_ADAPTIVE,
  SAMPLER_TYPE_OVERRIDE
} sampler_type_t;

/* ========== Base Sampler Structure ========== */

struct sampler {
  sampler_type_t type;
  void *impl; /* Type-specific implementation */
};

/* ========== Probability Sampler ========== */

/* Fast PRNG state (xorshift64) - shared across all samplers */
static _Atomic uint64_t g_rng_state = 88172645463325252ULL;

/**
 * Fast PRNG - xorshift64*
 */
static uint64_t xorshift64(void) {
  static _Atomic bool initialized = false;
  if (!atomic_load(&initialized)) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    uint64_t seed = (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
    atomic_store(&g_rng_state, seed);
    atomic_store(&initialized, true);
  }

  uint64_t x = atomic_load(&g_rng_state);
  x ^= x >> 12;
  x ^= x << 25;
  x ^= x >> 27;
  atomic_store(&g_rng_state, x);
  return x * 0x2545F4914F6CDD1DULL;
}

typedef struct {
  double rate;
  _Atomic uint64_t total_decisions;
  _Atomic uint64_t sampled_count;
} prob_sampler_t;

sampler_t *sampler_create_probability(double rate) {
  if (rate < 0.0 || rate > 1.0) {
    return NULL;
  }

  sampler_t *sampler = malloc(sizeof(sampler_t));
  if (!sampler) {
    return NULL;
  }

  prob_sampler_t *impl = calloc(1, sizeof(prob_sampler_t));
  if (!impl) {
    free(sampler);
    return NULL;
  }

  impl->rate = rate;
  atomic_store(&impl->total_decisions, 0);
  atomic_store(&impl->sampled_count, 0);

  sampler->type = SAMPLER_TYPE_PROBABILITY;
  sampler->impl = impl;

  return sampler;
}

static int prob_should_sample(prob_sampler_t *s, const char *endpoint,
                              sampling_decision_t *decision) {
  atomic_fetch_add(&s->total_decisions, 1);

  uint64_t random = xorshift64();
  double threshold = (double)random / (double)UINT64_MAX;
  bool sampled = threshold < s->rate;

  if (sampled) {
    atomic_fetch_add(&s->sampled_count, 1);
  }

  decision->sampled = sampled;
  decision->sample_rate = s->rate;
  snprintf(decision->reason, sizeof(decision->reason), "probabilistic(%.4f)",
           s->rate);

  (void)endpoint;
  return 0;
}

static int prob_get_stats(prob_sampler_t *s, sampler_stats_t *stats) {
  stats->total_decisions = atomic_load(&s->total_decisions);
  stats->sampled_count = atomic_load(&s->sampled_count);
  stats->dropped_count = stats->total_decisions - stats->sampled_count;
  stats->current_rate = s->rate;
  return 0;
}

/* ========== Adaptive Sampler ========== */

#define WINDOW_SIZE_SEC 10

typedef struct {
  int target_tps;
  _Atomic double current_rate;
  _Atomic uint64_t window_sampled[WINDOW_SIZE_SEC];
  _Atomic uint64_t window_total[WINDOW_SIZE_SEC];
  _Atomic int64_t last_update_sec;
  _Atomic uint64_t total_decisions;
  _Atomic uint64_t sampled_count;
} adaptive_sampler_t;

static int64_t get_current_sec(void) {
  struct timespec ts;
  clock_gettime(CLOCK_REALTIME, &ts);
  return ts.tv_sec;
}

static void adaptive_update_rate(adaptive_sampler_t *s) {
  int64_t now = get_current_sec();
  int64_t last = atomic_load(&s->last_update_sec);

  if (now == last) {
    return;
  }

  if (!atomic_compare_exchange_strong(&s->last_update_sec, &last, now)) {
    return;
  }

  uint64_t total_sampled = 0;
  // uint64_t total_requests = 0;

  for (int i = 0; i < WINDOW_SIZE_SEC; i++) {
    total_sampled += atomic_load(&s->window_sampled[i]);
    // total_requests += atomic_load(&s->window_total[i]);
  }

  int slot = (int)(now % WINDOW_SIZE_SEC);
  atomic_store(&s->window_sampled[slot], 0);
  atomic_store(&s->window_total[slot], 0);

  double current_tps = (double)total_sampled / WINDOW_SIZE_SEC;
  double current_rate = atomic_load(&s->current_rate);
  double new_rate = current_rate;

  if (current_tps > s->target_tps * 1.1) {
    new_rate = current_rate * (s->target_tps / current_tps) * 0.9;
  } else if (current_tps < s->target_tps * 0.9) {
    new_rate = fmin(1.0, current_rate * 1.1);
  }

  new_rate = fmax(0.0001, fmin(1.0, new_rate));
  atomic_store(&s->current_rate, new_rate);
}

sampler_t *sampler_create_adaptive(int target_tps) {
  if (target_tps <= 0) {
    return NULL;
  }

  sampler_t *sampler = malloc(sizeof(sampler_t));
  if (!sampler) {
    return NULL;
  }

  adaptive_sampler_t *impl = calloc(1, sizeof(adaptive_sampler_t));
  if (!impl) {
    free(sampler);
    return NULL;
  }

  impl->target_tps = target_tps;
  atomic_store(&impl->current_rate, 0.1);
  atomic_store(&impl->last_update_sec, get_current_sec());
  atomic_store(&impl->total_decisions, 0);
  atomic_store(&impl->sampled_count, 0);

  for (int i = 0; i < WINDOW_SIZE_SEC; i++) {
    atomic_store(&impl->window_sampled[i], 0);
    atomic_store(&impl->window_total[i], 0);
  }

  sampler->type = SAMPLER_TYPE_ADAPTIVE;
  sampler->impl = impl;

  return sampler;
}

static int adaptive_should_sample(adaptive_sampler_t *s, const char *endpoint,
                                  sampling_decision_t *decision) {
  adaptive_update_rate(s);

  double rate = atomic_load(&s->current_rate);
  uint64_t random = xorshift64();
  double threshold = (double)random / (double)UINT64_MAX;
  bool sampled = threshold < rate;

  atomic_fetch_add(&s->total_decisions, 1);

  int64_t now = get_current_sec();
  int slot = (int)(now % WINDOW_SIZE_SEC);
  atomic_fetch_add(&s->window_total[slot], 1);

  if (sampled) {
    atomic_fetch_add(&s->sampled_count, 1);
    atomic_fetch_add(&s->window_sampled[slot], 1);
  }

  decision->sampled = sampled;
  decision->sample_rate = rate;
  snprintf(decision->reason, sizeof(decision->reason),
           "adaptive(target=%d,rate=%.4f)", s->target_tps, rate);

  (void)endpoint;
  return 0;
}

static int adaptive_get_stats(adaptive_sampler_t *s, sampler_stats_t *stats) {
  stats->total_decisions = atomic_load(&s->total_decisions);
  stats->sampled_count = atomic_load(&s->sampled_count);
  stats->dropped_count = stats->total_decisions - stats->sampled_count;
  stats->current_rate = atomic_load(&s->current_rate);
  return 0;
}

/* ========== Override Sampler ========== */

#define MAX_OVERRIDES 16

typedef struct {
  char endpoint[128];
  double rate;
  sampler_t *override_sampler;
} override_entry_t;

typedef struct {
  sampler_t *base;
  override_entry_t overrides[MAX_OVERRIDES];
  int num_overrides;
} override_sampler_t;

sampler_t *sampler_create_override(sampler_t *base, const char *endpoint,
                                   double rate) {
  if (!base || !endpoint || rate < 0.0 || rate > 1.0) {
    return NULL;
  }

  sampler_t *sampler = malloc(sizeof(sampler_t));
  if (!sampler) {
    return NULL;
  }

  override_sampler_t *impl = calloc(1, sizeof(override_sampler_t));
  if (!impl) {
    free(sampler);
    return NULL;
  }

  impl->base = base;
  impl->num_overrides = 0;

  if (impl->num_overrides < MAX_OVERRIDES) {
    override_entry_t *entry = &impl->overrides[impl->num_overrides];
    strncpy(entry->endpoint, endpoint, sizeof(entry->endpoint) - 1);
    entry->endpoint[sizeof(entry->endpoint) - 1] = '\0';
    entry->rate = rate;
    entry->override_sampler = sampler_create_probability(rate);

    if (!entry->override_sampler) {
      free(impl);
      free(sampler);
      return NULL;
    }

    impl->num_overrides++;
  }

  sampler->type = SAMPLER_TYPE_OVERRIDE;
  sampler->impl = impl;

  return sampler;
}

static int override_should_sample(override_sampler_t *s, const char *endpoint,
                                  sampling_decision_t *decision);

static int override_get_stats(override_sampler_t *s, sampler_stats_t *stats);

/* ========== Unified Interface ========== */

int sampler_should_sample(sampler_t *sampler, const char *endpoint,
                          sampling_decision_t *decision) {
  if (!sampler || !decision) {
    return -1;
  }

  switch (sampler->type) {
  case SAMPLER_TYPE_PROBABILITY:
    return prob_should_sample((prob_sampler_t *)sampler->impl, endpoint,
                              decision);
  case SAMPLER_TYPE_ADAPTIVE:
    return adaptive_should_sample((adaptive_sampler_t *)sampler->impl, endpoint,
                                  decision);
  case SAMPLER_TYPE_OVERRIDE:
    return override_should_sample((override_sampler_t *)sampler->impl, endpoint,
                                  decision);
  default:
    return -1;
  }
}

static int override_should_sample(override_sampler_t *s, const char *endpoint,
                                  sampling_decision_t *decision) {
  if (endpoint) {
    for (int i = 0; i < s->num_overrides; i++) {
      if (strcmp(s->overrides[i].endpoint, endpoint) == 0) {
        int result = sampler_should_sample(s->overrides[i].override_sampler,
                                           endpoint, decision);
        if (result == 0) {
          snprintf(decision->reason, sizeof(decision->reason),
                   "override[%s](%.4f)", endpoint, s->overrides[i].rate);
        }
        return result;
      }
    }
  }

  return sampler_should_sample(s->base, endpoint, decision);
}

void sampler_destroy(sampler_t *sampler) {
  if (!sampler) {
    return;
  }

  if (sampler->type == SAMPLER_TYPE_OVERRIDE) {
    override_sampler_t *impl = (override_sampler_t *)sampler->impl;
    for (int i = 0; i < impl->num_overrides; i++) {
      sampler_destroy(impl->overrides[i].override_sampler);
    }
    /* Note: We don't destroy base - caller owns it */
  }

  free(sampler->impl);
  free(sampler);
}

int sampler_get_stats(sampler_t *sampler, sampler_stats_t *stats) {
  if (!sampler || !stats) {
    return -1;
  }

  switch (sampler->type) {
  case SAMPLER_TYPE_PROBABILITY:
    return prob_get_stats((prob_sampler_t *)sampler->impl, stats);
  case SAMPLER_TYPE_ADAPTIVE:
    return adaptive_get_stats((adaptive_sampler_t *)sampler->impl, stats);
  case SAMPLER_TYPE_OVERRIDE:
    return override_get_stats((override_sampler_t *)sampler->impl, stats);
  default:
    return -1;
  }
}

static int override_get_stats(override_sampler_t *s, sampler_stats_t *stats) {
  /* Delegate to base sampler */
  return sampler_get_stats(s->base, stats);
}
