/**
 * trace.c - Trace lifecycle implementation
 */

#include "dapper/trace.h"
#include "dapper/sampler.h"
#include "dapper/span.h"
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

/* Global trace ID counter (atomic for thread safety) */
static _Atomic uint64_t g_next_trace_id = 1;

/**
 * Generate a unique trace ID
 */
static trace_id_t generate_trace_id(void) {
  return atomic_fetch_add(&g_next_trace_id, 1);
}

trace_t *trace_create(void) {
  return trace_create_with_id(generate_trace_id());
}

trace_t *trace_create_sampled(struct sampler *sampler, const char *endpoint) {
  trace_t *trace = trace_create();
  if (!trace || !sampler) {
    return trace;
  }

  /* Make sampling decision */
  sampling_decision_t decision;
  if (sampler_should_sample(sampler, endpoint, &decision) == 0) {
    trace->sampled = decision.sampled;
    trace->sample_rate = decision.sample_rate;
    strncpy(trace->sampling_reason, decision.reason,
            sizeof(trace->sampling_reason) - 1);
    trace->sampling_reason[sizeof(trace->sampling_reason) - 1] = '\0';
  }

  return trace;
}

trace_t *trace_create_with_id(trace_id_t trace_id) {
  trace_t *trace = calloc(1, sizeof(trace_t));
  if (!trace) {
    return NULL;
  }

  trace->id = trace_id;
  trace->root_span = NULL;
  trace->all_spans = NULL;

  /* Initialize sampling fields (Phase 3) */
  trace->sampled = true; /* Default: sample everything */
  trace->sample_rate = 1.0;
  strcpy(trace->sampling_reason, "default(1.0)");

  return trace;
}

void trace_destroy(trace_t *trace) {
  if (!trace) {
    return;
  }

  /* Free every span via the ownership chain, independent of the
   * hierarchy, so orphan spans, multiple roots, and rootless traces
   * are all reclaimed. */
  span_t *span = trace->all_spans;
  while (span) {
    span_t *next = span->owner_next;
    free(span);
    span = next;
  }

  free(trace);
}
