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

#include "dapper/wire.h"
#include <arpa/inet.h>
#include <string.h>

/* ---- Byte order helpers ---- */

static uint64_t host_to_be64(uint64_t value) {
  static const int num = 42;
  if (*(const char *)&num == 42) {
    /* Little-endian: swap */
    return ((uint64_t)htonl((uint32_t)(value & 0xFFFFFFFF)) << 32) |
           (uint64_t)htonl((uint32_t)(value >> 32));
  }
  return value;
}

static uint64_t be64_to_host(uint64_t value) {
  static const int num = 42;
  if (*(const char *)&num == 42) {
    return ((uint64_t)ntohl((uint32_t)(value & 0xFFFFFFFF)) << 32) |
           (uint64_t)ntohl((uint32_t)(value >> 32));
  }
  return value;
}

/* ---- Write helpers ---- */

static int write_u64(uint8_t *buf, size_t off, size_t bufsize, uint64_t val) {
  if (off + 8 > bufsize)
    return -1;
  uint64_t net = host_to_be64(val);
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
  return be64_to_host(net);
}

static uint16_t read_u16(const uint8_t *buf, size_t off) {
  uint16_t net;
  memcpy(&net, buf + off, 2);
  return ntohs(net);
}

static uint8_t read_u8(const uint8_t *buf, size_t off) { return buf[off]; }

/* ---- Public API ---- */

int span_serialize(uint8_t *buffer, size_t bufsize, const span_t *span,
                   bool sampled) {
  if (!buffer || !span || bufsize < SPAN_WIRE_HEADER_SIZE) {
    return -1;
  }

  /* Compute derived fields */
  uint64_t start_ts = span->wall_start_us;
  uint64_t duration_us = 0;
  if (span->monotonic_end_ns > span->monotonic_start_ns) {
    duration_us = (span->monotonic_end_ns - span->monotonic_start_ns) / 1000ULL;
  }

  /* Determine name length (actual strlen, not buffer size) */
  size_t name_len = strlen(span->name);
  if (name_len > SPAN_NAME_MAX_LENGTH - 1) {
    name_len = SPAN_NAME_MAX_LENGTH - 1;
  }

  /* Check if name fits in buffer after header */
  if (SPAN_WIRE_HEADER_SIZE + name_len > bufsize) {
    /* Truncate name to fit */
    name_len = bufsize - SPAN_WIRE_HEADER_SIZE;
  }

  /* Count how many annotations we can fit */
  size_t pos = SPAN_WIRE_HEADER_SIZE + name_len;
  int num_annotations = 0;

  for (int i = 0; i < span->annotation_count; i++) {
    size_t key_len = strlen(span->annotations[i].key);
    size_t val_len = strlen(span->annotations[i].value);
    size_t ann_size = 2 + key_len + 2 + val_len;

    if (pos + ann_size > bufsize) {
      break; /* No more room — truncate remaining annotations */
    }
    pos += ann_size;
    num_annotations++;
  }

  /* Write header */
  if (write_u64(buffer, WIRE_OFF_TRACE_ID, bufsize, span->trace_id) < 0)
    return -1;
  if (write_u64(buffer, WIRE_OFF_SPAN_ID, bufsize, span->span_id) < 0)
    return -1;
  if (write_u64(buffer, WIRE_OFF_PARENT_SPAN_ID, bufsize,
                span->parent_span_id) < 0)
    return -1;
  if (write_u64(buffer, WIRE_OFF_START_TS, bufsize, start_ts) < 0)
    return -1;
  if (write_u64(buffer, WIRE_OFF_DURATION_US, bufsize, duration_us) < 0)
    return -1;
  if (write_u8(buffer, WIRE_OFF_SAMPLED, bufsize, sampled ? 1 : 0) < 0)
    return -1;
  if (write_u8(buffer, WIRE_OFF_FLAGS, bufsize, 0) < 0)
    return -1;
  if (write_u16(buffer, WIRE_OFF_NAME_LEN, bufsize, (uint16_t)name_len) < 0)
    return -1;
  if (write_u16(buffer, WIRE_OFF_NUM_ANNOTATIONS, bufsize,
                (uint16_t)num_annotations) < 0)
    return -1;
  if (write_u16(buffer, WIRE_OFF_RESERVED, bufsize, 0) < 0)
    return -1;

  /* Write span name */
  size_t offset = SPAN_WIRE_HEADER_SIZE;
  memcpy(buffer + offset, span->name, name_len);
  offset += name_len;

  /* Write annotations */
  for (int i = 0; i < num_annotations; i++) {
    size_t key_len = strlen(span->annotations[i].key);
    size_t val_len = strlen(span->annotations[i].value);

    if (write_u16(buffer, offset, bufsize, (uint16_t)key_len) < 0)
      return -1;
    offset += 2;
    memcpy(buffer + offset, span->annotations[i].key, key_len);
    offset += key_len;

    if (write_u16(buffer, offset, bufsize, (uint16_t)val_len) < 0)
      return -1;
    offset += 2;
    memcpy(buffer + offset, span->annotations[i].value, val_len);
    offset += val_len;
  }

  return (int)offset;
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
  for (int i = 0; i < num_annotations && i < MAX_ANNOTATIONS; i++) {
    if (offset + 2 > bufsize)
      return -1;
    uint16_t key_len = read_u16(buffer, offset);
    offset += 2;

    if (offset + key_len > bufsize)
      return -1;
    size_t klen = key_len;
    if (klen >= ANNOTATION_KEY_MAX_LENGTH) {
      klen = ANNOTATION_KEY_MAX_LENGTH - 1;
    }
    memcpy(span->annotations[i].key, buffer + offset, klen);
    span->annotations[i].key[klen] = '\0';
    offset += key_len;

    if (offset + 2 > bufsize)
      return -1;
    uint16_t val_len = read_u16(buffer, offset);
    offset += 2;

    if (offset + val_len > bufsize)
      return -1;
    size_t vlen = val_len;
    if (vlen >= ANNOTATION_VALUE_MAX_LENGTH) {
      vlen = ANNOTATION_VALUE_MAX_LENGTH - 1;
    }
    memcpy(span->annotations[i].value, buffer + offset, vlen);
    span->annotations[i].value[vlen] = '\0';
    offset += val_len;

    span->annotation_count++;
  }

  /* Hierarchy/ownership pointers are not part of wire format
   * (already cleared by the memset above). */
  span->parent = NULL;
  span->first_child = NULL;
  span->next_sibling = NULL;
  span->owner_next = NULL;

  return (int)offset;
}
