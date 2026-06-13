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

#include "internal.h"
#include "dapper/wire.h"
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

  /* Stage the entire record in memory first, then write it with a
   * single fwrite. This guarantees we never emit a header whose
   * declared span count exceeds the bytes that follow: any
   * serialization failure aborts before anything is written, and a
   * torn write (e.g. process killed) is detected by the reader's
   * length/count validation rather than producing a half-record that
   * still looks well-formed. */

  /* Header (8B trace_id + 4B num_spans); span count is filled in
   * after we know how many spans serialized successfully. */
  size_t cap = 4096;
  size_t used = 12;
  uint8_t *buf = malloc(cap);
  if (!buf) {
    return -1;
  }

  uint32_t span_count = 0;
  collected_span_t *s = pt->spans;
  while (s) {
    uint8_t wire_buf[SPAN_WIRE_MAX_SIZE];
    int wire_len =
        span_serialize(wire_buf, sizeof(wire_buf), &s->span, pt->sampled);
    if (wire_len < 0) {
      free(buf); /* abort: nothing written to disk */
      return -1;
    }

    /* Grow the staging buffer if needed (4B len prefix + payload). */
    size_t need = used + 4 + (size_t)wire_len;
    if (need > cap) {
      while (cap < need) {
        cap *= 2;
      }
      uint8_t *grown = realloc(buf, cap);
      if (!grown) {
        free(buf);
        return -1;
      }
      buf = grown;
    }

    uint32_t len_be = htonl((uint32_t)wire_len);
    memcpy(buf + used, &len_be, 4);
    used += 4;
    memcpy(buf + used, wire_buf, (size_t)wire_len);
    used += (size_t)wire_len;
    span_count++;

    s = s->next;
  }

  /* A zero-span record would be rejected as corrupt on read and would
   * desync the log, so treat "nothing to write" as a successful no-op. */
  if (span_count == 0) {
    free(buf);
    return 0;
  }

  /* Fill in the header now that the count is known. */
  uint64_t tid_be = host_to_be64(pt->trace_id);
  memcpy(buf, &tid_be, 8);
  uint32_t nspans_be = htonl(span_count);
  memcpy(buf + 8, &nspans_be, 4);

  size_t written = fwrite(buf, 1, used, ts->fp);
  free(buf);
  return written == used ? 0 : -1;
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
