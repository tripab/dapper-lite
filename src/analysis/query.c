/**
 * query.c - Trace storage reader and query engine
 *
 * Reads the collector's append-only storage log and reconstructs
 * trace_t objects with full span hierarchy (parent/child/sibling
 * pointers rebuilt from parent_span_id fields).
 *
 * Storage format (written by src/collector/storage.c):
 *   [8 bytes] trace_id       (big-endian)
 *   [4 bytes] num_spans      (big-endian)
 *   For each span:
 *     [4 bytes] span_wire_len (big-endian)
 *     [N bytes] serialized span data (wire format)
 */

#include "dapper/analysis.h"
#include "dapper/exporter.h"
#include "dapper/trace.h"
#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- Byte order helpers ---- */

static uint64_t be64_to_host(uint64_t value) {
  static const int num = 42;
  if (*(const char *)&num == 42) {
    return ((uint64_t)ntohl((uint32_t)(value & 0xFFFFFFFF)) << 32) |
           (uint64_t)ntohl((uint32_t)(value >> 32));
  }
  return value;
}

/* ---- Hierarchy reconstruction ---- */

/**
 * Find a span by span_id in a flat array.
 */
static span_t *find_span_by_id(span_t **spans, int count, span_id_t id) {
  for (int i = 0; i < count; i++) {
    if (spans[i]->span_id == id) {
      return spans[i];
    }
  }
  return NULL;
}

/**
 * Rebuild parent/child/sibling pointers from parent_span_id fields.
 * Sets trace->root_span to the root (parent_span_id == 0).
 *
 * Every span is linked into the trace's ownership list (owner_next)
 * so that trace_destroy() frees all of them, including orphan spans
 * whose parent is missing and extra roots beyond the first.
 */
static void rebuild_hierarchy(trace_t *trace, span_t **spans, int count) {
  trace->root_span = NULL;

  /* Clear hierarchy pointers and take ownership of every span. */
  for (int i = 0; i < count; i++) {
    spans[i]->parent = NULL;
    spans[i]->first_child = NULL;
    spans[i]->next_sibling = NULL;
    spans[i]->owner_next = trace->all_spans;
    trace->all_spans = spans[i];
  }

  /* Link children to parents */
  for (int i = 0; i < count; i++) {
    if (spans[i]->parent_span_id == 0) {
      /* Root span. If several spans claim to be root, keep the first
       * and leave the rest owned-but-unlinked (they still get freed). */
      if (trace->root_span == NULL) {
        trace->root_span = spans[i];
      }
    } else {
      span_t *parent = find_span_by_id(spans, count, spans[i]->parent_span_id);
      if (parent) {
        spans[i]->parent = parent;
        /* Append to parent's child list */
        if (parent->first_child == NULL) {
          parent->first_child = spans[i];
        } else {
          span_t *last = parent->first_child;
          while (last->next_sibling) {
            last = last->next_sibling;
          }
          last->next_sibling = spans[i];
        }
      }
    }
  }
}

/* ---- Storage reader ---- */

/**
 * Read a single trace from an open file.
 *
 * Returns a fully reconstructed trace_t on success (status OK), or
 * NULL with *status set to EOF (clean), CORRUPT (malformed/truncated
 * record), or IO_ERROR.
 *
 * A short read at the very first field (trace_id) is a clean EOF; a
 * short read anywhere after that means the record was truncated and
 * is reported as corruption rather than EOF.
 */
static trace_t *read_one_trace(FILE *fp, trace_read_status_t *status) {
  /* Read trace_id (8 bytes BE) */
  uint64_t tid_be;
  if (fread(&tid_be, 8, 1, fp) != 1) {
    /* Distinguish clean EOF from a partial trailing record. */
    *status = feof(fp) ? TRACE_READ_EOF : TRACE_READ_IO_ERROR;
    return NULL;
  }
  trace_id_t trace_id = be64_to_host(tid_be);

  /* Read num_spans (4 bytes BE) */
  uint32_t nspans_be;
  if (fread(&nspans_be, 4, 1, fp) != 1) {
    *status = TRACE_READ_CORRUPT; /* trace_id present but record truncated */
    return NULL;
  }
  uint32_t num_spans = ntohl(nspans_be);

  /* Reject impossible counts: zero spans or an absurd count that would
   * trigger a huge allocation. */
  if (num_spans == 0 || num_spans > STORAGE_MAX_SPANS_PER_TRACE) {
    *status = TRACE_READ_CORRUPT;
    return NULL;
  }

  /* Allocate span pointer array for hierarchy reconstruction */
  span_t **span_ptrs = calloc(num_spans, sizeof(span_t *));
  if (!span_ptrs) {
    *status = TRACE_READ_IO_ERROR;
    return NULL;
  }

  /* Read each span */
  bool sampled = false;
  int spans_read = 0;
  *status = TRACE_READ_CORRUPT; /* any failure below is corruption */

  for (uint32_t i = 0; i < num_spans; i++) {
    /* Read wire length (4 bytes BE) */
    uint32_t slen_be;
    if (fread(&slen_be, 4, 1, fp) != 1) {
      goto fail;
    }
    uint32_t slen = ntohl(slen_be);
    if (slen == 0 || slen > SPAN_WIRE_MAX_SIZE) {
      goto fail;
    }

    /* Read wire data */
    uint8_t wire[SPAN_WIRE_MAX_SIZE];
    if (fread(wire, 1, slen, fp) != slen) {
      goto fail;
    }

    /* Deserialize */
    span_t *span = calloc(1, sizeof(span_t));
    if (!span) {
      *status = TRACE_READ_IO_ERROR;
      goto fail;
    }

    bool span_sampled;
    int consumed = span_deserialize(wire, slen, span, &span_sampled);
    if (consumed < 0) {
      free(span);
      goto fail;
    }

    if (i == 0) {
      sampled = span_sampled;
    }

    span_ptrs[spans_read++] = span;
  }

  /* Build trace */
  trace_t *trace = trace_create_with_id(trace_id);
  if (!trace) {
    *status = TRACE_READ_IO_ERROR;
    goto fail;
  }

  trace->sampled = sampled;

  /* Rebuild parent/child/sibling pointers from parent_span_id. */
  rebuild_hierarchy(trace, span_ptrs, spans_read);

  free(span_ptrs);
  *status = TRACE_READ_OK;
  return trace;

fail:
  for (int i = 0; i < spans_read; i++) {
    free(span_ptrs[i]);
  }
  free(span_ptrs);
  return NULL;
}

/* ---- Public API ---- */

trace_t **query_load_all_status(const char *storage_path, int *out_count,
                                trace_read_status_t *out_status) {
  if (out_status) {
    *out_status = TRACE_READ_IO_ERROR;
  }
  if (!storage_path || !out_count) {
    if (out_count)
      *out_count = 0;
    return NULL;
  }

  *out_count = 0;

  FILE *fp = fopen(storage_path, "rb");
  if (!fp) {
    return NULL;
  }

  /* Dynamic array of traces */
  int capacity = 64;
  int count = 0;
  trace_t **traces = calloc(capacity, sizeof(trace_t *));
  if (!traces) {
    fclose(fp);
    return NULL;
  }

  trace_read_status_t status = TRACE_READ_EOF;
  trace_t *trace;
  while ((trace = read_one_trace(fp, &status)) != NULL) {
    if (count >= capacity) {
      capacity *= 2;
      trace_t **new_arr = realloc(traces, capacity * sizeof(trace_t *));
      if (!new_arr) {
        trace_destroy(trace);
        status = TRACE_READ_IO_ERROR;
        break;
      }
      traces = new_arr;
    }
    traces[count++] = trace;
  }

  fclose(fp);
  *out_count = count;

  /* Surface corruption instead of silently treating it as EOF. */
  if (status == TRACE_READ_CORRUPT || status == TRACE_READ_IO_ERROR) {
    fprintf(stderr,
            "query: storage log '%s' %s after %d valid trace(s)\n",
            storage_path,
            status == TRACE_READ_CORRUPT ? "is corrupt/truncated"
                                         : "hit an I/O error",
            count);
  }

  if (out_status) {
    *out_status = status;
  }

  if (count == 0) {
    free(traces);
    return NULL;
  }

  return traces;
}

trace_t **query_load_all(const char *storage_path, int *out_count) {
  return query_load_all_status(storage_path, out_count, NULL);
}

trace_t *query_trace_by_id(const char *storage_path, trace_id_t id) {
  int count;
  trace_t **all = query_load_all(storage_path, &count);
  if (!all) {
    return NULL;
  }

  trace_t *result = NULL;
  for (int i = 0; i < count; i++) {
    if (all[i]->id == id) {
      result = all[i];
    } else {
      trace_destroy(all[i]);
    }
  }
  free(all);
  return result;
}

/* Comparison function for sorting traces by duration (descending) */
static uint64_t trace_root_duration_us(const trace_t *trace) {
  if (!trace->root_span) {
    return 0;
  }
  return trace->root_span->monotonic_end_ns / 1000ULL;
}

static int cmp_trace_duration_desc(const void *a, const void *b) {
  const trace_t *ta = *(const trace_t *const *)a;
  const trace_t *tb = *(const trace_t *const *)b;
  uint64_t da = trace_root_duration_us(ta);
  uint64_t db = trace_root_duration_us(tb);
  if (da > db)
    return -1;
  if (da < db)
    return 1;
  return 0;
}

trace_t **query_slowest_traces(const char *storage_path, int limit,
                               int *out_count) {
  if (!out_count) {
    return NULL;
  }
  *out_count = 0;

  if (limit <= 0) {
    return NULL;
  }

  int total;
  trace_t **all = query_load_all(storage_path, &total);
  if (!all) {
    return NULL;
  }

  /* Sort by duration descending */
  qsort(all, (size_t)total, sizeof(trace_t *), cmp_trace_duration_desc);

  /* Return at most `limit` traces, free the rest */
  int result_count = (total < limit) ? total : limit;
  for (int i = result_count; i < total; i++) {
    trace_destroy(all[i]);
  }

  *out_count = result_count;

  if (result_count < total) {
    trace_t **trimmed = realloc(all, (size_t)result_count * sizeof(trace_t *));
    if (trimmed) {
      return trimmed;
    }
  }

  return all;
}
