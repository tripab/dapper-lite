/**
 * export_json.c - JSON trace export
 *
 * Serializes a trace_t to JSON format, including span hierarchy,
 * annotations, and critical path information.
 */

#include "dapper/analysis.h"
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

/**
 * Recursively collect all spans in DFS order into a flat array.
 */
static void collect_spans_dfs(const span_t *span, const span_t ***arr,
                              int *count, int *capacity) {
  if (!span) {
    return;
  }

  if (*count >= *capacity) {
    *capacity *= 2;
    const span_t **new_arr =
        realloc(*arr, (size_t)*capacity * sizeof(span_t *));
    if (!new_arr) {
      return;
    }
    *arr = new_arr;
  }
  (*arr)[(*count)++] = span;

  for (span_t *child = span->first_child; child; child = child->next_sibling) {
    collect_spans_dfs(child, arr, count, capacity);
  }
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

  /* Collect all spans */
  int span_count = 0;
  int span_capacity = 32;
  const span_t **all_spans = calloc((size_t)span_capacity, sizeof(span_t *));
  if (all_spans && trace->root_span) {
    collect_spans_dfs(trace->root_span, &all_spans, &span_count,
                      &span_capacity);
  }

  fprintf(output, "  \"spans\": [\n");
  for (int i = 0; i < span_count; i++) {
    write_span_json(output, all_spans[i], "    ");
    if (i < span_count - 1) {
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
  free(all_spans);
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
