/**
 * span.h - Span lifecycle and annotation management
 */

#ifndef SPAN_H
#define SPAN_H

#include "types.h"

/**
 * Create a new span within a trace
 *
 * trace: The trace this span belongs to (required)
 * parent: Parent span, or NULL for root span
 * name: Human-readable span name (will be truncated to SPAN_NAME_MAX_LENGTH)
 *
 * Returns: Pointer to newly allocated span, or NULL on failure
 *
 * Notes:
 * - Automatically generates unique span_id
 * - Captures monotonic_start_ns timestamp
 * - Adds span to parent's child list if parent is non-NULL
 */
span_t *span_create(trace_t *trace, span_t *parent, const char *name);

/**
 * Add a key-value annotation to a span
 *
 * span: The span to annotate (required)
 * key: Annotation key (will be truncated if too long)
 * value: Annotation value (will be truncated if too long)
 *
 * Notes:
 * - Silently ignores annotation if count >= MAX_ANNOTATIONS
 * - Both key and value are copied (caller retains ownership)
 */
void span_annotate(span_t *span, const char *key, const char *value);

/**
 * Finish a span (capture end timestamp)
 *
 * span: The span to finish (required)
 *
 * Notes:
 * - Captures monotonic_end_ns timestamp
 * - Span should not be modified after finishing
 * - Safe to call multiple times (subsequent calls are no-ops)
 */
void span_finish(span_t *span);

/**
 * Get the duration of a span in nanoseconds
 *
 * span: The span to measure
 *
 * Returns: Duration in nanoseconds, or 0 if span not finished
 */
uint64_t span_duration_ns(const span_t *span);

#endif /* SPAN_H */
