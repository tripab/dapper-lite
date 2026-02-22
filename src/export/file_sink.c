/**
 * file_sink.c - File-based span export sink for debugging
 *
 * Writes serialized span bytes to a file. Each span is written as:
 *   [4 bytes: uint32_t payload length, host byte order]
 *   [N bytes: serialized span data]
 *
 * The length prefix allows readers to parse spans back out of the file.
 * File is opened in append mode so multiple exporters can write safely.
 */

#include "dapper/exporter.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
  FILE *fp;
} file_sink_impl_t;

static int file_sink_write(sink_t *sink, const uint8_t *data, size_t len) {
  if (!sink || !sink->impl || !data || len == 0) {
    return -1;
  }

  file_sink_impl_t *impl = (file_sink_impl_t *)sink->impl;

  /* Write length prefix then payload */
  uint32_t payload_len = (uint32_t)len;
  if (fwrite(&payload_len, sizeof(payload_len), 1, impl->fp) != 1) {
    return -1;
  }
  if (fwrite(data, 1, len, impl->fp) != len) {
    return -1;
  }
  fflush(impl->fp);
  return 0;
}

static void file_sink_close(sink_t *sink) {
  if (!sink || !sink->impl) {
    return;
  }

  file_sink_impl_t *impl = (file_sink_impl_t *)sink->impl;
  if (impl->fp) {
    fclose(impl->fp);
    impl->fp = NULL;
  }
}

sink_t *sink_create_file(const char *filepath) {
  if (!filepath) {
    return NULL;
  }

  FILE *fp = fopen(filepath, "ab");
  if (!fp) {
    return NULL;
  }

  file_sink_impl_t *impl = calloc(1, sizeof(file_sink_impl_t));
  if (!impl) {
    fclose(fp);
    return NULL;
  }
  impl->fp = fp;

  sink_t *sink = calloc(1, sizeof(sink_t));
  if (!sink) {
    fclose(fp);
    free(impl);
    return NULL;
  }

  sink->type = SINK_TYPE_FILE;
  sink->impl = impl;
  sink->write = file_sink_write;
  sink->close = file_sink_close;

  return sink;
}

void sink_destroy(sink_t *sink) {
  if (!sink) {
    return;
  }
  if (sink->close) {
    sink->close(sink);
  }
  free(sink->impl);
  free(sink);
}
