# Quality Targets: Coverage, CRAP, and Mutation Testing

This document captures the repeatable quality loop for Dapper-Lite and
the prioritized targets that the loop should focus on. It complements
the unit suite (`make run-tests`) and the coverage target
(`make coverage`).

## Coverage

```bash
make coverage     # gcov line/branch coverage per source file
```

`scripts/coverage.sh` builds the unit tests with `--coverage`, runs
them, and prints per-file line coverage. If `gcovr` is installed it
also emits `build/coverage/coverage.html`.

Baseline weak spots from the audit's gcov pass (improve these first):

| File | Lines | Branches | Notes |
|------|-------|----------|-------|
| `src/analysis/aggregation.c` | ~65% | ~48% | percentile + group math |
| `src/collector/receiver.c` | ~77% | ~50% | error/timeout/auth paths |
| `src/export/file_sink.c` | ~77% | ~50% | write-failure branches |
| `src/export/udp_sink.c` | ~78% | ~50% | resolver/family handling |
| `src/analysis/query.c` | ~78% | ~66% | corrupt-record branches |
| `src/wire/serialize.c` | ~81% | ~63% | truncation/annotation edges |

The corrupt-input fixtures (B4/D2) and the exact-aggregation test (D3)
already push several of these branches; re-run `make coverage` after
test changes to track movement.

## CRAP-style prioritization

CRAP = complexity × (1 − coverage)². Prioritize functions that are
both complex and partially covered:

- `read_one_trace` (`src/analysis/query.c`) — corrupt vs EOF framing.
- `span_serialize` / `span_deserialize` (`src/wire/serialize.c`) —
  boundary/truncation handling (now split into helpers by F1).
- `aggregate_by_service` (`src/analysis/aggregation.c`) — percentile
  and sample-collection math.
- `receiver_thread_func` (`src/collector/receiver.c`) — rate-limit,
  allowlist, and auth branches.
- `storage_write_trace` (`src/collector/storage.c`) — staged-record
  write and serialize-failure abort.
- `write_json_string` / `export_trace_json` (`src/analysis/export_json.c`)
  — escaping and span/critical-path emission.

## Mutation testing targets

No mutation engine (Mull, etc.) is wired up in this environment, so the
targets below are the manual/scripted mutation focus. Each item lists a
mutation class and the test that should kill it.

| Target | Mutation class | Killed by |
|--------|----------------|-----------|
| Serializer length/offset checks | off-by-one bounds | `test_load_overlarge_span_len`, `test_load_truncated_payload` |
| `num_spans` / `slen` bounds | invert/remove comparison | `test_load_overlarge_num_spans`, `test_load_zero_spans` |
| `consumed == len` packet check | relax equality | (decode tests; extend if added) |
| JSON escaping branches | drop a `case` | `test_json_escapes_special_chars` |
| Percentile / comparator math | swap operands, ± rank | `test_aggregate_exact_stats` |
| Timeout comparison (`elapsed >= timeout`) | flip `>=`/`>` | `test_trace_map_flush_timeout`, `_respects_timeout` |
| Sampled / drop counters | skip increment | `test_exporter_backpressure`, `test_exporter_reflects_sampling` |
| Resource caps (traces/spans) | invert `>=` | `test_trace_map_caps` |
| Source allowlist / auth | accept on reject | `test_collector_allowlist_rejects`, `_auth_hook_rejects` |

### Likely surviving mutations to watch

Off-by-one length checks, removed JSON escaping branches, inverted
corrupt-record conditions, and any assertion that only checks a file is
non-empty. The D3/E4 semantic assertions and the B4/D2 corrupt fixtures
were added specifically to kill these.
