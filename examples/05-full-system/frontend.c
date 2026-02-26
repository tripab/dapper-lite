/**
 * frontend.c - Simulated frontend service for full-system demo
 *
 * Generates N requests to the middleware service, each creating a
 * trace with sampling. Demonstrates the full pipeline:
 *   frontend -> middleware -> database -> collector -> storage -> JSON
 *
 * After sending requests, exports traces from the collector's storage
 * as JSON files for visualization.
 */

#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L
#include "dapper/analysis.h"
#include "dapper/context.h"
#include "dapper/exporter.h"
#include "dapper/sampler.h"
#include "dapper/span.h"
#include "dapper/trace.h"
#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define MW_PORT 7834
#define COLLECTOR_PORT 7831
#define NUM_REQUESTS 10
#define DB_CALLS_PER_REQUEST 5

/**
 * Send a single request to the middleware, creating a traced span.
 */
static int send_request(int req_num, sampler_t *sampler, exporter_t *exporter) {
  /* Create sampled trace */
  trace_t *trace = trace_create_sampled(sampler, "/api/users");
  if (!trace) {
    return -1;
  }

  span_t *fe_span = span_create(trace, NULL, "frontend_request");
  span_annotate(fe_span, "service", "frontend");
  span_annotate(fe_span, "http.method", "GET");
  span_annotate(fe_span, "http.url", "/api/users");

  char req_str[16];
  snprintf(req_str, sizeof(req_str), "%d", req_num);
  span_annotate(fe_span, "request.num", req_str);

  /* Connect to middleware */
  int sock = socket(AF_INET, SOCK_STREAM, 0);
  if (sock < 0) {
    perror("socket");
    span_finish(fe_span);
    trace_destroy(trace);
    return -1;
  }

  struct sockaddr_in mw_addr = {0};
  mw_addr.sin_family = AF_INET;
  mw_addr.sin_port = htons(MW_PORT);
  inet_pton(AF_INET, "127.0.0.1", &mw_addr.sin_addr);

  if (connect(sock, (struct sockaddr *)&mw_addr, sizeof(mw_addr)) < 0) {
    perror("connect to middleware");
    close(sock);
    span_finish(fe_span);
    trace_destroy(trace);
    return -1;
  }

  /* Send context + num_db_calls */
  uint8_t buf[64];
  int ctx_len = context_inject(fe_span, buf, TRACE_CONTEXT_WIRE_SIZE);
  if (ctx_len < 0) {
    close(sock);
    span_finish(fe_span);
    trace_destroy(trace);
    return -1;
  }
  buf[ctx_len] = (uint8_t)DB_CALLS_PER_REQUEST;

  send(sock, buf, (size_t)(ctx_len + 1), 0);

  /* Wait for response */
  uint32_t resp = 0;
  recv(sock, &resp, sizeof(resp), 0);
  close(sock);

  /* Simulate rendering response */
  usleep(500 + (unsigned)(rand() % 1000));

  span_finish(fe_span);

  /* Export span */
  if (trace->sampled) {
    exporter_submit(exporter, fe_span);
  }

  uint64_t dur_us = span_duration_ns(fe_span) / 1000;
  printf("[fe] request=%d trace=%016llx sampled=%s duration=%lluus\n", req_num,
         (unsigned long long)trace->id, trace->sampled ? "yes" : "no",
         (unsigned long long)dur_us);

  trace_destroy(trace);
  return 0;
}

/**
 * After traces have been collected, read them from storage and
 * export as JSON files for the visualization scripts.
 */
static void export_traces_as_json(const char *storage_path,
                                  const char *output_dir) {
  int count = 0;
  trace_t **traces = query_load_all(storage_path, &count);
  if (!traces || count == 0) {
    printf("[fe] No traces found in storage.\n");
    return;
  }

  printf("[fe] Found %d traces in storage. Exporting JSON...\n", count);
  mkdir(output_dir, 0755);

  for (int i = 0; i < count; i++) {
    char path[256];
    snprintf(path, sizeof(path), "%s/trace_%016llx.json", output_dir,
             (unsigned long long)traces[i]->id);

    FILE *fp = fopen(path, "w");
    if (fp) {
      export_trace_json(traces[i], fp);
      fclose(fp);
      printf("[fe] Exported %s\n", path);
    }

    trace_destroy(traces[i]);
  }
  free(traces);
}

int main(void) {
  srand((unsigned)time(NULL));

  printf("=== Full System Demo: Frontend ===\n");
  printf("Sending %d requests with %d DB calls each\n\n", NUM_REQUESTS,
         DB_CALLS_PER_REQUEST);

  /* Create sampler: sample 100% for demo purposes */
  sampler_t *sampler = sampler_create_probability(1.0);

  /* Create exporter to send spans to collector */
  exporter_t *exporter = exporter_create_udp("127.0.0.1", COLLECTOR_PORT);
  if (!exporter || exporter_start(exporter) != 0) {
    fprintf(stderr, "[fe] Failed to create exporter\n");
    return 1;
  }

  /* Send requests */
  for (int i = 0; i < NUM_REQUESTS; i++) {
    send_request(i, sampler, exporter);
    /* Small delay between requests */
    usleep(100000); /* 100ms */
  }

  /* Wait for spans to be exported and collector to flush */
  printf("\n[fe] All requests sent. Waiting for collector to flush...\n");
  exporter_stats_t stats;
  exporter_get_stats(exporter, &stats);
  printf("[fe] Exporter stats: submitted=%llu exported=%llu dropped=%llu\n",
         (unsigned long long)stats.spans_submitted,
         (unsigned long long)stats.spans_exported,
         (unsigned long long)stats.spans_dropped);

  /* Give collector time to flush traces */
  sleep(7);

  /* Export traces as JSON */
  export_traces_as_json("traces.log", "traces");

  /* Cleanup */
  exporter_destroy(exporter);
  sampler_destroy(sampler);

  printf("\n=== Demo Complete ===\n");
  printf("Run visualization:\n");
  printf(
      "  python3 scripts/visualize_trace.py traces/<trace>.json output.png\n");
  printf("  python3 scripts/analyze_latency.py traces/\n");

  return 0;
}
