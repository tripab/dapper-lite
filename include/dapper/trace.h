/**
 * trace.h - Trace lifecycle management
 */

#ifndef TRACE_H
#define TRACE_H

#include "types.h"

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
 * Destroy a trace and all its spans
 *
 * This frees all memory associated with the trace, including all spans
 * in its hierarchy. The trace pointer becomes invalid after this call.
 *
 * trace: Trace to destroy (NULL is safe)
 */
void trace_destroy(trace_t *trace);

#endif /* TRACE_H */
