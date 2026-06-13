/**
 * internal.h - Private collector module interfaces
 *
 * Shared between the collector daemon (main.c) and its internal
 * modules (receiver.c). Not installed as a public header.
 */

#ifndef DAPPER_COLLECTOR_INTERNAL_H
#define DAPPER_COLLECTOR_INTERNAL_H

#include "dapper/collector.h"
#include <pthread.h>
#include <time.h>

/* ---- Trace assembly internals (assembler.c) ---- */

/**
 * A collector-owned list node wrapping one received span. The
 * collector keeps its own list rather than reusing the span's
 * hierarchy pointers, so those keep a single meaning.
 */
struct collected_span {
  span_t span;
  struct collected_span *next;
};

/**
 * A partially-assembled trace collecting spans as they arrive.
 * Stored in the trace_map hash table, keyed by trace_id.
 */
struct partial_trace {
  trace_id_t trace_id;

  /* Linked list of received spans (newest first) */
  collected_span_t *spans;
  int span_count;

  /* Sampling decision from first span received */
  bool sampled;

  /* Completion tracking */
  bool has_root;               /* Root span (parent_span_id==0) received? */
  struct timespec last_update; /* Clock time of last span arrival */

  /* Hash table chaining */
  struct partial_trace *next;
};

/**
 * Hash table: trace_id -> partial_trace.
 */
struct trace_map {
  partial_trace_t **buckets;
  size_t num_buckets;
  size_t count; /* Number of active partial traces */
  pthread_mutex_t lock;

  /* Resource caps (0 = unlimited), see trace_map_set_limits() */
  size_t max_traces;
  int max_spans_per_trace;

  /* Drop counters (protected by lock) */
  uint64_t traces_dropped; /* Spans for new traces rejected at capacity */
  uint64_t spans_dropped;  /* Spans rejected by the per-trace cap */
};

/* ---- UDP receiver (receiver.c) ---- */

typedef struct receiver receiver_t;

/**
 * Create a UDP receiver bound to config->bind_addr:config->port.
 * Decoded spans are inserted into trace_map.
 *
 * Returns NULL on invalid arguments, invalid bind address, or
 * socket/bind failure.
 */
receiver_t *receiver_create(const collector_config_t *config,
                            trace_map_t *trace_map);

int receiver_start(receiver_t *r);
void receiver_stop(receiver_t *r);
void receiver_destroy(receiver_t *r);

/** Return the actual bound UDP port (resolves an ephemeral port 0). */
int receiver_port(const receiver_t *r);

/**
 * Fill the packet-level fields of stats (packets_received,
 * packets_invalid, packets_unauthorized, packets_rate_limited,
 * spans_processed). Other fields are left untouched.
 */
void receiver_get_stats(const receiver_t *r, collector_stats_t *stats);

#endif /* DAPPER_COLLECTOR_INTERNAL_H */
