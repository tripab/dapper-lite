/**
 * collector_ingest.c - Benchmark collector ingestion rate
 *
 * Measures the maximum rate at which the collector can receive and
 * process spans over UDP. A sender thread fires serialized span
 * datagrams at the collector's UDP port; the collector's receiver
 * thread decodes and inserts them into the trace map.
 *
 * Target: > 100k spans/sec
 */

#define _POSIX_C_SOURCE 200809L
#include "common.h"
#include "dapper/collector.h"
#include "dapper/exporter.h"
#include "dapper/span.h"
#include "dapper/trace.h"
#include <arpa/inet.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define NUM_SPANS 50000
#define COLLECTOR_PORT 17831 /* Non-default port to avoid conflicts */

int main(void) {
  printf("=== Collector Ingestion Rate Benchmark ===\n\n");

  /* Pre-serialize a span to send as UDP datagrams */
  trace_t *trace = trace_create();
  if (!trace) {
    fprintf(stderr, "Failed to create trace\n");
    return 1;
  }

  span_t *span = span_create(trace, NULL, "bench_ingest");
  span_annotate(span, "key", "value");
  span_finish(span);

  uint8_t wire_buf[SPAN_WIRE_MAX_SIZE];
  int wire_len = span_serialize(wire_buf, sizeof(wire_buf), span, true);
  if (wire_len <= 0) {
    fprintf(stderr, "Failed to serialize span\n");
    trace_destroy(trace);
    return 1;
  }

  printf("Configuration:\n");
  printf("  Spans to send: %d\n", NUM_SPANS);
  printf("  Collector port: %d\n", COLLECTOR_PORT);
  printf("  Wire size: %d bytes\n", wire_len);
  printf("  Mode: UDP loopback\n\n");

  /* Start collector */
  collector_config_t config = collector_default_config();
  config.port = COLLECTOR_PORT;
  config.storage_path = "/tmp/dapper_bench_collector.log";
  config.timeout_sec = 1;
  config.flush_interval_sec = 1;

  collector_t *c = collector_create(&config);
  if (!c) {
    fprintf(stderr, "Failed to create collector\n");
    trace_destroy(trace);
    return 1;
  }

  if (collector_start(c) < 0) {
    fprintf(stderr, "Failed to start collector\n");
    collector_destroy(c);
    trace_destroy(trace);
    return 1;
  }

  /* Create sender UDP socket */
  int sockfd = socket(AF_INET, SOCK_DGRAM, 0);
  if (sockfd < 0) {
    fprintf(stderr, "Failed to create UDP socket\n");
    collector_destroy(c);
    trace_destroy(trace);
    return 1;
  }

  struct sockaddr_in dest;
  memset(&dest, 0, sizeof(dest));
  dest.sin_family = AF_INET;
  dest.sin_port = htons(COLLECTOR_PORT);
  dest.sin_addr.s_addr = inet_addr("127.0.0.1");

  printf("Running benchmark...\n");

  /* Send all spans as fast as possible, measuring total time */
  uint64_t start = bench_get_time_ns();

  for (int i = 0; i < NUM_SPANS; i++) {
    sendto(sockfd, wire_buf, (size_t)wire_len, 0, (struct sockaddr *)&dest,
           sizeof(dest));
  }

  uint64_t send_end = bench_get_time_ns();

  /* Wait for collector to process (poll stats) */
  collector_stats_t stats;
  int wait_ms = 0;
  const int max_wait_ms = 5000;
  while (wait_ms < max_wait_ms) {
    collector_get_stats(c, &stats);
    if ((int)stats.spans_processed >= NUM_SPANS) {
      break;
    }
    usleep(10000); /* 10ms */
    wait_ms += 10;
  }

  uint64_t end = bench_get_time_ns();

  collector_get_stats(c, &stats);

  /* Compute throughput based on send time (measures sender bottleneck) */
  uint64_t send_elapsed_ns = send_end - start;
  double send_elapsed_s = (double)send_elapsed_ns / 1e9;
  double send_throughput = (double)NUM_SPANS / send_elapsed_s;

  /* Compute throughput based on end-to-end time (send + process) */
  uint64_t total_elapsed_ns = end - start;
  double total_elapsed_s = (double)total_elapsed_ns / 1e9;
  double e2e_throughput = (double)stats.spans_processed / total_elapsed_s;

  printf("\nSend Phase:\n");
  printf("  Elapsed: %.3f s\n", send_elapsed_s);
  printf("  Send throughput: %.0f spans/sec\n", send_throughput);
  printf("  Per-span send: %.0f ns\n", (double)send_elapsed_ns / NUM_SPANS);

  printf("\nEnd-to-End (send + process):\n");
  printf("  Elapsed: %.3f s\n", total_elapsed_s);
  printf("  Throughput: %.0f spans/sec\n", e2e_throughput);

  printf("\nCollector Statistics:\n");
  printf("  Packets received: %llu\n",
         (unsigned long long)stats.packets_received);
  printf("  Packets invalid:  %llu\n",
         (unsigned long long)stats.packets_invalid);
  printf("  Spans processed:  %llu\n",
         (unsigned long long)stats.spans_processed);

  double loss_pct = 100.0 * (NUM_SPANS - (int)stats.spans_processed) /
                    (double)NUM_SPANS;
  printf("  Packet loss: %.1f%%\n", loss_pct);

  int rc = bench_check_throughput(e2e_throughput, 100000.0,
                                  "collector ingestion");

  /* Cleanup */
  close(sockfd);
  collector_destroy(c);
  unlink(config.storage_path);
  trace_destroy(trace);

  return rc;
}
