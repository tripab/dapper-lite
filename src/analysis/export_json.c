/**
 * export_json.c - JSON trace export
 *
 * Serializes a trace_t to JSON format, including span hierarchy,
 * annotations, and critical path information.
 */

#include "dapper/analysis.h"
#include "span_walk.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- JSON string escaping ---- */

/**
 * Write a JSON-escaped string to the file.
 * Escapes: backslash, double-quote, and control characters.
 */
static void write_json_string(FILE *fp, const char *str) {
  fputc('"', fp);
  for (const char *p = str; *p; p++) {
    switch (*p) {
    case '\\':
      fputs("\\\\", fp);
      break;
    case '"':
      fputs("\\\"", fp);
      break;
    case '\n':
      fputs("\\n", fp);
      break;
    case '\r':
      fputs("\\r", fp);
      break;
    case '\t':
      fputs("\\t", fp);
      break;
    default:
      if ((unsigned char)*p < 0x20) {
        fprintf(fp, "\\u%04x", (unsigned char)*p);
      } else {
        fputc(*p, fp);
      }
      break;
    }
  }
  fputc('"', fp);
}

/* ---- Span serialization ---- */

static void write_span_json(FILE *fp, const span_t *span, const char *indent) {
  uint64_t duration_us =
      (span->monotonic_end_ns - span->monotonic_start_ns) / 1000ULL;

  fprintf(fp, "%s{\n", indent);
  fprintf(fp, "%s  \"span_id\": \"%llu\",\n", indent,
          (unsigned long long)span->span_id);
  fprintf(fp, "%s  \"parent_span_id\": \"%llu\",\n", indent,
          (unsigned long long)span->parent_span_id);
  fprintf(fp, "%s  \"name\": ", indent);
  write_json_string(fp, span->name);
  fprintf(fp, ",\n");
  fprintf(fp, "%s  \"start_ts\": %llu,\n", indent,
          (unsigned long long)span->wall_start_us);
  fprintf(fp, "%s  \"duration_us\": %llu", indent,
          (unsigned long long)duration_us);

  if (span->annotation_count > 0) {
    fprintf(fp, ",\n%s  \"annotations\": {\n", indent);
    for (int i = 0; i < span->annotation_count; i++) {
      fprintf(fp, "%s    ", indent);
      write_json_string(fp, span->annotations[i].key);
      fputs(": ", fp);
      write_json_string(fp, span->annotations[i].value);
      if (i < span->annotation_count - 1) {
        fputc(',', fp);
      }
      fputc('\n', fp);
    }
    fprintf(fp, "%s  }", indent);
  }

  fprintf(fp, "\n%s}", indent);
}

/* Context for collecting spans into a growable flat array. */
typedef struct {
  const span_t **arr;
  int count;
  int capacity;
} span_array_t;

/**
 * Append one span to the flat array (preorder visitor). On allocation
 * failure the span is skipped; the partial array stays consistent.
 */
static void collect_span_into_array(const span_t *span, void *vctx) {
  span_array_t *a = (span_array_t *)vctx;
  if (a->count >= a->capacity) {
    int new_cap = a->capacity * 2;
    const span_t **new_arr =
        realloc(a->arr, (size_t)new_cap * sizeof(span_t *));
    if (!new_arr) {
      return;
    }
    a->arr = new_arr;
    a->capacity = new_cap;
  }
  a->arr[a->count++] = span;
}

/* ---- Public API ---- */

void export_trace_json(const trace_t *trace, FILE *output) {
  if (!trace || !output) {
    return;
  }

  uint64_t trace_duration_us = 0;
  if (trace->root_span) {
    trace_duration_us = (trace->root_span->monotonic_end_ns -
                         trace->root_span->monotonic_start_ns) /
                        1000ULL;
  }

  fprintf(output, "{\n");
  fprintf(output, "  \"trace_id\": \"%llu\",\n", (unsigned long long)trace->id);
  fprintf(output, "  \"sampled\": %s,\n", trace->sampled ? "true" : "false");
  fprintf(output, "  \"duration_us\": %llu,\n",
          (unsigned long long)trace_duration_us);

  /* Collect all spans in preorder */
  span_array_t spans = {NULL, 0, 32};
  spans.arr = calloc((size_t)spans.capacity, sizeof(span_t *));
  if (spans.arr && trace->root_span) {
    span_tree_walk_preorder(trace->root_span, collect_span_into_array, &spans);
  }

  fprintf(output, "  \"spans\": [\n");
  for (int i = 0; i < spans.count; i++) {
    write_span_json(output, spans.arr[i], "    ");
    if (i < spans.count - 1) {
      fputc(',', output);
    }
    fputc('\n', output);
  }
  fprintf(output, "  ],\n");

  /* Critical path */
  int path_len = 0;
  span_t **cpath = compute_critical_path(trace, &path_len);

  fprintf(output, "  \"critical_path\": [");
  for (int i = 0; i < path_len; i++) {
    fprintf(output, "\"%llu\"", (unsigned long long)cpath[i]->span_id);
    if (i < path_len - 1) {
      fputs(", ", output);
    }
  }
  fprintf(output, "]\n");

  fprintf(output, "}\n");

  free(cpath);
  free(spans.arr);
}

char *export_trace_json_string(const trace_t *trace) {
  if (!trace) {
    return NULL;
  }

  /* Write to a temporary memory stream */
  char *buf = NULL;
  size_t size = 0;
  FILE *stream = open_memstream(&buf, &size);
  if (!stream) {
    return NULL;
  }

  export_trace_json(trace, stream);
  fclose(stream);

  return buf;
}
