/**
 * collector.h - Central trace collector infrastructure
 *
 * Phase 5: Receives spans via UDP, reassembles them into traces,
 * and persists completed traces to an append-only disk log.
 *
 * Architecture:
 *   UDP Socket -> Receiver Thread -> Assembler -> Storage
 *
 * The collector uses a hash map (trace_map) to accumulate spans
 * by trace_id. Traces are flushed to storage when "complete"
 * (heuristic) or after a configurable timeout.
 */

#ifndef COLLECTOR_H
#define COLLECTOR_H

#include "types.h"
#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

/* ============================================================
 * Configuration
 * ============================================================ */

#define COLLECTOR_DEFAULT_BIND_ADDR "127.0.0.1"
#define COLLECTOR_DEFAULT_PORT 7831
#define COLLECTOR_DEFAULT_TIMEOUT_SEC 5
#define COLLECTOR_DEFAULT_FLUSH_COUNT 100
#define COLLECTOR_DEFAULT_FLUSH_INTERVAL_SEC 1
#define COLLECTOR_UDP_MAX_PACKET 512
#define TRACE_MAP_DEFAULT_BUCKETS 1024

/* Resource caps (0 = unlimited) */
#define COLLECTOR_DEFAULT_MAX_ACTIVE_TRACES 65536
#define COLLECTOR_DEFAULT_MAX_SPANS_PER_TRACE 4096
#define COLLECTOR_DEFAULT_MAX_PACKETS_PER_SEC 0

/* Maximum number of addresses in a source allowlist */
#define COLLECTOR_MAX_ALLOWED_SOURCES 16

/* ============================================================
 * Protocol Decode
 * ============================================================ */

/**
 * Decode a UDP datagram into a span.
 *
 * Wraps span_deserialize() with collector-specific validation:
 * - Rejects packets smaller than wire header
 * - Validates trace_id and span_id are non-zero
 *
 * data: Raw UDP packet bytes
 * len: Packet length
 * span: Output span (caller-allocated)
 * sampled: Output sampling decision
 *
 * Returns: 0 on success, -1 on error (malformed/invalid).
 */
int collector_decode_span(const uint8_t *data, size_t len, span_t *span,
                          bool *sampled);

/* ============================================================
 * Partial Trace (in-memory assembly)
 * ============================================================ */

/**
 * A partially-assembled trace collecting spans as they arrive.
 * Stored in the trace_map hash table, keyed by trace_id.
 */
typedef struct partial_trace {
  trace_id_t trace_id;

  /* Linked list of received spans (newest first) */
  span_t *spans;
  int span_count;

  /* Sampling decision from first span received */
  bool sampled;

  /* Completion tracking */
  bool has_root;               /* Root span (parent_span_id==0) received? */
  struct timespec last_update; /* Clock time of last span arrival */

  /* Hash table chaining */
  struct partial_trace *next;
} partial_trace_t;

/**
 * Create a new partial trace for the given trace_id.
 * Returns NULL on allocation failure.
 */
partial_trace_t *partial_trace_create(trace_id_t trace_id);

/**
 * Add a span to a partial trace. The span is copied (caller retains
 * ownership of the original).
 *
 * Returns: 0 on success, -1 on allocation failure.
 */
int partial_trace_add_span(partial_trace_t *pt, const span_t *span,
                           bool sampled);

/**
 * Destroy a partial trace and free all its spans.
 */
void partial_trace_destroy(partial_trace_t *pt);

/* ============================================================
 * Trace Map (hash table: trace_id -> partial_trace)
 * ============================================================ */

typedef struct {
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
} trace_map_t;

/**
 * Create a trace map with the given number of buckets.
 * Returns NULL on failure.
 */
trace_map_t *trace_map_create(size_t num_buckets);

/**
 * Destroy the trace map and all partial traces it contains.
 */
void trace_map_destroy(trace_map_t *tm);

/**
 * Set resource caps on the trace map (0 = unlimited).
 *
 * max_traces: Maximum number of active partial traces. Spans for
 *             unseen trace IDs are dropped once the map is full.
 * max_spans_per_trace: Maximum spans accumulated per trace; excess
 *             spans are dropped.
 *
 * Drops are counted in tm->traces_dropped / tm->spans_dropped
 * (read them via trace_map_get_drop_stats()).
 */
void trace_map_set_limits(trace_map_t *tm, size_t max_traces,
                          int max_spans_per_trace);

/**
 * Read the drop counters (thread-safe).
 */
void trace_map_get_drop_stats(trace_map_t *tm, uint64_t *traces_dropped,
                              uint64_t *spans_dropped);

/**
 * Insert a span into the trace map. If no partial trace exists
 * for this trace_id, one is created.
 *
 * Thread-safe (acquires internal lock).
 *
 * Returns: 0 on success, -1 on error or when the span is dropped
 *          because a resource cap was reached.
 */
int trace_map_insert(trace_map_t *tm, const span_t *span, bool sampled);

/**
 * Find a partial trace by trace_id.
 *
 * Thread-safe (acquires internal lock). The returned pointer is
 * valid only while the caller holds no assumption about map mutations.
 * For safe iteration, use trace_map_flush_completed() instead.
 *
 * Returns: Pointer to partial trace, or NULL if not found.
 */
partial_trace_t *trace_map_find(trace_map_t *tm, trace_id_t trace_id);

/**
 * Flush completed and timed-out traces from the map.
 *
 * A trace is flushed if:
 *   1. It has a root span AND no new spans for >= timeout_sec, OR
 *   2. It has no root span AND no new spans for >= timeout_sec
 *      (partial trace timeout)
 *
 * Thread-safe (acquires internal lock).
 *
 * out: Output array of partial_trace pointers (caller-allocated)
 * max_out: Maximum number of traces to flush
 * timeout_sec: Inactivity timeout in seconds
 *
 * Returns: Number of traces flushed (transferred to caller).
 *          Caller takes ownership of the returned partial_trace_t objects.
 */
int trace_map_flush(trace_map_t *tm, partial_trace_t **out, int max_out,
                    int timeout_sec);

/* ============================================================
 * Storage (append-only trace log)
 * ============================================================ */

/**
 * Opaque storage handle.
 */
typedef struct trace_storage trace_storage_t;

/**
 * Open (or create) a trace storage file.
 *
 * filepath: Path to the append-only log file
 *
 * Returns: Storage handle, or NULL on failure.
 */
trace_storage_t *storage_open(const char *filepath);

/**
 * Write a completed trace to storage.
 *
 * Format:
 *   [8 bytes] trace_id (big-endian)
 *   [4 bytes] num_spans (big-endian)
 *   For each span:
 *     [4 bytes] span_wire_len (big-endian)
 *     [N bytes] serialized span data
 *
 * Returns: 0 on success, -1 on error.
 */
int storage_write_trace(trace_storage_t *ts, const partial_trace_t *pt);

/**
 * Flush buffered writes to disk.
 * Returns: 0 on success, -1 on error.
 */
int storage_flush(trace_storage_t *ts);

/**
 * Close storage and release resources.
 */
void storage_close(trace_storage_t *ts);

/* ============================================================
 * Collector Statistics
 * ============================================================ */

typedef struct {
  uint64_t packets_received;
  uint64_t packets_invalid;
  uint64_t packets_unauthorized; /* Rejected by allowlist or auth hook */
  uint64_t packets_rate_limited; /* Dropped by max_packets_per_sec */
  uint64_t spans_processed;
  uint64_t spans_dropped;  /* Dropped by per-trace span cap */
  uint64_t traces_dropped; /* New traces rejected at max_active_traces */
  uint64_t traces_completed;
  uint64_t traces_timed_out;
  uint64_t storage_writes;
  uint64_t storage_errors;
} collector_stats_t;

/* ============================================================
 * Collector Daemon
 * ============================================================ */

/**
 * Optional datagram authentication hook.
 *
 * Called for every datagram that passes the source allowlist,
 * before decoding. Return true to accept, false to reject.
 *
 * source_ip: Dotted-quad sender address.
 * data/len:  Raw datagram bytes (e.g. for HMAC verification).
 * user_data: Opaque pointer from collector_config_t.auth_user_data.
 */
typedef bool (*collector_auth_fn)(const char *source_ip, const uint8_t *data,
                                  size_t len, void *user_data);

typedef struct {
  /* Network: defaults to loopback. Binding non-loopback addresses
   * (e.g. "0.0.0.0") is opt-in and should be combined with
   * allowed_sources and/or auth_fn — span datagrams are otherwise
   * unauthenticated. */
  const char *bind_addr;
  int port;

  /* Source allowlist: comma-separated IPv4 literals
   * (e.g. "127.0.0.1,10.0.0.5"); NULL accepts any source. */
  const char *allowed_sources;

  /* Optional packet authentication hook (NULL = accept). */
  collector_auth_fn auth_fn;
  void *auth_user_data;

  /* Assembly/flush behavior */
  int timeout_sec;
  int flush_count; /* Max traces flushed per batch */
  int flush_interval_sec;
  size_t map_buckets;
  const char *storage_path;

  /* Resource caps (0 = unlimited) */
  size_t max_active_traces;
  int max_spans_per_trace;
  int max_packets_per_sec;
} collector_config_t;

/**
 * Return a config struct populated with default values.
 */
collector_config_t collector_default_config(void);

/**
 * Opaque collector handle.
 */
typedef struct collector collector_t;

/**
 * Create a collector with the given configuration.
 * Returns NULL on failure.
 */
collector_t *collector_create(const collector_config_t *config);

/**
 * Start the collector (receiver thread + flush loop).
 * Returns 0 on success, -1 on failure.
 */
int collector_start(collector_t *c);

/**
 * Stop the collector. Flushes remaining traces to storage.
 */
void collector_stop(collector_t *c);

/**
 * Destroy the collector and free all resources.
 * Calls collector_stop() if still running.
 */
void collector_destroy(collector_t *c);

/**
 * Get a snapshot of collector statistics.
 */
void collector_get_stats(const collector_t *c, collector_stats_t *stats);

#endif /* COLLECTOR_H */
