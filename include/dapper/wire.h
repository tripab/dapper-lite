/**
 * wire.h - Neutral span wire-format codec
 *
 * Defines the on-the-wire / on-disk span encoding shared by the
 * exporter (producer), the collector (receiver/storage), and the
 * analysis layer (storage reader). Keeping the codec in its own
 * module means a wire-format change touches one place instead of
 * reaching through the exporter header into unrelated layers.
 *
 * Wire format (all multi-byte integers big-endian / network order):
 *   Header (48 bytes):
 *     [0..7]   trace_id        (uint64)
 *     [8..15]  span_id         (uint64)
 *     [16..23] parent_span_id  (uint64)
 *     [24..31] start_ts        (uint64, microseconds since epoch)
 *     [32..39] duration_us     (uint64)
 *     [40]     sampled         (uint8, 1=yes 0=no)
 *     [41]     flags           (uint8, reserved)
 *     [42..43] name_len        (uint16)
 *     [44..45] num_annotations (uint16)
 *     [46..47] reserved        (uint16)
 *   Variable section:
 *     [48..48+name_len)  span name (UTF-8, no null terminator)
 *     Annotations (num_annotations times):
 *       [2 bytes] key_len, [key_len] key,
 *       [2 bytes] value_len, [value_len] value
 */

#ifndef DAPPER_WIRE_H
#define DAPPER_WIRE_H

#include "types.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ============================================================
 * Wire Format Constants
 * ============================================================ */

#define SPAN_WIRE_HEADER_SIZE 48
#define SPAN_WIRE_MAX_SIZE 256

/* Wire format header offsets */
#define WIRE_OFF_TRACE_ID 0
#define WIRE_OFF_SPAN_ID 8
#define WIRE_OFF_PARENT_SPAN_ID 16
#define WIRE_OFF_START_TS 24
#define WIRE_OFF_DURATION_US 32
#define WIRE_OFF_SAMPLED 40
#define WIRE_OFF_FLAGS 41
#define WIRE_OFF_NAME_LEN 42
#define WIRE_OFF_NUM_ANNOTATIONS 44
#define WIRE_OFF_RESERVED 46

/* ============================================================
 * Serialization API
 * ============================================================ */

/**
 * Serialize a span into wire format bytes.
 *
 * All multi-byte integers are big-endian (network order).
 *
 * buffer: Output buffer (must be at least SPAN_WIRE_HEADER_SIZE bytes)
 * bufsize: Size of output buffer (typically SPAN_WIRE_MAX_SIZE)
 * span: The span to serialize
 * sampled: Sampling decision (from trace)
 *
 * Returns: Number of bytes written (>0), or -1 on error.
 *          Annotations are truncated if they exceed buffer space.
 */
int span_serialize(uint8_t *buffer, size_t bufsize, const span_t *span,
                   bool sampled);

/**
 * Deserialize wire format bytes back into span fields.
 *
 * buffer: Input buffer containing wire format data
 * bufsize: Size of input buffer
 * span: Output span (caller-allocated, will be populated)
 * sampled: Output sampling decision
 *
 * Returns: Number of bytes consumed (>0), or -1 on error.
 *
 * Note: Hierarchy pointers (parent, first_child, next_sibling,
 *       owner_next) are set to NULL since they are not part of the
 *       wire format.
 */
int span_deserialize(const uint8_t *buffer, size_t bufsize, span_t *span,
                     bool *sampled);

#endif /* DAPPER_WIRE_H */
