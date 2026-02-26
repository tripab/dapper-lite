/**
 * database.c - Simulated database service for full-system demo
 *
 * Listens on TCP for trace context + query type, simulates DB work
 * with variable latency, and sends response back. Exports spans
 * to the collector via UDP.
 */

#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L
#include "dapper/context.h"
#include "dapper/exporter.h"
#include "dapper/sampler.h"
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

#define DB_PORT 7835
#define COLLECTOR_PORT 7831

static volatile int running = 1;

static void handle_signal(int sig) {
  (void)sig;
  running = 0;
}

/**
 * Simulate database query latency.
 * Most queries are fast (1-5ms), but some are slow (50-200ms)
 * to demonstrate tail latency.
 */
static int simulate_query(const char *query_type) {
  int base_us;
  if (strcmp(query_type, "slow") == 0) {
    /* Simulate slow query: 50-200ms */
    base_us = 50000 + (rand() % 150000);
  } else {
    /* Fast query: 1-5ms */
    base_us = 1000 + (rand() % 4000);
  }
  usleep((unsigned)base_us);
  return base_us;
}

static void handle_connection(int client_sock, exporter_t *exporter) {
  /* Receive: context bytes + 1 byte query_type_len + query_type string */
  uint8_t buf[256];
  int n = (int)recv(client_sock, buf, sizeof(buf), 0);
  if (n < TRACE_CONTEXT_WIRE_SIZE + 1) {
    fprintf(stderr, "[db] Short read: %d bytes\n", n);
    return;
  }

  /* Extract trace context */
  trace_context_t ctx;
  if (context_extract(&ctx, buf, TRACE_CONTEXT_WIRE_SIZE) != 0) {
    fprintf(stderr, "[db] Failed to extract context\n");
    return;
  }

  /* Extract query type */
  int qt_len = buf[TRACE_CONTEXT_WIRE_SIZE];
  char query_type[64] = "default";
  if (qt_len > 0 && TRACE_CONTEXT_WIRE_SIZE + 1 + qt_len <= n) {
    int copy_len = qt_len < (int)sizeof(query_type) - 1
                       ? qt_len
                       : (int)sizeof(query_type) - 1;
    memcpy(query_type, buf + TRACE_CONTEXT_WIRE_SIZE + 1, (size_t)copy_len);
    query_type[copy_len] = '\0';
  }

  /* Create trace and span from received context */
  trace_t *trace = trace_create_with_id(ctx.trace_id);
  trace->sampled = true; /* Demo: always sample */
  span_t *db_span = span_create_from_context(trace, &ctx, "db_query");
  span_annotate(db_span, "service", "database");
  span_annotate(db_span, "db.type", "postgresql");
  span_annotate(db_span, "db.query_type", query_type);

  /* Simulate query */
  int actual_us = simulate_query(query_type);

  span_finish(db_span);

  /* Export span to collector */
  if (trace->sampled) {
    exporter_submit(exporter, db_span);
  }

  /* Send response: 4 bytes latency (network order) */
  uint32_t resp = htonl((uint32_t)actual_us);
  send(client_sock, &resp, sizeof(resp), 0);

  printf("[db] query=%s latency=%dus trace=%016llx\n", query_type, actual_us,
         (unsigned long long)trace->id);

  trace_destroy(trace);
}

int main(void) {
  srand((unsigned)time(NULL) ^ (unsigned)getpid());
  signal(SIGINT, handle_signal);
  signal(SIGTERM, handle_signal);

  printf("[db] Starting database service on port %d\n", DB_PORT);

  /* Create exporter */
  exporter_t *exporter = exporter_create_udp("127.0.0.1", COLLECTOR_PORT);
  if (!exporter || exporter_start(exporter) != 0) {
    fprintf(stderr, "[db] Failed to create exporter\n");
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
  addr.sin_port = htons(DB_PORT);

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

  printf("[db] Listening for connections...\n");

  while (running) {
    /* Use select for timeout so we can check running flag */
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

  printf("[db] Shutting down...\n");
  close(listen_sock);
  exporter_destroy(exporter);
  return 0;
}
