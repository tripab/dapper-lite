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

## Implementation Phases

Phase 1 establishes the foundational data model for distributed tracing:
- Traces and spans with unique IDs
- Parent-child hierarchy
- Monotonic timestamp measurement
- Bounded annotation storage
Phase 2 implements context propagation
- Thread-local current span management
- Cross-process context propagation (16-byte wire format)
- Wall-clock timestamps for cross-system correlation
- Endian-safe serialization

**What's implemented until now**:
- ✅ Trace creation and lifecycle
- ✅ Span creation, annotation, and finishing
- ✅ In-process parent-child relationships
- ✅ Monotonic time for accurate duration measurement
- ✅ Wall-clock timestamps for cross-system correlation
- ✅ Thread-local current span management
- ✅ Context propagation across process boundaries
- ✅ Memory-safe cleanup (trace owns all spans)

**What's deferred**:
- ❌ Sampling metadata
- ❌ Asynchronous export
- ❌ Collector service
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
│   ├── span.h           # Span lifecycle and annotation API
│   └── context.h        # Context propagation
├── src/core/
│   ├── trace.c          # Trace implementation
│   ├── span.c           # Span implementation
│   ├── clock.c          # Monotonic clock wrapper
│   ├── thread_local.c   # Thread-local storage
│   └── context.c        # Serialization
├── examples/
│   ├── 01-single-span/  # Simplest possible trace
│   ├── 02-parent-child/ # Demonstrates hierarchy
│   └── 03-cross-process/ # Cross-process propagation
├── tests/unit/
│   ├── minunit.h        # Minimal test framework
│   ├── test_phase1.c    # Phase 1 tests (14 tests)
│   └── test_phase2.c    # Phase 2 tests (9 tests)
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

### Thread-Local Context

```c
void span_set_current(span_t* span);
span_t* span_get_current(void);
```

### Context Propagation

```c
// Wire format size
#define TRACE_CONTEXT_WIRE_SIZE 16

// Context structure
typedef struct {
    trace_id_t trace_id;
    span_id_t span_id;
} trace_context_t;

// Serialize
int context_inject(const span_t* span, uint8_t* buffer, size_t bufsize);

// Deserialize
int context_extract(trace_context_t* ctx, const uint8_t* buffer, size_t bufsize);

// Create span from remote context
span_t* span_create_from_context(trace_t* trace, const trace_context_t* ctx, 
                                   const char* name);
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

### Example 3: Cross-Process Flow

- **frontend.c** - Initiates request, serializes context, sends via TCP
- **backend.c** - Receives context, deserializes, continues trace
- **run.sh** - Orchestrates cross-process demo

**Frontend Process**
```
1. Create trace & span
2. Serialize context (inject)
   ┌──────────────────────┐
   │ Trace ID: 0x0001     │
   │ Span ID:  0x0001     │
   └──────────────────────┘
3. Send via TCP to backend
4. Continue processing
5. Finish span
```

**Backend Process**
```
1. Receive context bytes
2. Deserialize (extract)
   ┌──────────────────────┐
   │ Trace ID: 0x0001     │
   │ Parent:   0x0001     │
   └──────────────────────┘
3. Create span from context
   ┌──────────────────────────┐
   │ NEW Span ID: 0x0002      │
   │ Trace ID: 0x0001 (same!) │
   │ Parent: 0x0001 (remote)  │
   └──────────────────────────┘
4. Process request
5. Finish span
```

**Result**: Single trace spans two processes.

---

## Integration Example

### HTTP-like Service

```c
// Middleware: Extract context from headers
void handle_request(http_request_t* req) {
    trace_t* trace = trace_create();
    span_t* span;
    
    // Check for trace context in headers
    uint8_t* ctx_header = http_get_header(req, "X-Trace-Context");
    if (ctx_header) {
        trace_context_t ctx;
        context_extract(&ctx, ctx_header, TRACE_CONTEXT_WIRE_SIZE);
        span = span_create_from_context(trace, &ctx, "http_handler");
    } else {
        // New trace
        span = span_create(trace, NULL, "http_handler");
    }
    
    span_set_current(span);  // Set as current for this thread
    
    // Call business logic (can use span_get_current() to detect parent)
    process_business_logic(req);
    
    span_finish(span);
    trace_destroy(trace);
}
```

---

## Test Coverage

### Unit Tests (23 total)

- Trace Tests
- Span Tests
- Hierarchy Tests
- Annotation Tests
- Thread-Local Tests
- Context Serialization Tests
- Cross-Process Span Tests
- Wall-Clock Timestamp Tests

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
