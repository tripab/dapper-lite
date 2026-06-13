/**
 * export_internal.h - Private export-layer internals
 *
 * White-box layout of the ring buffer and the sink interface. These
 * are implementation details of the exporter: the public exporter.h
 * exposes only opaque handles and the ring-buffer/exporter functions.
 * Consumed by the export module and by selected white-box tests
 * (compiled with -Isrc).
 */

#ifndef DAPPER_EXPORT_INTERNAL_H
#define DAPPER_EXPORT_INTERNAL_H

#include "dapper/exporter.h"
#include <stdatomic.h>

/* ---- Ring buffer (SPSC lock-free queue) layout ---- */

struct ring_buffer {
  uint8_t *data;
  size_t entry_size;
  size_t capacity;
  _Atomic size_t write_index;
  _Atomic size_t read_index;
};

/* ---- Sink interface ---- */

typedef enum { SINK_TYPE_FILE, SINK_TYPE_UDP } sink_type_t;

typedef struct sink {
  sink_type_t type;
  void *impl;

  /* Write serialized span bytes to this sink. Returns 0 on success. */
  int (*write)(struct sink *sink, const uint8_t *data, size_t len);

  /* Close and release sink resources. */
  void (*close)(struct sink *sink);
} sink_t;

/**
 * Create a file sink for debug output.
 * Returns a sink, or NULL on failure.
 */
sink_t *sink_create_file(const char *filepath);

/**
 * Create a UDP sink for network export.
 * Returns a sink, or NULL on failure.
 */
sink_t *sink_create_udp(const char *host, int port);

/**
 * Destroy a sink (calls close, then frees).
 */
void sink_destroy(sink_t *sink);

#endif /* DAPPER_EXPORT_INTERNAL_H */
