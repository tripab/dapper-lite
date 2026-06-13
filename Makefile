# Makefile for Dapper-Lite
# Builds core library, examples, and tests with separate build directory

CC = gcc
CFLAGS = -Wall -Wextra -Werror -std=c11 -O2 -g
INCLUDES = -Iinclude
LIBS = -lpthread -lm

# Build directory structure
BUILD_DIR = build
OBJ_DIR = $(BUILD_DIR)/obj
BIN_DIR = $(BUILD_DIR)/bin
TEST_DIR = $(BUILD_DIR)/test

# ---- Object sets, split to match the architecture layers ----
# Each binary links only the layers it actually uses. The dependency
# direction is: core <- sampling, wire; wire <- export, collector,
# analysis. core/trace.c references the sampler, so CORE and SAMPLING
# are always linked together as BASE_OBJS.

# core: trace/span/clock/thread-local/context
CORE_SRCS = src/core/trace.c src/core/span.c src/core/clock.c src/core/thread_local.c src/core/context.c
CORE_OBJS = $(patsubst src/%.c,$(OBJ_DIR)/%.o,$(CORE_SRCS))

# sampling: head-based samplers
SAMPLING_SRCS = src/sampling/sampler.c
SAMPLING_OBJS = $(patsubst src/%.c,$(OBJ_DIR)/%.o,$(SAMPLING_SRCS))

# wire: neutral span codec shared by export, collector, and analysis
WIRE_SRCS = src/wire/serialize.c
WIRE_OBJS = $(patsubst src/%.c,$(OBJ_DIR)/%.o,$(WIRE_SRCS))

# export: ring buffer, sinks, background exporter thread
EXPORT_SRCS = src/export/ring_buffer.c src/export/file_sink.c src/export/udp_sink.c src/export/exporter_thread.c
EXPORT_OBJS = $(patsubst src/%.c,$(OBJ_DIR)/%.o,$(EXPORT_SRCS))

# analysis: query, critical path, aggregation, JSON export
ANALYSIS_SRCS = src/analysis/query.c src/analysis/critical_path.c src/analysis/aggregation.c src/analysis/export_json.c
ANALYSIS_OBJS = $(patsubst src/%.c,$(OBJ_DIR)/%.o,$(ANALYSIS_SRCS))

# Base runtime needed by every binary (core + the sampler it references)
BASE_OBJS = $(CORE_OBJS) $(SAMPLING_OBJS)

# Collector source files
COLLECTOR_SRCS = src/collector/protocol.c src/collector/receiver.c src/collector/assembler.c src/collector/storage.c src/collector/main.c
COLLECTOR_OBJS = $(patsubst src/%.c,$(OBJ_DIR)/%.o,$(COLLECTOR_SRCS))
# Collector objects without main (for linking with tests)
COLLECTOR_LIB_OBJS = $(filter-out $(OBJ_DIR)/collector/main.o,$(COLLECTOR_OBJS))

# Example sources
EXAMPLE1_SRC = examples/01-single-span/main.c
EXAMPLE2_SRC = examples/02-parent-child/main.c
EXAMPLE3_FRONTEND_SRC = examples/03-cross-process/frontend.c
EXAMPLE3_BACKEND_SRC = examples/03-cross-process/backend.c
EXAMPLE4_SRC = examples/04-sampling/main.c
EXAMPLE5_FRONTEND_SRC = examples/05-full-system/frontend.c
EXAMPLE5_MIDDLEWARE_SRC = examples/05-full-system/middleware.c
EXAMPLE5_DATABASE_SRC = examples/05-full-system/database.c

EXAMPLE1_OBJ = $(OBJ_DIR)/examples/01-single-span/main.o
EXAMPLE2_OBJ = $(OBJ_DIR)/examples/02-parent-child/main.o
EXAMPLE3_FRONTEND_OBJ = $(OBJ_DIR)/examples/03-cross-process/frontend.o
EXAMPLE3_BACKEND_OBJ = $(OBJ_DIR)/examples/03-cross-process/backend.o
EXAMPLE4_OBJ = $(OBJ_DIR)/examples/04-sampling/main.o
EXAMPLE5_FRONTEND_OBJ = $(OBJ_DIR)/examples/05-full-system/frontend.o
EXAMPLE5_MIDDLEWARE_OBJ = $(OBJ_DIR)/examples/05-full-system/middleware.o
EXAMPLE5_DATABASE_OBJ = $(OBJ_DIR)/examples/05-full-system/database.o

# Test sources
TEST1_SRC = tests/unit/test_phase1.c
TEST2_SRC = tests/unit/test_phase2.c
TEST3_SRC = tests/unit/test_phase3.c
TEST4_SRC = tests/unit/test_phase4.c
TEST5_SRC = tests/unit/test_phase5.c
TEST6_SRC = tests/unit/test_phase6.c

TEST1_OBJ = $(OBJ_DIR)/tests/unit/test_phase1.o
TEST2_OBJ = $(OBJ_DIR)/tests/unit/test_phase2.o
TEST3_OBJ = $(OBJ_DIR)/tests/unit/test_phase3.o
TEST4_OBJ = $(OBJ_DIR)/tests/unit/test_phase4.o
TEST5_OBJ = $(OBJ_DIR)/tests/unit/test_phase5.o
TEST6_OBJ = $(OBJ_DIR)/tests/unit/test_phase6.o

# Benchmark sources
BENCH_SAMPLING_DECISION_SRC = benchmarks/sampling_decision.c
BENCH_TRACE_CREATION_SRC = benchmarks/trace_creation.c
BENCH_RING_BUFFER_SRC = benchmarks/ring_buffer_throughput.c
BENCH_EXPORT_SRC = benchmarks/export_throughput.c
BENCH_CONTEXT_INJECT_SRC = benchmarks/context_inject.c
BENCH_SPAN_CREATION_SRC = benchmarks/span_creation.c
BENCH_COLLECTOR_INGEST_SRC = benchmarks/collector_ingest.c
BENCH_OVERHEAD_SRC = benchmarks/overhead_analysis.c

BENCH_SAMPLING_DECISION_OBJ = $(OBJ_DIR)/benchmarks/sampling_decision.o
BENCH_TRACE_CREATION_OBJ = $(OBJ_DIR)/benchmarks/trace_creation.o
BENCH_RING_BUFFER_OBJ = $(OBJ_DIR)/benchmarks/ring_buffer_throughput.o
BENCH_EXPORT_OBJ = $(OBJ_DIR)/benchmarks/export_throughput.o
BENCH_CONTEXT_INJECT_OBJ = $(OBJ_DIR)/benchmarks/context_inject.o
BENCH_SPAN_CREATION_OBJ = $(OBJ_DIR)/benchmarks/span_creation.o
BENCH_COLLECTOR_INGEST_OBJ = $(OBJ_DIR)/benchmarks/collector_ingest.o
BENCH_OVERHEAD_OBJ = $(OBJ_DIR)/benchmarks/overhead_analysis.o

# Binaries
EXAMPLE1_BIN = $(BIN_DIR)/example1-single-span
EXAMPLE2_BIN = $(BIN_DIR)/example2-parent-child
EXAMPLE3_FRONTEND_BIN = $(BIN_DIR)/example3-frontend
EXAMPLE3_BACKEND_BIN = $(BIN_DIR)/example3-backend
EXAMPLE4_BIN = $(BIN_DIR)/example4-sampling
EXAMPLE5_FRONTEND_BIN = $(BIN_DIR)/example5-frontend
EXAMPLE5_MIDDLEWARE_BIN = $(BIN_DIR)/example5-middleware
EXAMPLE5_DATABASE_BIN = $(BIN_DIR)/example5-database

TEST1_BIN = $(TEST_DIR)/test_phase1
TEST2_BIN = $(TEST_DIR)/test_phase2
TEST3_BIN = $(TEST_DIR)/test_phase3
TEST4_BIN = $(TEST_DIR)/test_phase4
TEST5_BIN = $(TEST_DIR)/test_phase5
TEST6_BIN = $(TEST_DIR)/test_phase6

COLLECTOR_BIN = $(BIN_DIR)/collector

BENCH_SAMPLING_DECISION_BIN = $(BIN_DIR)/bench_sampling_decision
BENCH_TRACE_CREATION_BIN = $(BIN_DIR)/bench_trace_creation
BENCH_RING_BUFFER_BIN = $(BIN_DIR)/bench_ring_buffer
BENCH_EXPORT_BIN = $(BIN_DIR)/bench_export
BENCH_CONTEXT_INJECT_BIN = $(BIN_DIR)/bench_context_inject
BENCH_SPAN_CREATION_BIN = $(BIN_DIR)/bench_span_creation
BENCH_COLLECTOR_INGEST_BIN = $(BIN_DIR)/bench_collector_ingest
BENCH_OVERHEAD_BIN = $(BIN_DIR)/bench_overhead

EXAMPLES = $(EXAMPLE1_BIN) $(EXAMPLE2_BIN) $(EXAMPLE3_FRONTEND_BIN) $(EXAMPLE3_BACKEND_BIN) $(EXAMPLE4_BIN) $(EXAMPLE5_FRONTEND_BIN) $(EXAMPLE5_MIDDLEWARE_BIN) $(EXAMPLE5_DATABASE_BIN)
TESTS = $(TEST1_BIN) $(TEST2_BIN) $(TEST3_BIN) $(TEST4_BIN) $(TEST5_BIN) $(TEST6_BIN)
BENCHMARKS = $(BENCH_SAMPLING_DECISION_BIN) $(BENCH_TRACE_CREATION_BIN) $(BENCH_RING_BUFFER_BIN) $(BENCH_EXPORT_BIN) $(BENCH_CONTEXT_INJECT_BIN) $(BENCH_SPAN_CREATION_BIN) $(BENCH_COLLECTOR_INGEST_BIN) $(BENCH_OVERHEAD_BIN)

# Source files for formatting
FORMAT_SRCS = $(shell find src include examples tests -name '*.c' -o -name '*.h' 2>/dev/null | grep -v minunit.h)

# Check for clang-format
CLANG_FORMAT := $(shell command -v clang-format 2> /dev/null)

.PHONY: all clean examples tests collector run-examples run-tests run-full-system run-visualization valgrind format format-check dirs help

all: format dirs examples tests collector

# Create build directories
dirs:
	@mkdir -p $(OBJ_DIR)/core
	@mkdir -p $(OBJ_DIR)/sampling
	@mkdir -p $(OBJ_DIR)/wire
	@mkdir -p $(OBJ_DIR)/export
	@mkdir -p $(OBJ_DIR)/examples/01-single-span
	@mkdir -p $(OBJ_DIR)/examples/02-parent-child
	@mkdir -p $(OBJ_DIR)/examples/03-cross-process
	@mkdir -p $(OBJ_DIR)/examples/04-sampling
	@mkdir -p $(OBJ_DIR)/examples/05-full-system
	@mkdir -p $(OBJ_DIR)/analysis
	@mkdir -p $(OBJ_DIR)/collector
	@mkdir -p $(OBJ_DIR)/tests/unit
	@mkdir -p $(OBJ_DIR)/benchmarks
	@mkdir -p $(BIN_DIR)
	@mkdir -p $(TEST_DIR)

# Compile object files
$(OBJ_DIR)/%.o: src/%.c | dirs
	@echo "Compiling $<"
	@$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

$(OBJ_DIR)/examples/%.o: examples/%.c | dirs
	@echo "Compiling $<"
	@$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

$(OBJ_DIR)/tests/%.o: tests/%.c | dirs
	@echo "Compiling $<"
	@$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

$(OBJ_DIR)/benchmarks/%.o: benchmarks/%.c | dirs
	@echo "Compiling $<"
	@$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

# Link examples
# Examples 1-4 use only the base library (traces, spans, context, sampling).
$(EXAMPLE1_BIN): $(EXAMPLE1_OBJ) $(BASE_OBJS)
	@echo "Linking $@"
	@$(CC) $(CFLAGS) $^ -o $@ $(LIBS)

$(EXAMPLE2_BIN): $(EXAMPLE2_OBJ) $(BASE_OBJS)
	@echo "Linking $@"
	@$(CC) $(CFLAGS) $^ -o $@ $(LIBS)

$(EXAMPLE3_FRONTEND_BIN): $(EXAMPLE3_FRONTEND_OBJ) $(BASE_OBJS)
	@echo "Linking $@"
	@$(CC) $(CFLAGS) $^ -o $@ $(LIBS)

$(EXAMPLE3_BACKEND_BIN): $(EXAMPLE3_BACKEND_OBJ) $(BASE_OBJS)
	@echo "Linking $@"
	@$(CC) $(CFLAGS) $^ -o $@ $(LIBS)

$(EXAMPLE4_BIN): $(EXAMPLE4_OBJ) $(BASE_OBJS)
	@echo "Linking $@"
	@$(CC) $(CFLAGS) $^ -o $@ $(LIBS)

# The full-system demo exports spans, so it adds the wire + export layers.
$(EXAMPLE5_FRONTEND_BIN): $(EXAMPLE5_FRONTEND_OBJ) $(BASE_OBJS) $(WIRE_OBJS) $(EXPORT_OBJS) $(ANALYSIS_OBJS)
	@echo "Linking $@"
	@$(CC) $(CFLAGS) $^ -o $@ $(LIBS)

$(EXAMPLE5_MIDDLEWARE_BIN): $(EXAMPLE5_MIDDLEWARE_OBJ) $(BASE_OBJS) $(WIRE_OBJS) $(EXPORT_OBJS)
	@echo "Linking $@"
	@$(CC) $(CFLAGS) $^ -o $@ $(LIBS)

$(EXAMPLE5_DATABASE_BIN): $(EXAMPLE5_DATABASE_OBJ) $(BASE_OBJS) $(WIRE_OBJS) $(EXPORT_OBJS)
	@echo "Linking $@"
	@$(CC) $(CFLAGS) $^ -o $@ $(LIBS)

# Link tests
$(TEST1_BIN): $(TEST1_OBJ) $(BASE_OBJS)
	@echo "Linking $@"
	@$(CC) $(CFLAGS) $^ -o $@ $(LIBS)

$(TEST2_BIN): $(TEST2_OBJ) $(BASE_OBJS)
	@echo "Linking $@"
	@$(CC) $(CFLAGS) $^ -o $@ $(LIBS)

$(TEST3_BIN): $(TEST3_OBJ) $(BASE_OBJS)
	@echo "Linking $@"
	@$(CC) $(CFLAGS) $^ -o $@ $(LIBS) -lm

# Phase 4 exercises the wire codec, ring buffer, sinks, and exporter.
$(TEST4_BIN): $(TEST4_OBJ) $(BASE_OBJS) $(WIRE_OBJS) $(EXPORT_OBJS)
	@echo "Linking $@"
	@$(CC) $(CFLAGS) $^ -o $@ $(LIBS)

# Phase 5 test needs collector objects + COLLECTOR_NO_MAIN to avoid duplicate main
$(OBJ_DIR)/collector/main.test.o: src/collector/main.c | dirs
	@echo "Compiling $< (no main)"
	@$(CC) $(CFLAGS) $(INCLUDES) -DCOLLECTOR_NO_MAIN -c $< -o $@

# Phase 5: wire codec + collector library (no analysis, no export thread).
$(TEST5_BIN): $(TEST5_OBJ) $(BASE_OBJS) $(WIRE_OBJS) $(COLLECTOR_LIB_OBJS) $(OBJ_DIR)/collector/main.test.o
	@echo "Linking $@"
	@$(CC) $(CFLAGS) $^ -o $@ $(LIBS)

# Phase 6: analysis layer plus the collector storage helpers it reads.
$(TEST6_BIN): $(TEST6_OBJ) $(BASE_OBJS) $(WIRE_OBJS) $(ANALYSIS_OBJS) $(COLLECTOR_LIB_OBJS) $(OBJ_DIR)/collector/main.test.o
	@echo "Linking $@"
	@$(CC) $(CFLAGS) $^ -o $@ $(LIBS)

# Collector daemon binary: base + wire + collector library + main.
$(COLLECTOR_BIN): $(COLLECTOR_OBJS) $(BASE_OBJS) $(WIRE_OBJS)
	@echo "Linking $@"
	@$(CC) $(CFLAGS) $^ -o $@ $(LIBS)

# Link benchmarks
$(BENCH_SAMPLING_DECISION_BIN): $(BENCH_SAMPLING_DECISION_OBJ) $(BASE_OBJS)
	@echo "Linking $@"
	@$(CC) $(CFLAGS) $^ -o $@ $(LIBS) -lm

$(BENCH_TRACE_CREATION_BIN): $(BENCH_TRACE_CREATION_OBJ) $(BASE_OBJS)
	@echo "Linking $@"
	@$(CC) $(CFLAGS) $^ -o $@ $(LIBS) -lm

$(BENCH_RING_BUFFER_BIN): $(BENCH_RING_BUFFER_OBJ) $(BASE_OBJS) $(WIRE_OBJS) $(EXPORT_OBJS)
	@echo "Linking $@"
	@$(CC) $(CFLAGS) $^ -o $@ $(LIBS)

$(BENCH_EXPORT_BIN): $(BENCH_EXPORT_OBJ) $(BASE_OBJS) $(WIRE_OBJS) $(EXPORT_OBJS)
	@echo "Linking $@"
	@$(CC) $(CFLAGS) $^ -o $@ $(LIBS)

$(BENCH_CONTEXT_INJECT_BIN): $(BENCH_CONTEXT_INJECT_OBJ) $(BASE_OBJS)
	@echo "Linking $@"
	@$(CC) $(CFLAGS) $^ -o $@ $(LIBS)

$(BENCH_SPAN_CREATION_BIN): $(BENCH_SPAN_CREATION_OBJ) $(BASE_OBJS)
	@echo "Linking $@"
	@$(CC) $(CFLAGS) $^ -o $@ $(LIBS)

# Collector ingest benchmark needs the wire codec + collector objects.
$(BENCH_COLLECTOR_INGEST_BIN): $(BENCH_COLLECTOR_INGEST_OBJ) $(BASE_OBJS) $(WIRE_OBJS) $(COLLECTOR_LIB_OBJS) $(OBJ_DIR)/collector/main.test.o
	@echo "Linking $@"
	@$(CC) $(CFLAGS) $^ -o $@ $(LIBS)

# Overhead analysis exercises export submit, so it needs wire + export.
$(BENCH_OVERHEAD_BIN): $(BENCH_OVERHEAD_OBJ) $(BASE_OBJS) $(WIRE_OBJS) $(EXPORT_OBJS)
	@echo "Linking $@"
	@$(CC) $(CFLAGS) $^ -o $@ $(LIBS)

# Convenience targets
examples: $(EXAMPLES)

tests: $(TESTS)

collector: $(COLLECTOR_BIN)

benchmarks: $(BENCHMARKS)

run-examples: examples
	@echo "=== Running Example 1: Single Span ==="
	@$(EXAMPLE1_BIN)
	@echo ""
	@echo "=== Running Example 2: Parent-Child ==="
	@$(EXAMPLE2_BIN)
	@echo ""
	@echo "=== Running Example 3: Cross-Process ==="
	@echo "Starting backend..."
	@$(EXAMPLE3_BACKEND_BIN) & echo $$! > $(BUILD_DIR)/backend.pid; sleep 1
	@echo "Starting frontend..."
	@$(EXAMPLE3_FRONTEND_BIN) || true
	@sleep 1
	@kill `cat $(BUILD_DIR)/backend.pid 2>/dev/null` 2>/dev/null || true
	@rm -f $(BUILD_DIR)/backend.pid
	@echo ""
	@echo "=== Running Example 4: Sampling ==="
	@$(EXAMPLE4_BIN)

run-tests: tests
	@echo "=== Phase 1 Tests ==="
	@$(TEST1_BIN)
	@echo ""
	@echo "=== Phase 2 Tests ==="
	@$(TEST2_BIN)
	@echo ""
	@echo "=== Phase 3 Tests ==="
	@$(TEST3_BIN)
	@echo ""
	@echo "=== Phase 4 Tests ==="
	@$(TEST4_BIN)
	@echo ""
	@echo "=== Phase 5 Tests ==="
	@$(TEST5_BIN)
	@echo ""
	@echo "=== Phase 6 Tests ==="
	@$(TEST6_BIN)

# Run benchmarks
run-benchmarks: benchmarks
	@echo "=== Sampling Decision Benchmark ==="
	@$(BENCH_SAMPLING_DECISION_BIN)
	@echo ""
	@echo "=== Trace Creation Benchmark ==="
	@$(BENCH_TRACE_CREATION_BIN)
	@echo ""
	@echo "=== Ring Buffer Throughput Benchmark ==="
	@$(BENCH_RING_BUFFER_BIN)
	@echo ""
	@echo "=== Export Submit Latency Benchmark ==="
	@$(BENCH_EXPORT_BIN)
	@echo ""
	@echo "=== Context Injection Benchmark ==="
	@$(BENCH_CONTEXT_INJECT_BIN)
	@echo ""
	@echo "=== Span Creation Benchmark ==="
	@$(BENCH_SPAN_CREATION_BIN)
	@echo ""
	@echo "=== Collector Ingestion Benchmark ==="
	@$(BENCH_COLLECTOR_INGEST_BIN)
	@echo ""
	@echo "=== Overhead Analysis ==="
	@$(BENCH_OVERHEAD_BIN)

# Run full system demo (Phase 7)
run-full-system: examples collector
	@echo "=== Full System Demo ==="
	@./examples/05-full-system/run.sh

# Run visualization on existing trace JSON files
run-visualization:
	@if [ -d "traces" ] && ls traces/*.json >/dev/null 2>&1; then \
		echo "=== Latency Analysis ==="; \
		python3 scripts/analyze_latency.py traces/; \
		echo ""; \
		echo "=== Generating Waterfall ==="; \
		FIRST=$$(ls traces/*.json | head -1); \
		python3 scripts/visualize_trace.py "$$FIRST" traces/waterfall.png; \
	else \
		echo "No trace JSON files found. Run 'make run-full-system' first."; \
	fi

# Code formatting
format:
ifdef CLANG_FORMAT
	@echo "Formatting source files with clang-format..."
	@$(CLANG_FORMAT) -i $(FORMAT_SRCS)
	@echo "Formatting complete."
else
	@echo "Warning: clang-format not found. Skipping formatting."
	@echo "Install with: apt-get install clang-format"
endif

format-check:
ifdef CLANG_FORMAT
	@echo "Checking code formatting..."
	@$(CLANG_FORMAT) --dry-run --Werror $(FORMAT_SRCS) && echo "All files properly formatted." || (echo "ERROR: Some files need formatting. Run 'make format'"; exit 1)
else
	@echo "Warning: clang-format not found. Skipping format check."
endif

# Valgrind memory check
valgrind: tests examples
	@echo "=== Valgrind: Phase 1 Tests ==="
	@valgrind --leak-check=full --error-exitcode=1 $(TEST1_BIN)
	@echo ""
	@echo "=== Valgrind: Phase 2 Tests ==="
	@valgrind --leak-check=full --error-exitcode=1 $(TEST2_BIN)
	@echo ""
	@echo "=== Valgrind: Example 1 ==="
	@valgrind --leak-check=full --error-exitcode=1 $(EXAMPLE1_BIN)
	@echo ""
	@echo "=== Valgrind: Example 2 ==="
	@valgrind --leak-check=full --error-exitcode=1 $(EXAMPLE2_BIN)

clean:
	@echo "Cleaning build directory..."
	@rm -rf $(BUILD_DIR)
	@echo "Clean complete."

# Help target
help:
	@echo "Dapper-Lite Build System"
	@echo ""
	@echo "Targets:"
	@echo "  all             - Build everything (format, examples, tests, collector)"
	@echo "  examples        - Build all examples"
	@echo "  tests           - Build all tests"
	@echo "  collector       - Build collector daemon"
	@echo "  benchmarks      - Build all benchmarks"
	@echo "  run-examples    - Run all examples"
	@echo "  run-tests       - Run all unit tests"
	@echo "  run-benchmarks  - Run all benchmarks"
	@echo "  run-full-system - Run full system demo (Phase 7)"
	@echo "  run-visualization - Run analysis scripts on trace JSON files"
	@echo "  format          - Format all source files with clang-format"
	@echo "  format-check    - Check if files are properly formatted"
	@echo "  valgrind        - Run valgrind memory checks"
	@echo "  clean           - Remove build directory"
	@echo "  help            - Show this help message"
	@echo ""
	@echo "Build directory structure:"
	@echo "  build/obj/      - Object files"
	@echo "  build/bin/      - Example binaries and benchmarks"
	@echo "  build/test/     - Test binaries"

# Dependencies (automatic header dependency tracking would be better in production)
$(OBJ_DIR)/core/trace.o: src/core/trace.c include/dapper/trace.h include/dapper/types.h include/dapper/span.h
$(OBJ_DIR)/core/span.o: src/core/span.c include/dapper/span.h include/dapper/types.h include/dapper/trace.h
$(OBJ_DIR)/core/clock.o: src/core/clock.c
$(OBJ_DIR)/core/thread_local.o: src/core/thread_local.c include/dapper/span.h include/dapper/types.h
$(OBJ_DIR)/core/context.o: src/core/context.c include/dapper/context.h include/dapper/span.h include/dapper/trace.h include/dapper/types.h

$(EXAMPLE1_OBJ): $(EXAMPLE1_SRC) include/dapper/trace.h include/dapper/span.h include/dapper/types.h
$(EXAMPLE2_OBJ): $(EXAMPLE2_SRC) include/dapper/trace.h include/dapper/span.h include/dapper/types.h
$(EXAMPLE3_FRONTEND_OBJ): $(EXAMPLE3_FRONTEND_SRC) include/dapper/trace.h include/dapper/span.h include/dapper/context.h include/dapper/types.h
$(EXAMPLE3_BACKEND_OBJ): $(EXAMPLE3_BACKEND_SRC) include/dapper/trace.h include/dapper/span.h include/dapper/context.h include/dapper/types.h

$(TEST1_OBJ): $(TEST1_SRC) tests/unit/minunit.h include/dapper/trace.h include/dapper/span.h include/dapper/types.h
$(TEST2_OBJ): $(TEST2_SRC) tests/unit/minunit.h include/dapper/trace.h include/dapper/span.h include/dapper/context.h include/dapper/types.h
$(TEST4_OBJ): $(TEST4_SRC) tests/unit/minunit.h include/dapper/exporter.h include/dapper/trace.h include/dapper/span.h include/dapper/types.h

$(OBJ_DIR)/wire/serialize.o: src/wire/serialize.c include/dapper/wire.h include/dapper/types.h
$(OBJ_DIR)/export/ring_buffer.o: src/export/ring_buffer.c include/dapper/exporter.h include/dapper/wire.h include/dapper/types.h include/dapper/span.h
$(OBJ_DIR)/export/file_sink.o: src/export/file_sink.c include/dapper/exporter.h
$(OBJ_DIR)/export/udp_sink.o: src/export/udp_sink.c include/dapper/exporter.h
$(OBJ_DIR)/export/exporter_thread.o: src/export/exporter_thread.c include/dapper/exporter.h

$(OBJ_DIR)/collector/protocol.o: src/collector/protocol.c include/dapper/collector.h include/dapper/wire.h include/dapper/types.h
$(OBJ_DIR)/collector/receiver.o: src/collector/receiver.c src/collector/internal.h include/dapper/collector.h include/dapper/wire.h include/dapper/types.h
$(OBJ_DIR)/collector/assembler.o: src/collector/assembler.c include/dapper/collector.h include/dapper/wire.h include/dapper/types.h
$(OBJ_DIR)/collector/storage.o: src/collector/storage.c include/dapper/collector.h include/dapper/wire.h include/dapper/types.h
$(OBJ_DIR)/collector/main.o: src/collector/main.c src/collector/internal.h include/dapper/collector.h include/dapper/types.h

$(TEST5_OBJ): $(TEST5_SRC) tests/unit/minunit.h include/dapper/collector.h include/dapper/exporter.h include/dapper/trace.h include/dapper/span.h include/dapper/types.h

$(OBJ_DIR)/analysis/query.o: src/analysis/query.c include/dapper/analysis.h include/dapper/wire.h include/dapper/trace.h include/dapper/types.h
$(OBJ_DIR)/analysis/critical_path.o: src/analysis/critical_path.c include/dapper/analysis.h include/dapper/span.h include/dapper/types.h
$(OBJ_DIR)/analysis/aggregation.o: src/analysis/aggregation.c include/dapper/analysis.h include/dapper/types.h
$(OBJ_DIR)/analysis/export_json.o: src/analysis/export_json.c include/dapper/analysis.h include/dapper/types.h

$(TEST6_OBJ): $(TEST6_SRC) tests/unit/minunit.h include/dapper/analysis.h include/dapper/collector.h include/dapper/exporter.h include/dapper/trace.h include/dapper/span.h include/dapper/types.h

$(EXAMPLE5_FRONTEND_OBJ): $(EXAMPLE5_FRONTEND_SRC) include/dapper/trace.h include/dapper/span.h include/dapper/context.h include/dapper/exporter.h include/dapper/sampler.h include/dapper/analysis.h include/dapper/types.h
$(EXAMPLE5_MIDDLEWARE_OBJ): $(EXAMPLE5_MIDDLEWARE_SRC) include/dapper/trace.h include/dapper/span.h include/dapper/context.h include/dapper/exporter.h include/dapper/types.h
$(EXAMPLE5_DATABASE_OBJ): $(EXAMPLE5_DATABASE_SRC) include/dapper/trace.h include/dapper/span.h include/dapper/context.h include/dapper/exporter.h include/dapper/types.h

$(BENCH_SAMPLING_DECISION_OBJ): $(BENCH_SAMPLING_DECISION_SRC) benchmarks/common.h include/dapper/sampler.h include/dapper/types.h
$(BENCH_TRACE_CREATION_OBJ): $(BENCH_TRACE_CREATION_SRC) benchmarks/common.h include/dapper/trace.h include/dapper/sampler.h include/dapper/types.h
$(BENCH_RING_BUFFER_OBJ): $(BENCH_RING_BUFFER_SRC) benchmarks/common.h include/dapper/exporter.h include/dapper/span.h include/dapper/trace.h include/dapper/types.h
$(BENCH_EXPORT_OBJ): $(BENCH_EXPORT_SRC) benchmarks/common.h include/dapper/exporter.h include/dapper/span.h include/dapper/trace.h include/dapper/types.h
$(BENCH_CONTEXT_INJECT_OBJ): $(BENCH_CONTEXT_INJECT_SRC) benchmarks/common.h include/dapper/context.h include/dapper/span.h include/dapper/trace.h include/dapper/types.h
$(BENCH_SPAN_CREATION_OBJ): $(BENCH_SPAN_CREATION_SRC) benchmarks/common.h include/dapper/span.h include/dapper/trace.h include/dapper/types.h
$(BENCH_COLLECTOR_INGEST_OBJ): $(BENCH_COLLECTOR_INGEST_SRC) benchmarks/common.h include/dapper/collector.h include/dapper/exporter.h include/dapper/span.h include/dapper/trace.h include/dapper/types.h
$(BENCH_OVERHEAD_OBJ): $(BENCH_OVERHEAD_SRC) benchmarks/common.h include/dapper/context.h include/dapper/exporter.h include/dapper/sampler.h include/dapper/span.h include/dapper/trace.h include/dapper/types.h