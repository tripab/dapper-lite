/**
 * middleware.c - Simulated middleware/business-logic service
 *
 * Sits between frontend and database. Receives trace context from
 * frontend, makes one or more calls to the database, and returns
 * aggregated results. Exports spans to the collector via UDP.
 */

#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L
#include "dapper/context.h"
#include "dapper/exporter.h"
#include "dapper/span.h"
#include "dapper/trace.h"
#include <arpa/inet.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define MW_PORT 7834
#define DB_PORT 7835
#define COLLECTOR_PORT 7831

static volatile int running = 1;

static void handle_signal(int sig) {
  (void)sig;
  running = 0;
}

/**
 * Call the database service with a given query type.
 * Returns the reported latency in microseconds, or -1 on error.
 */
static int call_database(span_t *parent_span, trace_t *trace,
                         exporter_t *exporter, const char *query_type) {
  /* Create child span for this DB call */
  char span_name[128];
  snprintf(span_name, sizeof(span_name), "db_call_%s", query_type);
  span_t *db_span = span_create(trace, parent_span, span_name);
  span_annotate(db_span, "service", "middleware");
  span_annotate(db_span, "peer.service", "database");
  span_annotate(db_span, "db.query_type", query_type);

  /* Connect to database */
  int sock = socket(AF_INET, SOCK_STREAM, 0);
  if (sock < 0) {
    span_annotate(db_span, "error", "socket_failed");
    span_finish(db_span);
    if (trace->sampled) {
      exporter_submit(exporter, db_span);
    }
    return -1;
  }

  struct sockaddr_in db_addr = {0};
  db_addr.sin_family = AF_INET;
  db_addr.sin_port = htons(DB_PORT);
  inet_pton(AF_INET, "127.0.0.1", &db_addr.sin_addr);

  if (connect(sock, (struct sockaddr *)&db_addr, sizeof(db_addr)) < 0) {
    span_annotate(db_span, "error", "connect_failed");
    span_finish(db_span);
    if (trace->sampled) {
      exporter_submit(exporter, db_span);
    }
    close(sock);
    return -1;
  }

  /* Send context + query type */
  uint8_t buf[256];
  int ctx_len = context_inject(db_span, buf, TRACE_CONTEXT_WIRE_SIZE);
  if (ctx_len < 0) {
    span_annotate(db_span, "error", "inject_failed");
    span_finish(db_span);
    if (trace->sampled) {
      exporter_submit(exporter, db_span);
    }
    close(sock);
    return -1;
  }

  /* Append query type after context */
  int qt_len = (int)strlen(query_type);
  buf[ctx_len] = (uint8_t)qt_len;
  memcpy(buf + ctx_len + 1, query_type, (size_t)qt_len);
  int total = ctx_len + 1 + qt_len;

  send(sock, buf, (size_t)total, 0);

  /* Receive response (4 bytes latency) */
  uint32_t resp = 0;
  recv(sock, &resp, sizeof(resp), 0);
  int latency_us = (int)ntohl(resp);

  close(sock);

  span_finish(db_span);
  if (trace->sampled) {
    exporter_submit(exporter, db_span);
  }

  return latency_us;
}

static void handle_connection(int client_sock, exporter_t *exporter) {
  /* Receive context + request info from frontend */
  uint8_t buf[256];
  int n = (int)recv(client_sock, buf, sizeof(buf), 0);
  if (n < TRACE_CONTEXT_WIRE_SIZE + 1) {
    fprintf(stderr, "[mw] Short read: %d bytes\n", n);
    return;
  }

  /* Extract trace context */
  trace_context_t ctx;
  if (context_extract(&ctx, buf, TRACE_CONTEXT_WIRE_SIZE) != 0) {
    fprintf(stderr, "[mw] Failed to extract context\n");
    return;
  }

  /* Extract number of DB calls to make */
  int num_calls = buf[TRACE_CONTEXT_WIRE_SIZE];
  if (num_calls < 1) {
    num_calls = 1;
  }
  if (num_calls > 10) {
    num_calls = 10;
  }

  /* Create trace and middleware span. The sampling decision is
   * inherited from the propagated context, not forced. */
  trace_t *trace = trace_create_with_id(ctx.trace_id);
  span_t *mw_span = span_create_from_context(trace, &ctx, "middleware_process");
  span_annotate(mw_span, "service", "middleware");

  char calls_str[16];
  snprintf(calls_str, sizeof(calls_str), "%d", num_calls);
  span_annotate(mw_span, "db.call_count", calls_str);

  /* Simulate some local processing */
  usleep(500 + (unsigned)(rand() % 1000));

  /* Make DB calls - last one may be slow to demonstrate tail latency */
  int max_latency = 0;
  for (int i = 0; i < num_calls; i++) {
    const char *qt = (i == num_calls - 1 && num_calls > 1) ? "slow" : "fast";
    int lat = call_database(mw_span, trace, exporter, qt);
    if (lat > max_latency) {
      max_latency = lat;
    }
  }

  /* Simulate response assembly */
  usleep(200 + (unsigned)(rand() % 500));

  span_finish(mw_span);
  if (trace->sampled) {
    exporter_submit(exporter, mw_span);
  }

  /* Send response: 4 bytes total latency */
  uint32_t resp = htonl((uint32_t)max_latency);
  send(client_sock, &resp, sizeof(resp), 0);

  printf("[mw] calls=%d max_db_latency=%dus trace=%016llx\n", num_calls,
         max_latency, (unsigned long long)trace->id);

  trace_destroy(trace);
}

int main(void) {
  srand((unsigned)time(NULL) ^ (unsigned)getpid());
  signal(SIGINT, handle_signal);
  signal(SIGTERM, handle_signal);

  printf("[mw] Starting middleware service on port %d\n", MW_PORT);

  /* Create exporter */
  exporter_t *exporter = exporter_create_udp("127.0.0.1", COLLECTOR_PORT);
  if (!exporter || exporter_start(exporter) != 0) {
    fprintf(stderr, "[mw] Failed to create exporter\n");
    return 1;
  }

  /* Create listening socket */
  int listen_sock = socket(AF_INET, SOCK_STREAM, 0);
  if (listen_sock < 0) {
    perror("socket");
    return 1;
  }

  int opt = 1;
  setsockopt(listen_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

  struct sockaddr_in addr = {0};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = INADDR_ANY;
  addr.sin_port = htons(MW_PORT);

  if (bind(listen_sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
    perror("bind");
    close(listen_sock);
    return 1;
  }

  if (listen(listen_sock, 16) < 0) {
    perror("listen");
    close(listen_sock);
    return 1;
  }

  printf("[mw] Listening for connections...\n");

  while (running) {
    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(listen_sock, &fds);
    struct timeval tv = {.tv_sec = 1, .tv_usec = 0};

    int ready = select(listen_sock + 1, &fds, NULL, NULL, &tv);
    if (ready <= 0) {
      continue;
    }

    int client_sock = accept(listen_sock, NULL, NULL);
    if (client_sock < 0) {
      continue;
    }

    handle_connection(client_sock, exporter);
    close(client_sock);
  }

  printf("[mw] Shutting down...\n");
  close(listen_sock);
  exporter_destroy(exporter);
  return 0;
}
