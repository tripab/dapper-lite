/**
 * context.h - Trace context propagation across process boundaries
 *
 * Provides serialization and deserialization of trace context for RPC.
 */

#ifndef DAPPER_CONTEXT_H
#define DAPPER_CONTEXT_H

#include "types.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/**
 * Trace context - Serializable data for cross-process propagation
 *
 * This is the minimal information needed to continue a trace
 * across process boundaries (e.g., in RPC headers).
 */
typedef struct {
  trace_id_t trace_id;
  span_id_t span_id; /* Parent span ID for the receiving side */
  bool sampled;      /* Head-based sampling decision, propagated */
} trace_context_t;

/**
 * Wire format constants
 *
 * 8 bytes trace_id + 8 bytes span_id + 1 byte flags (bit0 = sampled).
 * Carrying the sampling decision keeps head-based sampling consistent
 * across process boundaries.
 */
#define TRACE_CONTEXT_WIRE_SIZE 17
#define TRACE_CONTEXT_FLAG_SAMPLED 0x01

/**
 * Inject trace context into a buffer (serialize)
 *
 * Serializes the span's trace context into network byte order
 * suitable for transmission over RPC.
 *
 * span: The span whose context to serialize
 * buffer: Output buffer (must be at least TRACE_CONTEXT_WIRE_SIZE bytes)
 * bufsize: Size of output buffer
 *
 * Returns: Number of bytes written, or -1 on error
 */
int context_inject(const span_t *span, uint8_t *buffer, size_t bufsize);

/**
 * Extract trace context from a buffer (deserialize)
 *
 * Deserializes trace context from network byte order.
 *
 * ctx: Output context structure
 * buffer: Input buffer containing serialized context
 * bufsize: Size of input buffer
 *
 * Returns: 0 on success, -1 on error
 */
int context_extract(trace_context_t *ctx, const uint8_t *buffer,
                    size_t bufsize);

/**
 * Create a span from extracted context
 *
 * Creates a new span that continues a trace from another process.
 * The new span will have the extracted trace_id and will be a child
 * of the span_id from the context.
 *
 * trace: Trace to create span in (will be updated with extracted trace_id)
 * ctx: Extracted trace context
 * name: Name for the new span
 *
 * Returns: Newly created span, or NULL on error
 */
span_t *span_create_from_context(trace_t *trace, const trace_context_t *ctx,
                                 const char *name);

#endif /* DAPPER_CONTEXT_H */
