/**
 * serialize.c - Span wire format serialization/deserialization
 *
 * Wire format (Appendix B):
 *   Header (48 bytes, all multi-byte integers big-endian):
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
 *
 *   Variable section:
 *     [48..48+name_len)  span name (UTF-8, no null terminator)
 *     Annotations (repeated num_annotations times):
 *       [2 bytes] key_len   (uint16)
 *       [key_len] key       (UTF-8)
 *       [2 bytes] value_len (uint16)
 *       [value_len] value   (UTF-8)
 */

#include "dapper/byteorder.h"
#include "dapper/wire.h"
#include <arpa/inet.h>
#include <string.h>

/* ---- Write helpers ---- */

static int write_u64(uint8_t *buf, size_t off, size_t bufsize, uint64_t val) {
  if (off + 8 > bufsize)
    return -1;
  uint64_t net = dapper_hton64(val);
  memcpy(buf + off, &net, 8);
  return 0;
}

static int write_u16(uint8_t *buf, size_t off, size_t bufsize, uint16_t val) {
  if (off + 2 > bufsize)
    return -1;
  uint16_t net = htons(val);
  memcpy(buf + off, &net, 2);
  return 0;
}

static int write_u8(uint8_t *buf, size_t off, size_t bufsize, uint8_t val) {
  if (off + 1 > bufsize)
    return -1;
  buf[off] = val;
  return 0;
}

/* ---- Read helpers ---- */

static uint64_t read_u64(const uint8_t *buf, size_t off) {
  uint64_t net;
  memcpy(&net, buf + off, 8);
  return dapper_ntoh64(net);
}

static uint16_t read_u16(const uint8_t *buf, size_t off) {
  uint16_t net;
  memcpy(&net, buf + off, 2);
  return ntohs(net);
}

static uint8_t read_u8(const uint8_t *buf, size_t off) { return buf[off]; }

/* ---- Serialization helpers ---- */

/* Name length to encode: actual strlen, clamped to the field limit and
 * to whatever space remains after the header. */
static size_t serialize_name_len(const span_t *span, size_t bufsize) {
  size_t name_len = strlen(span->name);
  if (name_len > SPAN_NAME_MAX_LENGTH - 1) {
    name_len = SPAN_NAME_MAX_LENGTH - 1;
  }
  if (SPAN_WIRE_HEADER_SIZE + name_len > bufsize) {
    name_len = bufsize - SPAN_WIRE_HEADER_SIZE;
  }
  return name_len;
}

/* Number of annotations that fit in the buffer after the name. */
static int serialize_annotation_count(const span_t *span, size_t bufsize,
                                      size_t start_pos) {
  size_t pos = start_pos;
  int n = 0;
  for (int i = 0; i < span->annotation_count; i++) {
    size_t ann_size = 2 + strlen(span->annotations[i].key) + 2 +
                      strlen(span->annotations[i].value);
    if (pos + ann_size > bufsize) {
      break; /* No more room — truncate remaining annotations */
    }
    pos += ann_size;
    n++;
  }
  return n;
}

/* Write the 48-byte fixed header. Returns 0 on success, -1 on error. */
static int serialize_header(uint8_t *buffer, size_t bufsize, const span_t *span,
                            bool sampled, size_t name_len, int num_annotations) {
  uint64_t start_ts = span->wall_start_us;
  uint64_t duration_us = 0;
  if (span->monotonic_end_ns > span->monotonic_start_ns) {
    duration_us = (span->monotonic_end_ns - span->monotonic_start_ns) / 1000ULL;
  }

  if (write_u64(buffer, WIRE_OFF_TRACE_ID, bufsize, span->trace_id) < 0 ||
      write_u64(buffer, WIRE_OFF_SPAN_ID, bufsize, span->span_id) < 0 ||
      write_u64(buffer, WIRE_OFF_PARENT_SPAN_ID, bufsize,
                span->parent_span_id) < 0 ||
      write_u64(buffer, WIRE_OFF_START_TS, bufsize, start_ts) < 0 ||
      write_u64(buffer, WIRE_OFF_DURATION_US, bufsize, duration_us) < 0 ||
      write_u8(buffer, WIRE_OFF_SAMPLED, bufsize, sampled ? 1 : 0) < 0 ||
      write_u8(buffer, WIRE_OFF_FLAGS, bufsize, 0) < 0 ||
      write_u16(buffer, WIRE_OFF_NAME_LEN, bufsize, (uint16_t)name_len) < 0 ||
      write_u16(buffer, WIRE_OFF_NUM_ANNOTATIONS, bufsize,
                (uint16_t)num_annotations) < 0 ||
      write_u16(buffer, WIRE_OFF_RESERVED, bufsize, 0) < 0) {
    return -1;
  }
  return 0;
}

/* Write `num_annotations` key/value pairs starting at *offset. */
static int serialize_annotations(uint8_t *buffer, size_t bufsize,
                                 const span_t *span, int num_annotations,
                                 size_t *offset) {
  for (int i = 0; i < num_annotations; i++) {
    size_t key_len = strlen(span->annotations[i].key);
    size_t val_len = strlen(span->annotations[i].value);

    if (write_u16(buffer, *offset, bufsize, (uint16_t)key_len) < 0)
      return -1;
    *offset += 2;
    memcpy(buffer + *offset, span->annotations[i].key, key_len);
    *offset += key_len;

    if (write_u16(buffer, *offset, bufsize, (uint16_t)val_len) < 0)
      return -1;
    *offset += 2;
    memcpy(buffer + *offset, span->annotations[i].value, val_len);
    *offset += val_len;
  }
  return 0;
}

/* ---- Public API ---- */

int span_serialize(uint8_t *buffer, size_t bufsize, const span_t *span,
                   bool sampled) {
  if (!buffer || !span || bufsize < SPAN_WIRE_HEADER_SIZE) {
    return -1;
  }

  size_t name_len = serialize_name_len(span, bufsize);
  int num_annotations = serialize_annotation_count(
      span, bufsize, SPAN_WIRE_HEADER_SIZE + name_len);

  if (serialize_header(buffer, bufsize, span, sampled, name_len,
                       num_annotations) < 0) {
    return -1;
  }

  size_t offset = SPAN_WIRE_HEADER_SIZE;
  memcpy(buffer + offset, span->name, name_len);
  offset += name_len;

  if (serialize_annotations(buffer, bufsize, span, num_annotations, &offset) <
      0) {
    return -1;
  }

  return (int)offset;
}

/* ---- Deserialization helpers ---- */

/* Copy one length-prefixed string field from buffer[*offset], advancing
 * *offset by the on-wire length. The destination is truncated to
 * dest_size-1 and null-terminated. Returns 0 on success, -1 if the
 * field runs past the buffer. */
static int read_string_field(const uint8_t *buffer, size_t bufsize,
                             size_t *offset, char *dest, size_t dest_size) {
  if (*offset + 2 > bufsize) {
    return -1;
  }
  uint16_t len = read_u16(buffer, *offset);
  *offset += 2;
  if (*offset + len > bufsize) {
    return -1;
  }
  size_t copy = len;
  if (copy >= dest_size) {
    copy = dest_size - 1;
  }
  memcpy(dest, buffer + *offset, copy);
  dest[copy] = '\0';
  *offset += len;
  return 0;
}

/* Parse up to MAX_ANNOTATIONS key/value pairs starting at *offset. */
static int deserialize_annotations(const uint8_t *buffer, size_t bufsize,
                                   span_t *span, uint16_t num_annotations,
                                   size_t *offset) {
  for (int i = 0; i < num_annotations && i < MAX_ANNOTATIONS; i++) {
    if (read_string_field(buffer, bufsize, offset, span->annotations[i].key,
                          ANNOTATION_KEY_MAX_LENGTH) < 0) {
      return -1;
    }
    if (read_string_field(buffer, bufsize, offset, span->annotations[i].value,
                          ANNOTATION_VALUE_MAX_LENGTH) < 0) {
      return -1;
    }
    span->annotation_count++;
  }
  return 0;
}

int span_deserialize(const uint8_t *buffer, size_t bufsize, span_t *span,
                     bool *sampled) {
  if (!buffer || !span || !sampled || bufsize < SPAN_WIRE_HEADER_SIZE) {
    return -1;
  }

  /* Zero out the span (clears pointers and annotation array) */
  memset(span, 0, sizeof(span_t));

  /* Read header */
  span->trace_id = read_u64(buffer, WIRE_OFF_TRACE_ID);
  span->span_id = read_u64(buffer, WIRE_OFF_SPAN_ID);
  span->parent_span_id = read_u64(buffer, WIRE_OFF_PARENT_SPAN_ID);

  uint64_t start_ts = read_u64(buffer, WIRE_OFF_START_TS);
  uint64_t duration_us = read_u64(buffer, WIRE_OFF_DURATION_US);

  /* Reconstruct timestamps:
   * wall_start_us = start_ts (epoch microseconds)
   * monotonic times: we store start=0 and end=duration*1000 so that
   * span_duration_ns() returns the correct value. The absolute
   * monotonic values are lost during serialization (by design). */
  span->wall_start_us = start_ts;
  span->monotonic_start_ns = 0;
  span->monotonic_end_ns = duration_us * 1000ULL;

  *sampled = read_u8(buffer, WIRE_OFF_SAMPLED) != 0;
  span->sampled = *sampled;
  /* flags at offset 41 — reserved, skip */

  uint16_t name_len = read_u16(buffer, WIRE_OFF_NAME_LEN);
  uint16_t num_annotations = read_u16(buffer, WIRE_OFF_NUM_ANNOTATIONS);
  /* reserved at offset 46 — skip */

  /* Validate name length against remaining buffer */
  size_t offset = SPAN_WIRE_HEADER_SIZE;
  if (offset + name_len > bufsize) {
    return -1;
  }

  /* Read span name */
  size_t copy_len = name_len;
  if (copy_len >= SPAN_NAME_MAX_LENGTH) {
    copy_len = SPAN_NAME_MAX_LENGTH - 1;
  }
  memcpy(span->name, buffer + offset, copy_len);
  span->name[copy_len] = '\0';
  offset += name_len;

  /* Read annotations */
  if (deserialize_annotations(buffer, bufsize, span, num_annotations,
                              &offset) < 0) {
    return -1;
  }

  /* Hierarchy/ownership pointers are not part of wire format
   * (already cleared by the memset above). */
  span->parent = NULL;
  span->first_child = NULL;
  span->next_sibling = NULL;
  span->owner_next = NULL;

  return (int)offset;
}
