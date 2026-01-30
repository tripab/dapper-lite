/**
 * 02-parent-child - Demonstrates span hierarchy
 */

#include "dapper/span.h"
#include "dapper/trace.h"
#include <stdio.h>
#include <unistd.h>

void print_span_tree(span_t *span, int depth) {
  if (!span) {
    return;
  }

  /* Print indentation */
  for (int i = 0; i < depth; i++) {
    printf("  ");
  }

  /* Print span info */
  uint64_t duration = span_duration_ns(span);
  printf("├─ %s (ID: %016llx, Duration: %.2f ms)\n", span->name, span->span_id,
         duration / 1000000.0);

  /* Print children */
  span_t *child = span->first_child;
  while (child) {
    print_span_tree(child, depth + 1);
    child = child->next_sibling;
  }
}

int main(void) {
  printf("=== Parent-Child Hierarchy Example ===\n\n");

  /* Create trace */
  trace_t *trace = trace_create();
  printf("Trace ID: %016llx\n\n", trace->id);

  /* Create parent span */
  span_t *parent = span_create(trace, NULL, "parent_operation");
  span_annotate(parent, "type", "parent");

  /* Simulate some work before children */
  usleep(2000); /* 2ms */

  /* Create first child */
  span_t *child1 = span_create(trace, parent, "child_op_1");
  span_annotate(child1, "db", "users");
  usleep(5000); /* 5ms */
  span_finish(child1);

  /* Simulate work between children */
  usleep(1000); /* 1ms */

  /* Create second child */
  span_t *child2 = span_create(trace, parent, "child_op_2");
  span_annotate(child2, "db", "orders");
  usleep(1000); /* 1ms */

  /* Create nested child (grandchild) BEFORE finishing child2 */
  span_t *grandchild = span_create(trace, child2, "nested_query");
  span_annotate(grandchild, "cache", "hit");
  usleep(1000); /* 1ms */
  span_finish(grandchild);

  usleep(1000); /* 1ms more work in child2 */
  span_finish(child2);

  /* Some more work in parent */
  usleep(2000); /* 2ms */

  /* Finish parent */
  span_finish(parent);

  /* Display hierarchy */
  printf("Span Hierarchy:\n");
  print_span_tree(trace->root_span, 0);

  /* Verify parent-child relationships */
  printf("\n=== Verification ===\n");
  printf("Parent's first child: %s\n",
         parent->first_child ? parent->first_child->name : "NULL");
  printf("Child1's next sibling: %s\n",
         child1->next_sibling ? child1->next_sibling->name : "NULL");
  printf("Child2's parent: %s\n",
         child2->parent ? child2->parent->name : "NULL");
  printf("Grandchild's parent: %s\n",
         grandchild->parent ? grandchild->parent->name : "NULL");

  /* Verify timing constraints */
  printf("\n=== Timing Constraints ===\n");
  printf("Parent encompasses child1: %s\n",
         (parent->monotonic_start_ns <= child1->monotonic_start_ns &&
          child1->monotonic_end_ns <= parent->monotonic_end_ns)
             ? "YES"
             : "NO");
  printf("Parent encompasses child2: %s\n",
         (parent->monotonic_start_ns <= child2->monotonic_start_ns &&
          child2->monotonic_end_ns <= parent->monotonic_end_ns)
             ? "YES"
             : "NO");
  printf("Child2 encompasses grandchild: %s\n",
         (child2->monotonic_start_ns <= grandchild->monotonic_start_ns &&
          grandchild->monotonic_end_ns <= child2->monotonic_end_ns)
             ? "YES"
             : "NO");

  /* Cleanup */
  trace_destroy(trace);
  printf("\nTrace destroyed successfully\n");

  return 0;
}
