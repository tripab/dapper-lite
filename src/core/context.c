/**
 * context.c - Trace context serialization/deserialization
 */

#include "dapper/context.h"
#include "dapper/byteorder.h"
#include "dapper/span.h"
#include "dapper/trace.h"
#include <string.h>

int context_inject(const span_t *span, uint8_t *buffer, size_t bufsize) {
  if (!span || !buffer || bufsize < TRACE_CONTEXT_WIRE_SIZE) {
    return -1;
  }

  /* Serialize in network byte order (big-endian) */
  uint64_t trace_id_net = dapper_hton64(span->trace_id);
  uint64_t span_id_net = dapper_hton64(span->span_id);

  memcpy(buffer, &trace_id_net, sizeof(uint64_t));
  memcpy(buffer + 8, &span_id_net, sizeof(uint64_t));

  /* Flags byte carries the head-based sampling decision. */
  buffer[16] = span->sampled ? TRACE_CONTEXT_FLAG_SAMPLED : 0;

  return TRACE_CONTEXT_WIRE_SIZE;
}

int context_extract(trace_context_t *ctx, const uint8_t *buffer,
                    size_t bufsize) {
  if (!ctx || !buffer || bufsize < TRACE_CONTEXT_WIRE_SIZE) {
    return -1;
  }

  /* Deserialize from network byte order */
  uint64_t trace_id_net, span_id_net;

  memcpy(&trace_id_net, buffer, sizeof(uint64_t));
  memcpy(&span_id_net, buffer + 8, sizeof(uint64_t));

  ctx->trace_id = dapper_ntoh64(trace_id_net);
  ctx->span_id = dapper_ntoh64(span_id_net);
  ctx->sampled = (buffer[16] & TRACE_CONTEXT_FLAG_SAMPLED) != 0;

  return 0;
}

span_t *span_create_from_context(trace_t *trace, const trace_context_t *ctx,
                                 const char *name) {
  if (!trace || !ctx || !name) {
    return NULL;
  }

  /* Update trace with the extracted trace_id and propagated sampling
   * decision before creating the span, so the new span inherits the
   * upstream head-based decision. */
  trace->id = ctx->trace_id;
  trace->sampled = ctx->sampled;

  /* Create span as root (no in-process parent) */
  span_t *span = span_create(trace, NULL, name);
  if (!span) {
    return NULL;
  }

  /* Set parent_span_id to the remote parent */
  span->parent_span_id = ctx->span_id;

  return span;
}
