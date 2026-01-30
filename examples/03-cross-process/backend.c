/**
 * backend.c - Simulated backend service
 *
 * Demonstrates receiving trace context from frontend:
 * 1. Listen for incoming connections
 * 2. Receive serialized context
 * 3. Deserialize and create child span
 * 4. Verify trace continuity
 */

#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L
#include "dapper/context.h"
#include "dapper/span.h"
#include "dapper/trace.h"
#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define LISTEN_PORT 7831

int main(void) {
  printf("=== Backend Service ===\n\n");
  printf("Listening on port %d...\n", LISTEN_PORT);

  /* Create listening socket */
  int listen_sock = socket(AF_INET, SOCK_STREAM, 0);
  if (listen_sock < 0) {
    perror("socket");
    return 1;
  }

  /* Allow reuse of address */
  int opt = 1;
  setsockopt(listen_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

  struct sockaddr_in server_addr = {0};
  server_addr.sin_family = AF_INET;
  server_addr.sin_addr.s_addr = INADDR_ANY;
  server_addr.sin_port = htons(LISTEN_PORT);

  if (bind(listen_sock, (struct sockaddr *)&server_addr, sizeof(server_addr)) <
      0) {
    perror("bind");
    close(listen_sock);
    return 1;
  }

  if (listen(listen_sock, 1) < 0) {
    perror("listen");
    close(listen_sock);
    return 1;
  }

  printf("Waiting for frontend connection...\n\n");

  /* Accept connection */
  int client_sock = accept(listen_sock, NULL, NULL);
  if (client_sock < 0) {
    perror("accept");
    close(listen_sock);
    return 1;
  }

  printf("Frontend connected!\n");

  /* Receive serialized context */
  uint8_t ctx_buffer[TRACE_CONTEXT_WIRE_SIZE];
  int n = recv(client_sock, ctx_buffer, sizeof(ctx_buffer), 0);
  if (n != TRACE_CONTEXT_WIRE_SIZE) {
    fprintf(stderr, "Failed to receive complete context (got %d bytes)\n", n);
    close(client_sock);
    close(listen_sock);
    return 1;
  }

  printf("Received context: %d bytes\n", n);

  /* Deserialize context */
  trace_context_t ctx;
  if (context_extract(&ctx, ctx_buffer, sizeof(ctx_buffer)) != 0) {
    fprintf(stderr, "Failed to deserialize context\n");
    close(client_sock);
    close(listen_sock);
    return 1;
  }

  printf("Extracted context:\n");
  printf("  Trace ID: %016llx\n", ctx.trace_id);
  printf("  Parent Span ID: %016llx\n", ctx.span_id);

  /* Create trace and span from context */
  trace_t *trace = trace_create(); /* Will be updated with extracted ID */
  span_t *backend_span = span_create_from_context(trace, &ctx, "backend_query");
  if (!backend_span) {
    fprintf(stderr, "Failed to create span from context\n");
    close(client_sock);
    close(listen_sock);
    return 1;
  }

  span_annotate(backend_span, "service", "backend");
  span_annotate(backend_span, "db", "users_table");

  printf("\nCreated backend span:\n");
  printf("  Trace ID: %016llx (inherited)\n", backend_span->trace_id);
  printf("  Span ID: %016llx\n", backend_span->span_id);
  printf("  Parent Span ID: %016llx (from frontend)\n",
         backend_span->parent_span_id);

  /* Verify trace continuity */
  printf("\n=== Trace Continuity Verification ===\n");
  printf("Trace IDs match: %s\n",
         (trace->id == ctx.trace_id) ? "YES ✓" : "NO ✗");
  printf("Parent span ID matches frontend: %s\n",
         (backend_span->parent_span_id == ctx.span_id) ? "YES ✓" : "NO ✗");

  /* Simulate backend work */
  printf("\nProcessing backend query...\n");
  usleep(5000); /* 5ms of work */

  /* Finish span */
  span_finish(backend_span);

  printf("Backend span finished:\n");
  printf("  Duration: %.2f ms\n", span_duration_ns(backend_span) / 1000000.0);
  printf("  Wall-clock start: %llu us since epoch\n",
         backend_span->wall_start_us);

  /* Send response */
  const char *response = "OK";
  send(client_sock, response, strlen(response), 0);

  close(client_sock);
  close(listen_sock);

  trace_destroy(trace);
  printf("\nBackend processed trace successfully!\n");
  printf("Single trace propagated across two processes ✓\n");

  return 0;
}
