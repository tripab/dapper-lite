/**
 * 01-single-span - Simplest possible trace with one span
 */

#include "dapper/span.h"
#include "dapper/trace.h"
#include <stdio.h>
#include <unistd.h>

int main(void) {
  printf("=== Single Span Example ===\n\n");

  /* Create a new trace */
  trace_t *trace = trace_create();
  if (!trace) {
    fprintf(stderr, "Failed to create trace\n");
    return 1;
  }

  printf("Created trace with ID: %016llx\n", trace->id);

  /* Create a root span */
  span_t *span = span_create(trace, NULL, "main_operation");
  if (!span) {
    fprintf(stderr, "Failed to create span\n");
    trace_destroy(trace);
    return 1;
  }

  printf("Created span '%s' with ID: %016llx\n", span->name, span->span_id);

  /* Add some annotations */
  span_annotate(span, "user_id", "12345");
  span_annotate(span, "endpoint", "/api/users");
  span_annotate(span, "method", "GET");

  printf("Added %d annotations\n", span->annotation_count);

  /* Simulate work */
  printf("Doing work...\n");
  usleep(10000); /* 10ms */

  /* Finish the span */
  span_finish(span);

  /* Display results */
  uint64_t duration = span_duration_ns(span);
  printf("\nSpan finished:\n");
  printf("  Duration: %llu ns (%.2f ms)\n", duration, duration / 1000000.0);
  printf("  Annotations:\n");
  for (int i = 0; i < span->annotation_count; i++) {
    printf("    %s = %s\n", span->annotations[i].key,
           span->annotations[i].value);
  }

  /* Cleanup */
  trace_destroy(trace);
  printf("\nTrace destroyed successfully\n");

  return 0;
}
