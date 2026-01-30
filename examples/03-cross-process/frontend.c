/**
 * frontend.c - Simulated frontend service
 *
 * Demonstrates cross-process trace propagation by:
 * 1. Creating a trace and span
 * 2. Serializing context
 * 3. Sending to backend via TCP
 * 4. Receiving response
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

#define BACKEND_PORT 7831
#define BACKEND_HOST "127.0.0.1"

int main(void) {
  printf("=== Frontend Service ===\n\n");

  /* Create trace and root span */
  trace_t *trace = trace_create();
  span_t *frontend_span = span_create(trace, NULL, "frontend_request");
  span_annotate(frontend_span, "service", "frontend");
  span_annotate(frontend_span, "endpoint", "/api/users");

  printf("Created trace: %016llx\n", trace->id);
  printf("Created span: %016llx (parent: %016llx)\n", frontend_span->span_id,
         frontend_span->parent_span_id);

  /* Serialize context for RPC */
  uint8_t ctx_buffer[TRACE_CONTEXT_WIRE_SIZE];
  int len = context_inject(frontend_span, ctx_buffer, sizeof(ctx_buffer));
  if (len < 0) {
    fprintf(stderr, "Failed to serialize context\n");
    return 1;
  }

  printf("Serialized context: %d bytes\n", len);
  printf("  Trace ID: %016llx\n", frontend_span->trace_id);
  printf("  Span ID:  %016llx\n", frontend_span->span_id);

  /* Connect to backend */
  int sock = socket(AF_INET, SOCK_STREAM, 0);
  if (sock < 0) {
    perror("socket");
    return 1;
  }

  struct sockaddr_in server_addr = {0};
  server_addr.sin_family = AF_INET;
  server_addr.sin_port = htons(BACKEND_PORT);
  inet_pton(AF_INET, BACKEND_HOST, &server_addr.sin_addr);

  printf("\nConnecting to backend at %s:%d...\n", BACKEND_HOST, BACKEND_PORT);

  if (connect(sock, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
    perror("connect");
    fprintf(stderr, "\nERROR: Could not connect to backend.\n");
    fprintf(stderr, "Make sure backend is running first:\n");
    fprintf(stderr, "  ./examples/03-cross-process/backend\n");
    close(sock);
    return 1;
  }

  printf("Connected! Sending context...\n");

  /* Send context to backend */
  if (send(sock, ctx_buffer, len, 0) < 0) {
    perror("send");
    close(sock);
    return 1;
  }

  /* Wait for backend to process (simulated work) */
  char response[64];
  int n = recv(sock, response, sizeof(response) - 1, 0);
  if (n > 0) {
    response[n] = '\0';
    printf("Received response: %s\n", response);
  }

  close(sock);

  /* Finish frontend span */
  usleep(1000); /* Simulate some final work */
  span_finish(frontend_span);

  printf("\nFrontend span finished:\n");
  printf("  Duration: %.2f ms\n", span_duration_ns(frontend_span) / 1000000.0);
  printf("  Wall-clock start: %llu us since epoch\n",
         frontend_span->wall_start_us);

  trace_destroy(trace);
  printf("\nTrace propagated successfully!\n");

  return 0;
}
