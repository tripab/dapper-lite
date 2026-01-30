# Makefile for Dapper-Lite
# Builds core library, examples, and tests with separate build directory

CC = gcc
CFLAGS = -Wall -Wextra -Werror -std=c11 -O2 -g
INCLUDES = -Iinclude
LIBS = -lpthread

# Build directory structure
BUILD_DIR = build
OBJ_DIR = $(BUILD_DIR)/obj
BIN_DIR = $(BUILD_DIR)/bin
TEST_DIR = $(BUILD_DIR)/test

# Source files
CORE_SRCS = src/core/trace.c src/core/span.c src/core/clock.c src/core/thread_local.c src/core/context.c
CORE_OBJS = $(patsubst src/%.c,$(OBJ_DIR)/%.o,$(CORE_SRCS))

# Example sources
EXAMPLE1_SRC = examples/01-single-span/main.c
EXAMPLE2_SRC = examples/02-parent-child/main.c
EXAMPLE3_FRONTEND_SRC = examples/03-cross-process/frontend.c
EXAMPLE3_BACKEND_SRC = examples/03-cross-process/backend.c

EXAMPLE1_OBJ = $(OBJ_DIR)/examples/01-single-span/main.o
EXAMPLE2_OBJ = $(OBJ_DIR)/examples/02-parent-child/main.o
EXAMPLE3_FRONTEND_OBJ = $(OBJ_DIR)/examples/03-cross-process/frontend.o
EXAMPLE3_BACKEND_OBJ = $(OBJ_DIR)/examples/03-cross-process/backend.o

# Test sources
TEST1_SRC = tests/unit/test_phase1.c
TEST2_SRC = tests/unit/test_phase2.c

TEST1_OBJ = $(OBJ_DIR)/tests/unit/test_phase1.o
TEST2_OBJ = $(OBJ_DIR)/tests/unit/test_phase2.o

# Binaries
EXAMPLE1_BIN = $(BIN_DIR)/example1-single-span
EXAMPLE2_BIN = $(BIN_DIR)/example2-parent-child
EXAMPLE3_FRONTEND_BIN = $(BIN_DIR)/example3-frontend
EXAMPLE3_BACKEND_BIN = $(BIN_DIR)/example3-backend

TEST1_BIN = $(TEST_DIR)/test_phase1
TEST2_BIN = $(TEST_DIR)/test_phase2

EXAMPLES = $(EXAMPLE1_BIN) $(EXAMPLE2_BIN) $(EXAMPLE3_FRONTEND_BIN) $(EXAMPLE3_BACKEND_BIN)
TESTS = $(TEST1_BIN) $(TEST2_BIN)

# Source files for formatting
FORMAT_SRCS = $(shell find src include examples tests -name '*.c' -o -name '*.h' 2>/dev/null | grep -v minunit.h)

# Check for clang-format
CLANG_FORMAT := $(shell command -v clang-format 2> /dev/null)

.PHONY: all clean examples tests run-examples run-tests valgrind format format-check dirs help

all: format dirs examples tests

# Create build directories
dirs:
	@mkdir -p $(OBJ_DIR)/core
	@mkdir -p $(OBJ_DIR)/examples/01-single-span
	@mkdir -p $(OBJ_DIR)/examples/02-parent-child
	@mkdir -p $(OBJ_DIR)/examples/03-cross-process
	@mkdir -p $(OBJ_DIR)/tests/unit
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

# Link examples
$(EXAMPLE1_BIN): $(EXAMPLE1_OBJ) $(CORE_OBJS)
	@echo "Linking $@"
	@$(CC) $(CFLAGS) $^ -o $@ $(LIBS)

$(EXAMPLE2_BIN): $(EXAMPLE2_OBJ) $(CORE_OBJS)
	@echo "Linking $@"
	@$(CC) $(CFLAGS) $^ -o $@ $(LIBS)

$(EXAMPLE3_FRONTEND_BIN): $(EXAMPLE3_FRONTEND_OBJ) $(CORE_OBJS)
	@echo "Linking $@"
	@$(CC) $(CFLAGS) $^ -o $@ $(LIBS)

$(EXAMPLE3_BACKEND_BIN): $(EXAMPLE3_BACKEND_OBJ) $(CORE_OBJS)
	@echo "Linking $@"
	@$(CC) $(CFLAGS) $^ -o $@ $(LIBS)

# Link tests
$(TEST1_BIN): $(TEST1_OBJ) $(CORE_OBJS)
	@echo "Linking $@"
	@$(CC) $(CFLAGS) $^ -o $@ $(LIBS)

$(TEST2_BIN): $(TEST2_OBJ) $(CORE_OBJS)
	@echo "Linking $@"
	@$(CC) $(CFLAGS) $^ -o $@ $(LIBS)

# Convenience targets
examples: $(EXAMPLES)

tests: $(TESTS)

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

run-tests: tests
	@echo "=== Phase 1 Tests ==="
	@$(TEST1_BIN)
	@echo ""
	@echo "=== Phase 2 Tests ==="
	@$(TEST2_BIN)

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
	@echo "  all           - Build everything (format, examples, tests)"
	@echo "  examples      - Build all examples"
	@echo "  tests         - Build all tests"
	@echo "  run-examples  - Run all examples"
	@echo "  run-tests     - Run all unit tests"
	@echo "  format        - Format all source files with clang-format"
	@echo "  format-check  - Check if files are properly formatted"
	@echo "  valgrind      - Run valgrind memory checks"
	@echo "  clean         - Remove build directory"
	@echo "  help          - Show this help message"
	@echo ""
	@echo "Build directory structure:"
	@echo "  build/obj/    - Object files"
	@echo "  build/bin/    - Example binaries"
	@echo "  build/test/   - Test binaries"

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