/**
 * types.h - Core data structures for Dapper-Lite
 * Phase 1: Semantic correctness only (minimal design)
 */

#ifndef TYPES_H
#define TYPES_H

#include <stdint.h>
#include <stdbool.h>

/* Configuration constants */
#define SPAN_NAME_MAX_LENGTH 128
#define MAX_ANNOTATIONS 16
#define ANNOTATION_KEY_MAX_LENGTH 64
#define ANNOTATION_VALUE_MAX_LENGTH 192

/* Type aliases for clarity */
typedef uint64_t trace_id_t;
typedef uint64_t span_id_t;

/**
 * Annotation - Key-value metadata attached to spans
 */
typedef struct
{
    char key[ANNOTATION_KEY_MAX_LENGTH];
    char value[ANNOTATION_VALUE_MAX_LENGTH];
} annotation_t;

/**
 * Span - Represents a single operation within a trace
 *
 * Phase 1 design:
 * - Monotonic timestamps only (wall-clock deferred to an upcoming phase)
 * - In-process parent-child relationships
 * - No sampling metadata (deferred to an upcoming phase)
 * - No export-related fields (deferred to an upcoming phase)
 */
typedef struct span
{
    trace_id_t trace_id;
    span_id_t span_id;
    span_id_t parent_span_id; /* 0 if root span */
    char name[SPAN_NAME_MAX_LENGTH];

    /* Phase 1: Monotonic time for accurate duration measurement */
    uint64_t monotonic_start_ns;
    uint64_t monotonic_end_ns;

    /* Annotations (bounded array) */
    annotation_t annotations[MAX_ANNOTATIONS];
    int annotation_count;

    /* In-process hierarchy */
    struct span *parent;       /* Parent span (NULL if root) */
    struct span *first_child;  /* First child in linked list */
    struct span *next_sibling; /* Next sibling in parent's child list */
} span_t;

/**
 * Trace - Container for related spans
 *
 * Phase 1 design:
 * - Simple container with unique ID
 * - Sampling metadata deferred to an upcoming phase
 */
typedef struct
{
    trace_id_t id;
    span_t *root_span;
} trace_t;

#endif /* TYPES_H */
