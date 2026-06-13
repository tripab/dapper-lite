/**
 * exporter_thread.c - Background exporter thread integration
 *
 * Ties together ring buffer + sink into a complete async export pipeline:
 *   1. Hot path: exporter_submit() serializes span into ring buffer
 *   2. Background thread: drains ring buffer, sends bytes to sink
 *   3. Statistics: atomic counters for submitted/exported/dropped
 *
 * The exporter owns the ring buffer and sink. The background thread
 * polls the ring buffer and sleeps briefly when empty.
 */

#define _POSIX_C_SOURCE 200809L
#include "../core/clock.h" /* dapper_sleep_us */
#include "export_internal.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>

struct exporter {
  ring_buffer_t *ring_buffer;
  sink_t *sink;
  pthread_t thread;
  _Atomic bool running;
  bool started;
  exporter_stats_t stats;
};

/* Background thread: drain ring buffer and send to sink */
static void *exporter_thread_func(void *arg) {
  exporter_t *exp = (exporter_t *)arg;
  uint8_t buffer[RING_BUFFER_ENTRY_SIZE];
  size_t len;

  while (atomic_load_explicit(&exp->running, memory_order_acquire)) {
    if (ring_buffer_pop(exp->ring_buffer, buffer, &len)) {
      if (exp->sink->write(exp->sink, buffer, len) == 0) {
        atomic_fetch_add_explicit(&exp->stats.spans_exported, 1,
                                  memory_order_relaxed);
      }
    } else {
      dapper_sleep_us(100); /* Sleep 100us when buffer is empty */
    }
  }

  /* Drain remaining spans before exiting */
  while (ring_buffer_pop(exp->ring_buffer, buffer, &len)) {
    if (exp->sink->write(exp->sink, buffer, len) == 0) {
      atomic_fetch_add_explicit(&exp->stats.spans_exported, 1,
                                memory_order_relaxed);
    }
  }

  return NULL;
}

static exporter_t *exporter_create_with_sink(sink_t *sink) {
  if (!sink) {
    return NULL;
  }

  ring_buffer_t *rb = ring_buffer_create(RING_BUFFER_DEFAULT_CAPACITY);
  if (!rb) {
    sink_destroy(sink);
    return NULL;
  }

  exporter_t *exp = calloc(1, sizeof(exporter_t));
  if (!exp) {
    ring_buffer_destroy(rb);
    sink_destroy(sink);
    return NULL;
  }

  exp->ring_buffer = rb;
  exp->sink = sink;
  atomic_store(&exp->running, false);
  exp->started = false;
  atomic_store(&exp->stats.spans_submitted, 0);
  atomic_store(&exp->stats.spans_exported, 0);
  atomic_store(&exp->stats.spans_dropped, 0);

  return exp;
}

exporter_t *exporter_create_udp(const char *collector_host, int port) {
  sink_t *sink = sink_create_udp(collector_host, port);
  if (!sink) {
    return NULL;
  }
  return exporter_create_with_sink(sink);
}

exporter_t *exporter_create_file(const char *filepath) {
  sink_t *sink = sink_create_file(filepath);
  if (!sink) {
    return NULL;
  }
  return exporter_create_with_sink(sink);
}

int exporter_start(exporter_t *exporter) {
  if (!exporter || exporter->started) {
    return -1;
  }

  atomic_store_explicit(&exporter->running, true, memory_order_release);

  if (pthread_create(&exporter->thread, NULL, exporter_thread_func, exporter) !=
      0) {
    atomic_store(&exporter->running, false);
    return -1;
  }

  exporter->started = true;
  return 0;
}

void exporter_stop(exporter_t *exporter) {
  if (!exporter || !exporter->started) {
    return;
  }

  atomic_store_explicit(&exporter->running, false, memory_order_release);
  pthread_join(exporter->thread, NULL);
  exporter->started = false;
}

void exporter_destroy(exporter_t *exporter) {
  if (!exporter) {
    return;
  }

  if (exporter->started) {
    exporter_stop(exporter);
  }

  ring_buffer_destroy(exporter->ring_buffer);
  sink_destroy(exporter->sink);
  free(exporter);
}

void exporter_submit(exporter_t *exporter, const span_t *span) {
  if (!exporter || !span) {
    return;
  }

  atomic_fetch_add_explicit(&exporter->stats.spans_submitted, 1,
                            memory_order_relaxed);

  /* Encode the span's real head-based sampling decision, not a
   * hardcoded "true". The caller is still responsible for gating
   * submission on the sampling decision; this keeps the wire format
   * honest if an unsampled span is ever submitted. */
  if (!ring_buffer_push(exporter->ring_buffer, span, span->sampled)) {
    atomic_fetch_add_explicit(&exporter->stats.spans_dropped, 1,
                              memory_order_relaxed);
  }
}

void exporter_get_stats(const exporter_t *exporter, exporter_stats_t *stats) {
  if (!exporter || !stats) {
    return;
  }

  atomic_store(&stats->spans_submitted,
               atomic_load_explicit(&exporter->stats.spans_submitted,
                                    memory_order_relaxed));
  atomic_store(&stats->spans_exported,
               atomic_load_explicit(&exporter->stats.spans_exported,
                                    memory_order_relaxed));
  atomic_store(&stats->spans_dropped,
               atomic_load_explicit(&exporter->stats.spans_dropped,
                                    memory_order_relaxed));
}
