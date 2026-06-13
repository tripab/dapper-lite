#!/usr/bin/env bash
#
# coverage.sh - Build the unit tests with gcov instrumentation, run them,
# and report per-file line coverage. Also points at the CRAP / mutation
# priority list in docs/quality-targets.md.
#
# Usage: scripts/coverage.sh
#
# Requires: gcc (with --coverage) and gcov. gcovr is optional and used
# for an HTML report if present.

set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

COV="build/coverage"
rm -rf "$COV"
mkdir -p "$COV"

CC="${CC:-gcc}"
CFLAGS="-std=c11 -O0 -g --coverage -Iinclude -Isrc"
LIBS="-lpthread -lm"

# Library sources (collector main compiled with COLLECTOR_NO_MAIN so it
# can be linked next to a test's own main()). All basenames are unique,
# so object/gcno files are named by basename for easy gcov resolution.
LIB_SRCS=$(find src -name '*.c' ! -path 'src/collector/main.c' | sort)

echo "==> Compiling instrumented library objects"
LIB_OBJS=""
for src in $LIB_SRCS; do
  obj="$COV/$(basename "${src%.c}").o"
  $CC $CFLAGS -c "$src" -o "$obj"
  LIB_OBJS="$LIB_OBJS $obj"
done
$CC $CFLAGS -DCOLLECTOR_NO_MAIN -c src/collector/main.c -o "$COV/main.o"
LIB_OBJS="$LIB_OBJS $COV/main.o"

echo "==> Building and running instrumented test binaries"
for test in tests/unit/test_phase*.c; do
  name="$(basename "$test" .c)"
  $CC $CFLAGS "$test" $LIB_OBJS -o "$COV/$name" $LIBS
  echo "    running $name"
  ( cd "$COV" && "./$name" >/dev/null 2>&1 ) || {
    echo "    WARNING: $name exited non-zero"; }
done

echo "==> Per-file line/branch coverage"
for src in $LIB_SRCS src/collector/main.c; do
  base="$(basename "${src%.c}")"
  out=$(gcov -b -o "$COV" "$src" 2>/dev/null || true)
  lines=$(printf '%s\n' "$out" | awk -F: '/Lines executed/{gsub(/ .*/,"",$2); print $2; exit}')
  branch=$(printf '%s\n' "$out" | awk -F: '/Branches executed/{gsub(/ .*/,"",$2); print $2; exit}')
  printf "  %-22s lines %-8s branches %-8s\n" "$(basename "$src")" \
    "${lines:-n/a}" "${branch:-n/a}"
done
# Move generated .gcov files into the coverage dir.
mv ./*.gcov "$COV"/ 2>/dev/null || true

if command -v gcovr >/dev/null 2>&1; then
  echo "==> gcovr HTML report -> $COV/coverage.html"
  gcovr -r "$ROOT" --html --html-details -o "$COV/coverage.html" 2>/dev/null || true
fi

echo
echo "==> CRAP / mutation priorities: see docs/quality-targets.md"
echo "Done. Raw .gcov files are under $COV/."
