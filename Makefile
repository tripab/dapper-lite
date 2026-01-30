# Makefile for Dapper-Lite Phase 1
# Builds core library, examples, and tests

CC = gcc
CFLAGS = -Wall -Wextra -Werror -std=c11 -O2 -g
INCLUDES = -Iinclude
LIBS = -lpthread

# Source files
CORE_SRCS = src/core/trace.c src/core/span.c src/core/clock.c src/core/thread_local.c src/core/context.c
CORE_OBJS = $(CORE_SRCS:.c=.o)

# Targets
EXAMPLES = examples/01-single-span/main examples/02-parent-child/main examples/03-cross-process/frontend examples/03-cross-process/backend
TESTS = tests/unit/test_phase1 tests/unit/test_phase2

.PHONY: all clean examples tests run-examples run-tests

all: examples tests

# Build rules
%.o: %.c
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

# Examples
examples/01-single-span/main: examples/01-single-span/main.o $(CORE_OBJS)
	$(CC) $(CFLAGS) $^ -o $@ $(LIBS)

examples/02-parent-child/main: examples/02-parent-child/main.o $(CORE_OBJS)
	$(CC) $(CFLAGS) $^ -o $@ $(LIBS)

# Phase 2 examples
examples/03-cross-process/frontend: examples/03-cross-process/frontend.o $(CORE_OBJS)
	$(CC) $(CFLAGS) $^ -o $@ $(LIBS)

examples/03-cross-process/backend: examples/03-cross-process/backend.o $(CORE_OBJS)
	$(CC) $(CFLAGS) $^ -o $@ $(LIBS)

# Tests
tests/unit/test_phase1: tests/unit/test_phase1.o $(CORE_OBJS)
	$(CC) $(CFLAGS) $^ -o $@ $(LIBS)

tests/unit/test_phase2: tests/unit/test_phase2.o $(CORE_OBJS)
	$(CC) $(CFLAGS) $^ -o $@ $(LIBS)

# Convenience targets
examples: $(EXAMPLES)

tests: $(TESTS)

run-examples: examples
	@echo "=== Running Example 1 ==="
	@./examples/01-single-span/main
	@echo ""
	@echo "=== Running Example 2 ==="
	@./examples/02-parent-child/main
	@echo ""
	@echo "=== Running Example 3 (Cross-Process) ==="
	@./examples/03-cross-process/run.sh

run-tests: tests
	@echo "=== Phase 1 Tests ==="
	@./tests/unit/test_phase1
	@echo ""
	@echo "=== Phase 2 Tests ==="
	@./tests/unit/test_phase2

# Valgrind memory check
valgrind: tests examples
	@echo "=== Valgrind: Phase 1 Tests ==="
	@valgrind --leak-check=full --error-exitcode=1 ./tests/unit/test_phase1
	@echo ""
	@echo "=== Valgrind: Phase 2 Tests ==="
	@valgrind --leak-check=full --error-exitcode=1 ./tests/unit/test_phase2
	@echo ""
	@echo "=== Valgrind: Example 1 ==="
	@valgrind --leak-check=full --error-exitcode=1 ./examples/01-single-span/main
	@echo ""
	@echo "=== Valgrind: Example 2 ==="
	@valgrind --leak-check=full --error-exitcode=1 ./examples/02-parent-child/main

clean:
	rm -f $(CORE_OBJS)
	rm -f examples/01-single-span/*.o examples/01-single-span/main
	rm -f examples/02-parent-child/*.o examples/02-parent-child/main
	rm -f examples/03-cross-process/*.o examples/03-cross-process/frontend examples/03-cross-process/backend
	rm -f tests/unit/*.o tests/unit/test_phase1 tests/unit/test_phase2

# Dependencies (simplified - real project would use automatic dependency generation)
src/core/trace.o: src/core/trace.c include/dapper/trace.h include/dapper/types.h include/dapper/span.h
src/core/span.o: src/core/span.c include/dapper/span.h include/dapper/types.h include/dapper/trace.h
src/core/clock.o: src/core/clock.c
examples/01-single-span/main.o: examples/01-single-span/main.c include/dapper/trace.h include/dapper/span.h include/dapper/types.h
examples/02-parent-child/main.o: examples/02-parent-child/main.c include/dapper/trace.h include/dapper/span.h include/dapper/types.h
tests/unit/test_phase1.o: tests/unit/test_phase1.c tests/unit/minunit.h include/dapper/trace.h include/dapper/span.h include/dapper/types.h
