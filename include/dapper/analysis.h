/**
 * analysis.h - Trace reconstruction, query, and analysis
 *
 * Phase 6: Read traces from the collector's append-only storage log,
 * reconstruct span DAGs, compute critical paths, aggregate statistics,
 * and export traces as JSON.
 */

#ifndef ANALYSIS_H
#define ANALYSIS_H

#include "types.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

/* ============================================================
 * Storage Reader / Trace Query
 * ============================================================ */

/**
 * Load all traces from a storage file.
 *
 * Reads the append-only log written by the collector, deserializes
 * each trace's spans, and rebuilds the span DAG (parent/child/sibling
 * pointers reconstructed from parent_span_id fields).
 *
 * storage_path: Path to the collector's storage file
 * out_count: Set to the number of traces loaded
 *
 * Returns: Array of trace_t pointers (caller owns and must free each
 *          with trace_destroy(), then free the array itself).
 *          Returns NULL on error (out_count set to 0).
 */
trace_t **query_load_all(const char *storage_path, int *out_count);

/**
 * Load a single trace by its trace ID.
 *
 * Scans the storage file for a trace with the given ID.
 *
 * Returns: trace_t pointer (caller owns), or NULL if not found.
 */
trace_t *query_trace_by_id(const char *storage_path, trace_id_t id);

/**
 * Find the slowest traces by total duration.
 *
 * Loads all traces and returns the top N sorted by root span duration
 * (descending). Traces without a root span are skipped.
 *
 * storage_path: Path to the collector's storage file
 * limit: Maximum number of traces to return
 * out_count: Set to the actual number of traces returned
 *
 * Returns: Array of trace_t pointers (caller owns each trace and the
 *          array). Returns NULL on error.
 */
trace_t **query_slowest_traces(const char *storage_path, int limit,
                               int *out_count);

/* ============================================================
 * Critical Path Computation
 * ============================================================ */

/**
 * Compute the critical path of a trace.
 *
 * The critical path is the chain of spans forming the longest
 * wall-clock dependency from root to leaf. This determines
 * the end-to-end latency of the trace.
 *
 * Uses DFS traversal on the span tree. For each span, the
 * critical child is the one with the largest subtree duration.
 *
 * trace: A reconstructed trace (with hierarchy pointers)
 * out_length: Set to the number of spans in the critical path
 *
 * Returns: Array of span_t pointers (root first, leaf last).
 *          Caller must free the array (but NOT the spans, which
 *          are owned by the trace). Returns NULL if trace has
 *          no root span.
 */
span_t **compute_critical_path(const trace_t *trace, int *out_length);

/**
 * Get the total duration of the critical path in microseconds.
 *
 * Convenience function that computes the critical path and returns
 * the root span's duration.
 */
uint64_t critical_path_duration_us(const trace_t *trace);

/* ============================================================
 * Per-Service Aggregation
 * ============================================================ */

/**
 * Per-service (per-span-name) latency statistics.
 */
typedef struct {
  char name[SPAN_NAME_MAX_LENGTH];
  int count;
  double mean_latency_us;
  double p50_latency_us;
  double p99_latency_us;
  double min_latency_us;
  double max_latency_us;
} service_stats_t;

/**
 * Aggregate latency statistics grouped by span name.
 *
 * Walks all spans across the given traces, groups by name,
 * and computes count, mean, p50, p99, min, and max latency.
 *
 * traces: Array of trace pointers
 * trace_count: Number of traces
 * out_count: Set to the number of distinct span names
 *
 * Returns: Array of service_stats_t (caller must free).
 *          Returns NULL on error.
 */
service_stats_t *aggregate_by_service(trace_t **traces, int trace_count,
                                      int *out_count);

/* ============================================================
 * JSON Export
 * ============================================================ */

/**
 * Export a trace as JSON to a file stream.
 *
 * Outputs a JSON object containing trace metadata, all spans
 * with their annotations, and optionally the critical path.
 *
 * trace: The trace to export
 * output: File stream to write to (e.g., stdout or fopen'd file)
 */
void export_trace_json(const trace_t *trace, FILE *output);

/**
 * Export a trace as JSON to a newly allocated string.
 *
 * Returns: Heap-allocated JSON string (caller must free).
 *          Returns NULL on error.
 */
char *export_trace_json_string(const trace_t *trace);

#endif /* ANALYSIS_H */
