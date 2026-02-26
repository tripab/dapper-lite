/**
 * aggregation.c - Per-service latency statistics
 *
 * Groups spans by name (proxy for service/endpoint) and computes
 * count, mean, p50, p99, min, and max latency in microseconds.
 */

#include "dapper/analysis.h"
#include <stdlib.h>
#include <string.h>

/* Maximum number of distinct span names we track */
#define MAX_GROUPS 256

/* Maximum spans per group for percentile computation */
#define MAX_SAMPLES 65536

typedef struct {
  char name[SPAN_NAME_MAX_LENGTH];
  double *latencies; /* Dynamic array of latency values */
  int count;
  int capacity;
} group_t;

static int find_or_create_group(group_t *groups, int *num_groups,
                                const char *name) {
  for (int i = 0; i < *num_groups; i++) {
    if (strcmp(groups[i].name, name) == 0) {
      return i;
    }
  }

  if (*num_groups >= MAX_GROUPS) {
    return -1;
  }

  int idx = *num_groups;
  strncpy(groups[idx].name, name, SPAN_NAME_MAX_LENGTH - 1);
  groups[idx].name[SPAN_NAME_MAX_LENGTH - 1] = '\0';
  groups[idx].count = 0;
  groups[idx].capacity = 128;
  groups[idx].latencies = calloc((size_t)groups[idx].capacity, sizeof(double));
  if (!groups[idx].latencies) {
    return -1;
  }
  (*num_groups)++;
  return idx;
}

static int add_sample(group_t *g, double latency_us) {
  if (g->count >= g->capacity) {
    if (g->capacity >= MAX_SAMPLES) {
      return -1; /* Cap reached */
    }
    int new_cap = g->capacity * 2;
    if (new_cap > MAX_SAMPLES) {
      new_cap = MAX_SAMPLES;
    }
    double *new_lat = realloc(g->latencies, (size_t)new_cap * sizeof(double));
    if (!new_lat) {
      return -1;
    }
    g->latencies = new_lat;
    g->capacity = new_cap;
  }
  g->latencies[g->count++] = latency_us;
  return 0;
}

/**
 * Walk all spans in a trace (DFS).
 */
static void collect_spans(const span_t *span, group_t *groups,
                          int *num_groups) {
  if (!span) {
    return;
  }

  uint64_t duration_us =
      (span->monotonic_end_ns - span->monotonic_start_ns) / 1000ULL;
  int idx = find_or_create_group(groups, num_groups, span->name);
  if (idx >= 0) {
    add_sample(&groups[idx], (double)duration_us);
  }

  /* Recurse into children */
  for (span_t *child = span->first_child; child; child = child->next_sibling) {
    collect_spans(child, groups, num_groups);
  }
}

static int cmp_double(const void *a, const void *b) {
  double da = *(const double *)a;
  double db = *(const double *)b;
  if (da < db)
    return -1;
  if (da > db)
    return 1;
  return 0;
}

static double percentile(double *sorted, int count, double pct) {
  if (count == 0) {
    return 0.0;
  }
  if (count == 1) {
    return sorted[0];
  }
  double rank = pct / 100.0 * (count - 1);
  int lo = (int)rank;
  int hi = lo + 1;
  if (hi >= count) {
    hi = count - 1;
  }
  double frac = rank - lo;
  return sorted[lo] + frac * (sorted[hi] - sorted[lo]);
}

service_stats_t *aggregate_by_service(trace_t **traces, int trace_count,
                                      int *out_count) {
  if (!traces || trace_count <= 0 || !out_count) {
    if (out_count)
      *out_count = 0;
    return NULL;
  }

  group_t groups[MAX_GROUPS];
  int num_groups = 0;
  memset(groups, 0, sizeof(groups));

  /* Collect latency samples from all traces */
  for (int i = 0; i < trace_count; i++) {
    if (traces[i] && traces[i]->root_span) {
      collect_spans(traces[i]->root_span, groups, &num_groups);
    }
  }

  if (num_groups == 0) {
    *out_count = 0;
    return NULL;
  }

  /* Compute statistics for each group */
  service_stats_t *stats = calloc((size_t)num_groups, sizeof(service_stats_t));
  if (!stats) {
    for (int i = 0; i < num_groups; i++) {
      free(groups[i].latencies);
    }
    *out_count = 0;
    return NULL;
  }

  for (int i = 0; i < num_groups; i++) {
    strncpy(stats[i].name, groups[i].name, SPAN_NAME_MAX_LENGTH - 1);
    stats[i].name[SPAN_NAME_MAX_LENGTH - 1] = '\0';
    stats[i].count = groups[i].count;

    if (groups[i].count > 0) {
      /* Sort for percentile computation */
      qsort(groups[i].latencies, (size_t)groups[i].count, sizeof(double),
            cmp_double);

      /* Mean */
      double sum = 0.0;
      for (int j = 0; j < groups[i].count; j++) {
        sum += groups[i].latencies[j];
      }
      stats[i].mean_latency_us = sum / groups[i].count;

      /* Percentiles */
      stats[i].p50_latency_us =
          percentile(groups[i].latencies, groups[i].count, 50.0);
      stats[i].p99_latency_us =
          percentile(groups[i].latencies, groups[i].count, 99.0);

      /* Min/Max */
      stats[i].min_latency_us = groups[i].latencies[0];
      stats[i].max_latency_us = groups[i].latencies[groups[i].count - 1];
    }

    free(groups[i].latencies);
  }

  *out_count = num_groups;
  return stats;
}
