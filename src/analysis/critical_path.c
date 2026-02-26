/**
 * critical_path.c - Critical path computation for traces
 *
 * The critical path is the chain of spans forming the longest
 * wall-clock dependency from root to leaf. It determines the
 * end-to-end latency of the trace.
 *
 * Algorithm: Recursive DFS on the span tree. At each node,
 * pick the child whose subtree has the maximum duration.
 */

#include "dapper/analysis.h"
#include "dapper/span.h"
#include <stdlib.h>

/**
 * Get span duration in microseconds (from deserialized span).
 * Deserialized spans have monotonic_start_ns=0, monotonic_end_ns=duration*1000.
 */
static uint64_t span_duration_us(const span_t *span) {
  if (!span) {
    return 0;
  }
  return (span->monotonic_end_ns - span->monotonic_start_ns) / 1000ULL;
}

/**
 * Count the depth of the critical path from this span downward.
 * Returns the number of spans in the longest path (including this span).
 */
static int critical_path_depth(const span_t *span) {
  if (!span) {
    return 0;
  }

  int max_child_depth = 0;
  for (span_t *child = span->first_child; child; child = child->next_sibling) {
    int d = critical_path_depth(child);
    if (d > max_child_depth) {
      max_child_depth = d;
    }
  }

  return 1 + max_child_depth;
}

/**
 * Find the critical child: the child with the largest duration
 * (its own duration acts as the tiebreaker for the critical path).
 */
static span_t *find_critical_child(const span_t *span) {
  if (!span) {
    return NULL;
  }

  span_t *critical = NULL;
  uint64_t max_duration = 0;

  for (span_t *child = span->first_child; child; child = child->next_sibling) {
    uint64_t d = span_duration_us(child);
    if (d > max_duration || critical == NULL) {
      max_duration = d;
      critical = child;
    }
  }

  return critical;
}

span_t **compute_critical_path(const trace_t *trace, int *out_length) {
  if (!trace || !out_length || !trace->root_span) {
    if (out_length)
      *out_length = 0;
    return NULL;
  }

  /* Count path depth */
  int depth = critical_path_depth(trace->root_span);
  if (depth == 0) {
    *out_length = 0;
    return NULL;
  }

  /* Allocate path array */
  span_t **path = calloc((size_t)depth, sizeof(span_t *));
  if (!path) {
    *out_length = 0;
    return NULL;
  }

  /* Walk the critical path from root to leaf */
  span_t *current = trace->root_span;
  int idx = 0;
  while (current && idx < depth) {
    path[idx++] = current;
    current = find_critical_child(current);
  }

  *out_length = idx;
  return path;
}

uint64_t critical_path_duration_us(const trace_t *trace) {
  if (!trace || !trace->root_span) {
    return 0;
  }
  return span_duration_us(trace->root_span);
}
