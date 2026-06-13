/**
 * assembler.c - Trace assembly from individual spans
 *
 * Implements partial_trace and trace_map: a mutex-protected hash
 * table that accumulates spans by trace_id. The flush function
 * returns traces that have timed out (no new spans for N seconds).
 *
 * Hash function: FNV-1a on the 8-byte trace_id.
 */

#include "dapper/collector.h"
#include "dapper/wire.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ============================================================
 * Partial Trace
 * ============================================================ */

partial_trace_t *partial_trace_create(trace_id_t trace_id) {
  partial_trace_t *pt = calloc(1, sizeof(partial_trace_t));
  if (!pt) {
    return NULL;
  }

  pt->trace_id = trace_id;
  pt->spans = NULL;
  pt->span_count = 0;
  pt->sampled = false;
  pt->has_root = false;
  clock_gettime(CLOCK_MONOTONIC, &pt->last_update);
  pt->next = NULL;

  return pt;
}

int partial_trace_add_span(partial_trace_t *pt, const span_t *span,
                           bool sampled) {
  if (!pt || !span) {
    return -1;
  }

  /* Allocate a collector list node holding a copy of the span. */
  collected_span_t *node = calloc(1, sizeof(collected_span_t));
  if (!node) {
    return -1;
  }
  memcpy(&node->span, span, sizeof(span_t));

  /* Hierarchy/ownership pointers are not meaningful on the collector
   * side; the partial trace tracks its spans via node->next. */
  node->span.parent = NULL;
  node->span.first_child = NULL;
  node->span.next_sibling = NULL;
  node->span.owner_next = NULL;

  /* Prepend to the collector-owned list. */
  node->next = pt->spans;
  pt->spans = node;
  pt->span_count++;

  /* Track root span */
  if (span->parent_span_id == 0) {
    pt->has_root = true;
  }

  /* Update sampling from first span */
  if (pt->span_count == 1) {
    pt->sampled = sampled;
  }

  /* Update last activity time */
  clock_gettime(CLOCK_MONOTONIC, &pt->last_update);

  return 0;
}

void partial_trace_destroy(partial_trace_t *pt) {
  if (!pt) {
    return;
  }

  /* Free all collector list nodes. */
  collected_span_t *s = pt->spans;
  while (s) {
    collected_span_t *next = s->next;
    free(s);
    s = next;
  }

  free(pt);
}

/* ============================================================
 * Trace Map (hash table)
 * ============================================================ */

/* FNV-1a hash for 8-byte trace_id */
static size_t trace_id_hash(trace_id_t id, size_t num_buckets) {
  uint64_t hash = 14695981039346656037ULL; /* FNV offset basis */
  const uint8_t *bytes = (const uint8_t *)&id;
  for (int i = 0; i < 8; i++) {
    hash ^= bytes[i];
    hash *= 1099511628211ULL; /* FNV prime */
  }
  return (size_t)(hash % num_buckets);
}

trace_map_t *trace_map_create(size_t num_buckets) {
  if (num_buckets == 0) {
    num_buckets = TRACE_MAP_DEFAULT_BUCKETS;
  }

  trace_map_t *tm = calloc(1, sizeof(trace_map_t));
  if (!tm) {
    return NULL;
  }

  tm->buckets = calloc(num_buckets, sizeof(partial_trace_t *));
  if (!tm->buckets) {
    free(tm);
    return NULL;
  }

  tm->num_buckets = num_buckets;
  tm->count = 0;
  tm->max_traces = COLLECTOR_DEFAULT_MAX_ACTIVE_TRACES;
  tm->max_spans_per_trace = COLLECTOR_DEFAULT_MAX_SPANS_PER_TRACE;
  tm->traces_dropped = 0;
  tm->spans_dropped = 0;

  if (pthread_mutex_init(&tm->lock, NULL) != 0) {
    free(tm->buckets);
    free(tm);
    return NULL;
  }

  return tm;
}

void trace_map_destroy(trace_map_t *tm) {
  if (!tm) {
    return;
  }

  /* Free all partial traces in all buckets */
  for (size_t i = 0; i < tm->num_buckets; i++) {
    partial_trace_t *pt = tm->buckets[i];
    while (pt) {
      partial_trace_t *next = pt->next;
      partial_trace_destroy(pt);
      pt = next;
    }
  }

  pthread_mutex_destroy(&tm->lock);
  free(tm->buckets);
  free(tm);
}

void trace_map_set_limits(trace_map_t *tm, size_t max_traces,
                          int max_spans_per_trace) {
  if (!tm) {
    return;
  }
  pthread_mutex_lock(&tm->lock);
  tm->max_traces = max_traces;
  tm->max_spans_per_trace = max_spans_per_trace;
  pthread_mutex_unlock(&tm->lock);
}

void trace_map_get_drop_stats(trace_map_t *tm, uint64_t *traces_dropped,
                              uint64_t *spans_dropped) {
  if (!tm) {
    return;
  }
  pthread_mutex_lock(&tm->lock);
  if (traces_dropped) {
    *traces_dropped = tm->traces_dropped;
  }
  if (spans_dropped) {
    *spans_dropped = tm->spans_dropped;
  }
  pthread_mutex_unlock(&tm->lock);
}

int trace_map_insert(trace_map_t *tm, const span_t *span, bool sampled) {
  if (!tm || !span) {
    return -1;
  }

  pthread_mutex_lock(&tm->lock);

  size_t bucket = trace_id_hash(span->trace_id, tm->num_buckets);

  /* Search for existing partial trace */
  partial_trace_t *pt = tm->buckets[bucket];
  while (pt) {
    if (pt->trace_id == span->trace_id) {
      break;
    }
    pt = pt->next;
  }

  /* Create new partial trace if not found */
  if (!pt) {
    if (tm->max_traces > 0 && tm->count >= tm->max_traces) {
      tm->traces_dropped++;
      pthread_mutex_unlock(&tm->lock);
      return -1;
    }
    pt = partial_trace_create(span->trace_id);
    if (!pt) {
      pthread_mutex_unlock(&tm->lock);
      return -1;
    }
    pt->next = tm->buckets[bucket];
    tm->buckets[bucket] = pt;
    tm->count++;
  }

  if (tm->max_spans_per_trace > 0 &&
      pt->span_count >= tm->max_spans_per_trace) {
    tm->spans_dropped++;
    pthread_mutex_unlock(&tm->lock);
    return -1;
  }

  int rc = partial_trace_add_span(pt, span, sampled);

  pthread_mutex_unlock(&tm->lock);
  return rc;
}

partial_trace_t *trace_map_find(trace_map_t *tm, trace_id_t trace_id) {
  if (!tm) {
    return NULL;
  }

  pthread_mutex_lock(&tm->lock);

  size_t bucket = trace_id_hash(trace_id, tm->num_buckets);
  partial_trace_t *pt = tm->buckets[bucket];
  while (pt) {
    if (pt->trace_id == trace_id) {
      pthread_mutex_unlock(&tm->lock);
      return pt;
    }
    pt = pt->next;
  }

  pthread_mutex_unlock(&tm->lock);
  return NULL;
}

int trace_map_flush(trace_map_t *tm, partial_trace_t **out, int max_out,
                    int timeout_sec) {
  if (!tm || !out || max_out <= 0) {
    return 0;
  }

  struct timespec now;
  clock_gettime(CLOCK_MONOTONIC, &now);

  int flushed = 0;

  pthread_mutex_lock(&tm->lock);

  for (size_t i = 0; i < tm->num_buckets && flushed < max_out; i++) {
    partial_trace_t **prev_ptr = &tm->buckets[i];
    partial_trace_t *pt = *prev_ptr;

    while (pt && flushed < max_out) {
      /* Check if this trace has timed out */
      long elapsed_sec = now.tv_sec - pt->last_update.tv_sec;
      if (now.tv_nsec < pt->last_update.tv_nsec) {
        elapsed_sec--;
      }

      if (elapsed_sec >= timeout_sec) {
        /* Remove from hash table and transfer to caller */
        *prev_ptr = pt->next;
        pt->next = NULL;
        out[flushed++] = pt;
        tm->count--;
        pt = *prev_ptr;
      } else {
        prev_ptr = &pt->next;
        pt = pt->next;
      }
    }
  }

  pthread_mutex_unlock(&tm->lock);
  return flushed;
}
