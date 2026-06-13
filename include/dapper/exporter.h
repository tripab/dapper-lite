/**
 * exporter.h - Asynchronous span export infrastructure
 *
 * Phase 4: Lock-free ring buffer, background exporter thread,
 * UDP and file sinks, backpressure handling.
 *
 * Design:
 * - Ring buffer stores pre-serialized span bytes (not span_t structs)
 * - Producer (hot path): span_finish() -> serialize -> enqueue bytes
 * - Consumer (background thread): dequeue bytes -> transmit via sink
 * - Backpressure: drop spans when ring buffer is full
 */

#ifndef EXPORTER_H
#define EXPORTER_H

#include "types.h"
#include "wire.h" /* span_serialize/deserialize, SPAN_WIRE_* constants */
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ============================================================
 * Ring Buffer (SPSC Lock-Free Queue)
 * ============================================================ */

#define RING_BUFFER_ENTRY_SIZE SPAN_WIRE_MAX_SIZE
#define RING_BUFFER_DEFAULT_CAPACITY 4096

/* Opaque ring buffer handle; layout is private to the export module
 * (see src/export/export_internal.h). */
typedef struct ring_buffer ring_buffer_t;

/**
 * Create a ring buffer with the given capacity.
 * Each entry is RING_BUFFER_ENTRY_SIZE bytes.
 *
 * Returns: Pointer to ring buffer, or NULL on failure.
 */
ring_buffer_t *ring_buffer_create(size_t capacity);

/**
 * Destroy a ring buffer and free its memory.
 */
void ring_buffer_destroy(ring_buffer_t *rb);

/**
 * Push a serialized span into the ring buffer (producer side).
 *
 * Serializes the span and writes the bytes into the next available slot.
 * Non-blocking: returns false immediately if the buffer is full.
 *
 * rb: The ring buffer
 * span: Span to serialize and enqueue
 * sampled: Sampling decision for this span
 *
 * Returns: true if enqueued, false if buffer is full (span dropped).
 */
bool ring_buffer_push(ring_buffer_t *rb, const span_t *span, bool sampled);

/**
 * Pop serialized bytes from the ring buffer (consumer side).
 *
 * Copies the next entry's bytes into the output buffer.
 * Non-blocking: returns false immediately if the buffer is empty.
 *
 * rb: The ring buffer
 * output: Output buffer (must be at least RING_BUFFER_ENTRY_SIZE bytes)
 * out_len: Set to the number of valid bytes in the entry
 *
 * Returns: true if an entry was dequeued, false if buffer is empty.
 */
bool ring_buffer_pop(ring_buffer_t *rb, uint8_t *output, size_t *out_len);

/**
 * Check if the ring buffer is empty.
 */
bool ring_buffer_is_empty(const ring_buffer_t *rb);

/**
 * Check if the ring buffer is full.
 */
bool ring_buffer_is_full(const ring_buffer_t *rb);

/* The sink interface (file/UDP transports) is an export-layer
 * implementation detail; see src/export/export_internal.h. */

/* ============================================================
 * Exporter (Background Thread + Ring Buffer + Sink)
 * ============================================================ */

typedef struct {
  _Atomic uint64_t spans_submitted;
  _Atomic uint64_t spans_exported;
  _Atomic uint64_t spans_dropped;
} exporter_stats_t;

typedef struct exporter exporter_t;

/**
 * Create an exporter that sends spans via UDP.
 *
 * collector_host: Hostname or IP of the collector
 * port: UDP port of the collector
 *
 * Returns: Exporter pointer, or NULL on failure.
 */
exporter_t *exporter_create_udp(const char *collector_host, int port);

/**
 * Create an exporter that writes spans to a file (for debugging).
 *
 * filepath: Path to output file
 *
 * Returns: Exporter pointer, or NULL on failure.
 */
exporter_t *exporter_create_file(const char *filepath);

/**
 * Start the exporter background thread.
 * Must be called before exporter_submit().
 *
 * Returns: 0 on success, -1 on failure.
 */
int exporter_start(exporter_t *exporter);

/**
 * Stop the exporter background thread.
 * Drains remaining spans in the ring buffer before stopping.
 */
void exporter_stop(exporter_t *exporter);

/**
 * Destroy the exporter and free all resources.
 * Calls exporter_stop() if the exporter is still running.
 */
void exporter_destroy(exporter_t *exporter);

/**
 * Submit a span for asynchronous export (non-blocking).
 *
 * This is the hot-path function called from instrumentation code.
 * It serializes the span and enqueues it in the ring buffer.
 * If the buffer is full, the span is dropped and the drop counter
 * is incremented.
 *
 * exporter: The exporter instance
 * span: The finished span to export
 */
void exporter_submit(exporter_t *exporter, const span_t *span);

/**
 * Get export statistics (thread-safe snapshot).
 */
void exporter_get_stats(const exporter_t *exporter, exporter_stats_t *stats);

#endif /* EXPORTER_H */
