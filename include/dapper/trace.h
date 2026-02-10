/**
 * trace.h - Trace lifecycle management
 */

#ifndef TRACE_H
#define TRACE_H

#include "types.h"

/* Forward declarations */
struct sampler;

/**
 * Create a new trace with a unique ID
 *
 * Returns: Pointer to newly allocated trace, or NULL on failure
 */
trace_t *trace_create(void);

/**
 * Create a trace with a specific ID (used for cross-process continuity)
 *
 * trace_id: The trace ID to use
 * Returns: Pointer to newly allocated trace, or NULL on failure
 */
trace_t *trace_create_with_id(trace_id_t trace_id);

/**
 * Create a trace with sampling decision (Phase 3)
 *
 * sampler: Sampler to use for decision
 * endpoint: Endpoint name (or NULL)
 *
 * Returns: Pointer to newly allocated trace, or NULL on failure
 *
 * The trace will have sampled, sample_rate, and sampling_reason set
 * based on the sampler's decision.
 */
trace_t *trace_create_sampled(struct sampler *sampler, const char *endpoint);

/**
 * Destroy a trace and all its spans
 *
 * This frees all memory associated with the trace, including all spans
 * in its hierarchy. The trace pointer becomes invalid after this call.
 *
 * trace: Trace to destroy (NULL is safe)
 */
void trace_destroy(trace_t *trace);

#endif /* TRACE_H */
