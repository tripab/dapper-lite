/**
 * storage.c - Append-only trace log for the collector
 *
 * Writes completed traces to disk in a binary format:
 *   [8 bytes] trace_id       (big-endian)
 *   [4 bytes] num_spans      (big-endian)
 *   For each span:
 *     [4 bytes] span_wire_len (big-endian)
 *     [N bytes] serialized span data
 *
 * Uses buffered I/O (FILE*) opened in append mode.
 * storage_flush() forces data to disk.
 */

#include "dapper/collector.h"
#include "dapper/exporter.h"
#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- Byte order helpers ---- */

static uint64_t host_to_be64(uint64_t value) {
  static const int num = 42;
  if (*(const char *)&num == 42) {
    return ((uint64_t)htonl((uint32_t)(value & 0xFFFFFFFF)) << 32) |
           (uint64_t)htonl((uint32_t)(value >> 32));
  }
  return value;
}

/* ---- Storage implementation ---- */

struct trace_storage {
  FILE *fp;
};

trace_storage_t *storage_open(const char *filepath) {
  if (!filepath) {
    return NULL;
  }

  FILE *fp = fopen(filepath, "ab");
  if (!fp) {
    return NULL;
  }

  trace_storage_t *ts = calloc(1, sizeof(trace_storage_t));
  if (!ts) {
    fclose(fp);
    return NULL;
  }

  ts->fp = fp;
  return ts;
}

int storage_write_trace(trace_storage_t *ts, const partial_trace_t *pt) {
  if (!ts || !ts->fp || !pt) {
    return -1;
  }

  /* Write trace_id (8 bytes, big-endian) */
  uint64_t tid_be = host_to_be64(pt->trace_id);
  if (fwrite(&tid_be, 8, 1, ts->fp) != 1) {
    return -1;
  }

  /* Write num_spans (4 bytes, big-endian) */
  uint32_t nspans_be = htonl((uint32_t)pt->span_count);
  if (fwrite(&nspans_be, 4, 1, ts->fp) != 1) {
    return -1;
  }

  /* Serialize and write each span */
  span_t *s = pt->spans;
  while (s) {
    uint8_t wire_buf[SPAN_WIRE_MAX_SIZE];
    int wire_len = span_serialize(wire_buf, sizeof(wire_buf), s, pt->sampled);
    if (wire_len < 0) {
      return -1;
    }

    /* Write span length prefix (4 bytes, big-endian) */
    uint32_t len_be = htonl((uint32_t)wire_len);
    if (fwrite(&len_be, 4, 1, ts->fp) != 1) {
      return -1;
    }

    /* Write span wire data */
    if (fwrite(wire_buf, 1, (size_t)wire_len, ts->fp) != (size_t)wire_len) {
      return -1;
    }

    s = s->next_sibling;
  }

  return 0;
}

int storage_flush(trace_storage_t *ts) {
  if (!ts || !ts->fp) {
    return -1;
  }
  return fflush(ts->fp) == 0 ? 0 : -1;
}

void storage_close(trace_storage_t *ts) {
  if (!ts) {
    return;
  }
  if (ts->fp) {
    fclose(ts->fp);
    ts->fp = NULL;
  }
  free(ts);
}
