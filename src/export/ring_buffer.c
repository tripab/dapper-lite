/**
 * ring_buffer.c - Lock-free SPSC (Single Producer, Single Consumer) queue
 *
 * Design:
 * - Fixed-size entries, each RING_BUFFER_ENTRY_SIZE bytes
 * - One slot is always left empty to distinguish full from empty:
 *     empty: write_index == read_index
 *     full:  (write_index + 1) % capacity == read_index
 * - Producer writes data then updates write_index with release semantics
 * - Consumer reads data then updates read_index with release semantics
 * - No locks needed for single-producer, single-consumer usage
 *
 * Entry layout within each slot:
 *   [0..3]  uint32_t payload_len  (host byte order)
 *   [4..4+payload_len)  serialized span bytes
 *   [remainder]  unused padding
 *
 * The 4-byte length prefix lets the consumer know exactly how many
 * valid bytes are in each entry without parsing the wire format.
 */

#include "export_internal.h"
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#define SLOT_HEADER_SIZE 4
#define MAX_PAYLOAD_SIZE (RING_BUFFER_ENTRY_SIZE - SLOT_HEADER_SIZE)

ring_buffer_t *ring_buffer_create(size_t capacity) {
  if (capacity == 0) {
    return NULL;
  }

  ring_buffer_t *rb = calloc(1, sizeof(ring_buffer_t));
  if (!rb) {
    return NULL;
  }

  rb->data = calloc(capacity, RING_BUFFER_ENTRY_SIZE);
  if (!rb->data) {
    free(rb);
    return NULL;
  }

  rb->entry_size = RING_BUFFER_ENTRY_SIZE;
  rb->capacity = capacity;
  atomic_store(&rb->write_index, 0);
  atomic_store(&rb->read_index, 0);

  return rb;
}

void ring_buffer_destroy(ring_buffer_t *rb) {
  if (!rb) {
    return;
  }
  free(rb->data);
  free(rb);
}

bool ring_buffer_push(ring_buffer_t *rb, const span_t *span, bool sampled) {
  if (!rb || !span) {
    return false;
  }

  size_t w = atomic_load_explicit(&rb->write_index, memory_order_relaxed);
  size_t r = atomic_load_explicit(&rb->read_index, memory_order_acquire);

  /* Full check: one slot always left empty */
  if ((w + 1) % rb->capacity == r) {
    return false;
  }

  /* Serialize directly into the slot (after the 4-byte length prefix) */
  uint8_t *slot = rb->data + (w * rb->entry_size);
  int len =
      span_serialize(slot + SLOT_HEADER_SIZE, MAX_PAYLOAD_SIZE, span, sampled);
  if (len < 0) {
    return false;
  }

  /* Write length prefix */
  uint32_t payload_len = (uint32_t)len;
  memcpy(slot, &payload_len, SLOT_HEADER_SIZE);

  /* Publish: release ensures data is visible before index update */
  atomic_store_explicit(&rb->write_index, (w + 1) % rb->capacity,
                        memory_order_release);
  return true;
}

bool ring_buffer_pop(ring_buffer_t *rb, uint8_t *output, size_t *out_len) {
  if (!rb || !output || !out_len) {
    return false;
  }

  size_t r = atomic_load_explicit(&rb->read_index, memory_order_relaxed);
  size_t w = atomic_load_explicit(&rb->write_index, memory_order_acquire);

  /* Empty check */
  if (r == w) {
    return false;
  }

  /* Read length prefix and payload from slot */
  uint8_t *slot = rb->data + (r * rb->entry_size);
  uint32_t payload_len;
  memcpy(&payload_len, slot, SLOT_HEADER_SIZE);

  if (payload_len > MAX_PAYLOAD_SIZE) {
    payload_len = MAX_PAYLOAD_SIZE;
  }

  memcpy(output, slot + SLOT_HEADER_SIZE, payload_len);
  *out_len = (size_t)payload_len;

  /* Publish: release ensures we're done reading before index update */
  atomic_store_explicit(&rb->read_index, (r + 1) % rb->capacity,
                        memory_order_release);
  return true;
}

bool ring_buffer_is_empty(const ring_buffer_t *rb) {
  if (!rb) {
    return true;
  }
  size_t r = atomic_load_explicit(&rb->read_index, memory_order_acquire);
  size_t w = atomic_load_explicit(&rb->write_index, memory_order_acquire);
  return r == w;
}

bool ring_buffer_is_full(const ring_buffer_t *rb) {
  if (!rb) {
    return true;
  }
  size_t w = atomic_load_explicit(&rb->write_index, memory_order_acquire);
  size_t r = atomic_load_explicit(&rb->read_index, memory_order_acquire);
  return (w + 1) % rb->capacity == r;
}
