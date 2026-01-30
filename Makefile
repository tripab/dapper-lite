# Makefile for Dapper-Lite
# Builds core library, examples, and tests

CC = gcc
CFLAGS = -Wall -Wextra -Werror -std=c11 -O2 -g
INCLUDES = -Iinclude
LIBS = -lpthread

# Source files
CORE_SRCS = src/core/trace.c src/core/span.c src/core/clock.c
CORE_OBJS = $(CORE_SRCS:.c=.o)

# Targets
EXAMPLES = examples/01-single-span/main examples/02-parent-child/main
TESTS = tests/unit/test_phase1

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

# Tests
tests/unit/test_phase1: tests/unit/test_phase1.o $(CORE_OBJS)
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

run-tests: tests
	@./tests/unit/test_phase1

# Valgrind memory check
valgrind: tests examples
	@echo "=== Valgrind: Test Suite ==="
	@valgrind --leak-check=full --error-exitcode=1 ./tests/unit/test_phase1
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
	rm -f tests/unit/*.o tests/unit/test_phase1

# Dependencies (simplified - real project would use automatic dependency generation)
src/core/trace.o: src/core/trace.c include/dapper/trace.h include/dapper/types.h include/dapper/span.h
src/core/span.o: src/core/span.c include/dapper/span.h include/dapper/types.h include/dapper/trace.h
src/core/clock.o: src/core/clock.c
examples/01-single-span/main.o: examples/01-single-span/main.c include/dapper/trace.h include/dapper/span.h include/dapper/types.h
examples/02-parent-child/main.o: examples/02-parent-child/main.c include/dapper/trace.h include/dapper/span.h include/dapper/types.h
tests/unit/test_phase1.o: tests/unit/test_phase1.c tests/unit/minunit.h include/dapper/trace.h include/dapper/span.h include/dapper/types.h
