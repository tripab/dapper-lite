/**
 * span.c - Span lifecycle and annotation implementation
 */

#define _POSIX_C_SOURCE 200809L
#include "dapper/span.h"
#include "dapper/trace.h"
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* External clock function */
extern uint64_t clock_monotonic_ns(void);

/* Global span ID counter (atomic for thread safety) */
static _Atomic uint64_t g_next_span_id = 1;

/**
 * Generate a unique span ID
 */
static span_id_t generate_span_id(void) {
  return atomic_fetch_add(&g_next_span_id, 1);
}

/**
 * Safe string copy with truncation
 */
static void safe_strncpy(char *dest, const char *src, size_t dest_size) {
  if (!src || !dest || dest_size == 0) {
    return;
  }

  strncpy(dest, src, dest_size - 1);
  dest[dest_size - 1] = '\0';
}

span_t *span_create(trace_t *trace, span_t *parent, const char *name) {
  if (!trace || !name) {
    return NULL;
  }

  span_t *span = calloc(1, sizeof(span_t));
  if (!span) {
    return NULL;
  }

  /* Initialize IDs */
  span->trace_id = trace->id;
  span->span_id = generate_span_id();
  span->parent_span_id = parent ? parent->span_id : 0;

  /* Copy name */
  safe_strncpy(span->name, name, SPAN_NAME_MAX_LENGTH);

  /* Capture start timestamps (both monotonic and wall-clock) */
  span->monotonic_start_ns = clock_monotonic_ns();
  /* Wall-clock time for cross-system correlation */
  struct timespec wall;
  clock_gettime(CLOCK_REALTIME, &wall);
  span->wall_start_us =
      (uint64_t)wall.tv_sec * 1000000ULL + (uint64_t)wall.tv_nsec / 1000ULL;
  span->monotonic_end_ns = 0; /* Not finished yet */

  /* Initialize annotation count */
  span->annotation_count = 0;

  /* Set up parent-child relationship */
  span->parent = parent;
  span->first_child = NULL;
  span->next_sibling = NULL;

  /* Add to the trace's ownership list so trace_destroy() frees it
   * even if it never becomes reachable through the hierarchy (e.g. a
   * second root span). */
  span->owner_next = trace->all_spans;
  trace->all_spans = span;

  if (parent) {
    /* Add to parent's child list */
    if (parent->first_child == NULL) {
      parent->first_child = span;
    } else {
      /* Find last sibling and append */
      span_t *last = parent->first_child;
      while (last->next_sibling) {
        last = last->next_sibling;
      }
      last->next_sibling = span;
    }
  } else {
    /* This is a root span - set it on the trace if not already set */
    if (trace->root_span == NULL) {
      trace->root_span = span;
    }
  }

  return span;
}

void span_annotate(span_t *span, const char *key, const char *value) {
  if (!span || !key || !value) {
    return;
  }

  /* Silently ignore if at capacity */
  if (span->annotation_count >= MAX_ANNOTATIONS) {
    return;
  }

  /* Add annotation */
  annotation_t *ann = &span->annotations[span->annotation_count];
  safe_strncpy(ann->key, key, ANNOTATION_KEY_MAX_LENGTH);
  safe_strncpy(ann->value, value, ANNOTATION_VALUE_MAX_LENGTH);
  span->annotation_count++;
}

void span_finish(span_t *span) {
  if (!span) {
    return;
  }

  /* Safe to call multiple times - only capture timestamp once */
  if (span->monotonic_end_ns == 0) {
    span->monotonic_end_ns = clock_monotonic_ns();
  }
}

uint64_t span_duration_ns(const span_t *span) {
  if (!span || span->monotonic_end_ns == 0) {
    return 0;
  }

  return span->monotonic_end_ns - span->monotonic_start_ns;
}
