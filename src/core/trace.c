/**
 * trace.c - Trace lifecycle implementation
 */

#include "dapper/trace.h"
#include "dapper/span.h"
#include <stdlib.h>
#include <stdatomic.h>

/* Global trace ID counter (atomic for thread safety) */
static _Atomic uint64_t g_next_trace_id = 1;

/**
 * Generate a unique trace ID
 */
static trace_id_t generate_trace_id(void)
{
    return atomic_fetch_add(&g_next_trace_id, 1);
}

trace_t *trace_create(void)
{
    return trace_create_with_id(generate_trace_id());
}

trace_t *trace_create_with_id(trace_id_t trace_id)
{
    trace_t *trace = calloc(1, sizeof(trace_t));
    if (!trace)
    {
        return NULL;
    }

    trace->id = trace_id;
    trace->root_span = NULL;

    return trace;
}

/**
 * Recursively destroy a span and all its descendants
 */
static void span_destroy_recursive(span_t *span)
{
    if (!span)
    {
        return;
    }

    /* Destroy all children first */
    span_t *child = span->first_child;
    while (child)
    {
        span_t *next = child->next_sibling;
        span_destroy_recursive(child);
        child = next;
    }

    /* Now safe to free this span */
    free(span);
}

void trace_destroy(trace_t *trace)
{
    if (!trace)
    {
        return;
    }

    /* Destroy all spans in the trace */
    span_destroy_recursive(trace->root_span);

    free(trace);
}
