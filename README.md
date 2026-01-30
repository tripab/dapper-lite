# Dapper-Lite: Implementation Plan
## A From-Scratch Distributed Tracing System in C

**Reference Paper**: Sigelman et al., "Dapper: Distributed Tracing Infrastructure" (Google, 2010)

**Project Scope**: Between toy and production-grade; demonstrating what a senior staff engineer would ship as a first launch

**Language**: C (minimal dependencies, maximum control)

---

## Overview

Dapper-Lite is a minimal yet serious distributed tracing system that implements all core features from Google's Dapper paper. The implementation is structured in phases, and the intent is to showcase distributed systems observability, production constraints, and performance-first engineering.

### Key Design Principles

1. **Low overhead** - Instrumentation cost < 100ns/span
2. **Asynchronous reporting** - Zero blocking in hot path
3. **Decoupled architecture** - Services, collectors, and storage are independent
4. **Head-based sampling** - Decision made once per trace and propagated
5. **From scratch** - Minimal external dependencies, maximum learning value

---

## Implementation Phase 1

Phase 1 establishes the foundational data model for distributed tracing:
- Traces and spans with unique IDs
- Parent-child hierarchy
- Monotonic timestamp measurement
- Bounded annotation storage

**What's implemented until now**:
- ✅ Trace creation and lifecycle
- ✅ Span creation, annotation, and finishing
- ✅ In-process parent-child relationships
- ✅ Monotonic time for accurate duration measurement
- ✅ Memory-safe cleanup (trace owns all spans)

**What's deferred**:
- ❌ Thread-local current span
- ❌ Sampling metadata
- ❌ Asynchronous export
- ❌ Wall-clock timestamps
- ❌ Performance benchmarks

## Quick Start

### Build Everything

```bash
make all
```

### Run Examples

```bash
make run-examples
```

### Run Tests

```bash
make run-tests
```

### Memory Check (requires valgrind)

```bash
make valgrind
```

## Project Structure

```
dapper-lite/
├── include/dapper/
│   ├── types.h          # Core data structures
│   ├── trace.h          # Trace lifecycle API
│   └── span.h           # Span lifecycle and annotation API
├── src/core/
│   ├── trace.c          # Trace implementation
│   ├── span.c           # Span implementation
│   └── clock.c          # Monotonic clock wrapper
├── examples/
│   ├── 01-single-span/  # Simplest possible trace
│   └── 02-parent-child/ # Demonstrates hierarchy
├── tests/unit/
│   ├── minunit.h        # Minimal test framework
│   └── test_phase1.c    # Comprehensive unit tests
└── Makefile
```

## API Reference

### Trace Management

```c
// Create a new trace with auto-generated ID
trace_t* trace_create(void);

// Create a trace with specific ID (for cross-process continuity)
trace_t* trace_create_with_id(trace_id_t trace_id);

// Destroy trace and all its spans
void trace_destroy(trace_t* trace);
```

### Span Lifecycle

```c
// Create a span (parent can be NULL for root span)
span_t* span_create(trace_t* trace, span_t* parent, const char* name);

// Add key-value annotation
void span_annotate(span_t* span, const char* key, const char* value);

// Finish span (capture end timestamp)
void span_finish(span_t* span);

// Get span duration in nanoseconds
uint64_t span_duration_ns(const span_t* span);
```

## Examples

### Example 1: Single Span

```c
trace_t* trace = trace_create();
span_t* span = span_create(trace, NULL, "operation");
span_annotate(span, "user_id", "12345");

// Do work...
usleep(10000);

span_finish(span);
printf("Duration: %lu ns\n", span_duration_ns(span));

trace_destroy(trace);
```

### Example 2: Parent-Child Hierarchy

```c
trace_t* trace = trace_create();

span_t* parent = span_create(trace, NULL, "parent");

span_t* child1 = span_create(trace, parent, "child1");
usleep(5000);
span_finish(child1);

span_t* child2 = span_create(trace, parent, "child2");
usleep(3000);
span_finish(child2);

span_finish(parent);

// Verify: parent->first_child == child1
//         child1->next_sibling == child2

trace_destroy(trace);
```

## Design Decisions

### 1. Monotonic Time Only (Phase 1)

Phase 1 uses only monotonic timestamps (`CLOCK_MONOTONIC`):
- **Why**: Immune to NTP adjustments, leap seconds, manual clock changes
- **When**: Span duration calculation
- **Later**: Wall-clock time added in Phase 2 for cross-system correlation

### 2. Explicit Memory Ownership

**Rule**: Trace owns all its spans.

```c
trace_destroy(trace);  // Frees all spans recursively
```

**Benefits**:
- Clear mental model
- No use-after-free bugs
- No reference counting complexity

### 3. Bounded Annotations

Annotations are stored in a fixed-size array (`MAX_ANNOTATIONS = 16`).
- Overflow is **silently ignored** (production behavior)
- No dynamic allocation in hot path
- Predictable memory usage

### 4. Thread Safety

Phase 1 provides basic thread safety:
- ✅ Trace ID generation is atomic
- ✅ Span ID generation is atomic
- ❌ Individual spans are NOT thread-safe (caller must synchronize)

## Test Coverage

### Unit Tests (17 total)

**Trace Tests** 3
**Span Tests** 4
**Hierarchy Tests** 3
**Annotation Tests** 4

## Upcoming implementation phases will address,

1. **No cross-process propagation** - add context serialization
2. **No sampling** - add probabilistic sampling
3. **No export** - add async span reporting
4. **No thread-local span** - add `span_get_current()`

## Memory Layout

### Span Structure Size

```
sizeof(span_t) ≈ 520 bytes
  - 64 bytes: IDs and timestamps
  - 128 bytes: name
  - 4096 bytes: annotations (16 × 256 bytes)
  - 24 bytes: hierarchy pointers
```

### Memory Usage Example

A trace with 100 spans:
- 100 spans × 520 bytes = ~52 KB
- Plus trace overhead: ~100 bytes
- **Total**: ~52 KB

## Next Steps

Planned for phase 2:
1. Thread-local current span management
2. Context propagation (serialize/deserialize)
3. Wall-clock timestamps for cross-system correlation
4. Cross-process trace continuity demo

## Building from Scratch

If starting fresh:

```bash
# Install dependencies (Ubuntu/Debian)
sudo apt-get install build-essential valgrind

# Clone and build
git clone <repo>
cd dapper-lite
make all

# Verify
make run-tests
make run-examples
make valgrind
```
