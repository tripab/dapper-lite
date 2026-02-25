/**
 * protocol.c - Collector-side wire format decoding
 *
 * Wraps the existing span_deserialize() with collector-specific
 * validation: minimum packet size, non-zero IDs, etc.
 */

#include "dapper/collector.h"
#include "dapper/exporter.h"
#include <string.h>

int collector_decode_span(const uint8_t *data, size_t len, span_t *span,
                          bool *sampled) {
  if (!data || !span || !sampled) {
    return -1;
  }

  /* Reject packets smaller than wire header */
  if (len < SPAN_WIRE_HEADER_SIZE) {
    return -1;
  }

  /* Reject oversized packets */
  if (len > COLLECTOR_UDP_MAX_PACKET) {
    return -1;
  }

  /* Delegate to existing deserializer */
  int consumed = span_deserialize(data, len, span, sampled);
  if (consumed < 0) {
    return -1;
  }

  /* Collector-specific validation */
  if (span->trace_id == 0) {
    return -1;
  }
  if (span->span_id == 0) {
    return -1;
  }

  return 0;
}
