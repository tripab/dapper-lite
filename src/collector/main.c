/**
 * main.c - Collector daemon entry point
 *
 * Wires together receiver, assembler (trace_map), and storage.
 * Runs two threads:
 *   1. Receiver thread: UDP recv -> decode -> trace_map_insert
 *   2. Flush loop (main thread or dedicated thread): periodically
 *      flushes completed/timed-out traces to storage
 *
 * Handles SIGINT/SIGTERM for graceful shutdown: stops receiver,
 * flushes all remaining traces, closes storage.
 */

#include "../core/clock.h" /* dapper_sleep_us */
#include "internal.h"
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* ---- Collector struct ---- */

struct collector {
  collector_config_t config;
  trace_map_t *trace_map;
  trace_storage_t *storage;
  receiver_t *receiver;

  pthread_t flush_thread;
  _Atomic bool running;

  /* Stats */
  _Atomic uint64_t traces_completed;
  _Atomic uint64_t traces_timed_out;
  _Atomic uint64_t storage_writes;
  _Atomic uint64_t storage_errors;
};

/* ---- Flush thread ---- */

#define MAX_FLUSH_BATCH 256

static void flush_traces(collector_t *c, int timeout_sec) {
  partial_trace_t *batch[MAX_FLUSH_BATCH];

  /* Batch size comes from config.flush_count, bounded by the local
   * array. Loop until the map has no more flushable traces so the
   * backlog cannot grow without bound between flush intervals. */
  int batch_size = c->config.flush_count;
  if (batch_size <= 0 || batch_size > MAX_FLUSH_BATCH) {
    batch_size = MAX_FLUSH_BATCH;
  }

  int count;
  bool wrote_any = false;
  while ((count = trace_map_flush(c->trace_map, batch, batch_size,
                                  timeout_sec)) > 0) {
    for (int i = 0; i < count; i++) {
      if (batch[i]->has_root) {
        atomic_fetch_add(&c->traces_completed, 1);
      } else {
        atomic_fetch_add(&c->traces_timed_out, 1);
      }

      if (storage_write_trace(c->storage, batch[i]) == 0) {
        atomic_fetch_add(&c->storage_writes, 1);
      } else {
        atomic_fetch_add(&c->storage_errors, 1);
      }

      partial_trace_destroy(batch[i]);
    }
    wrote_any = true;
  }

  if (wrote_any) {
    storage_flush(c->storage);
  }
}

static void *flush_thread_func(void *arg) {
  collector_t *c = (collector_t *)arg;

  while (atomic_load(&c->running)) {
    dapper_sleep_us((uint64_t)c->config.flush_interval_sec * 1000000ULL);
    if (atomic_load(&c->running)) {
      flush_traces(c, c->config.timeout_sec);
    }
  }

  return NULL;
}

/* ---- Public API ---- */

collector_config_t collector_default_config(void) {
  collector_config_t config;
  memset(&config, 0, sizeof(config));
  config.bind_addr = COLLECTOR_DEFAULT_BIND_ADDR;
  config.port = COLLECTOR_DEFAULT_PORT;
  config.allowed_sources = NULL; /* Any source (bind is loopback-only) */
  config.auth_fn = NULL;
  config.auth_user_data = NULL;
  config.timeout_sec = COLLECTOR_DEFAULT_TIMEOUT_SEC;
  config.flush_count = COLLECTOR_DEFAULT_FLUSH_COUNT;
  config.flush_interval_sec = COLLECTOR_DEFAULT_FLUSH_INTERVAL_SEC;
  config.map_buckets = TRACE_MAP_DEFAULT_BUCKETS;
  config.storage_path = "traces.log";
  config.max_active_traces = COLLECTOR_DEFAULT_MAX_ACTIVE_TRACES;
  config.max_spans_per_trace = COLLECTOR_DEFAULT_MAX_SPANS_PER_TRACE;
  config.max_packets_per_sec = COLLECTOR_DEFAULT_MAX_PACKETS_PER_SEC;
  return config;
}

collector_t *collector_create(const collector_config_t *config) {
  if (!config || !config->storage_path) {
    return NULL;
  }

  collector_t *c = calloc(1, sizeof(collector_t));
  if (!c) {
    return NULL;
  }

  c->config = *config;
  atomic_store(&c->running, false);

  /* Create trace map and apply resource caps */
  c->trace_map = trace_map_create(config->map_buckets);
  if (!c->trace_map) {
    free(c);
    return NULL;
  }
  trace_map_set_limits(c->trace_map, config->max_active_traces,
                       config->max_spans_per_trace);

  /* Open storage */
  c->storage = storage_open(config->storage_path);
  if (!c->storage) {
    trace_map_destroy(c->trace_map);
    free(c);
    return NULL;
  }

  /* Create receiver */
  c->receiver = receiver_create(config, c->trace_map);
  if (!c->receiver) {
    storage_close(c->storage);
    trace_map_destroy(c->trace_map);
    free(c);
    return NULL;
  }

  return c;
}

int collector_start(collector_t *c) {
  if (!c) {
    return -1;
  }

  atomic_store(&c->running, true);

  /* Start receiver thread */
  if (receiver_start(c->receiver) < 0) {
    atomic_store(&c->running, false);
    return -1;
  }

  /* Start flush thread */
  if (pthread_create(&c->flush_thread, NULL, flush_thread_func, c) != 0) {
    receiver_stop(c->receiver);
    atomic_store(&c->running, false);
    return -1;
  }

  return 0;
}

void collector_stop(collector_t *c) {
  if (!c || !atomic_load(&c->running)) {
    return;
  }

  atomic_store(&c->running, false);

  /* Stop receiver first (no more incoming spans) */
  receiver_stop(c->receiver);

  /* Stop flush thread */
  pthread_join(c->flush_thread, NULL);

  /* Final flush with timeout=0 to drain everything */
  flush_traces(c, 0);
}

void collector_destroy(collector_t *c) {
  if (!c) {
    return;
  }

  if (atomic_load(&c->running)) {
    collector_stop(c);
  }

  receiver_destroy(c->receiver);
  storage_close(c->storage);
  trace_map_destroy(c->trace_map);
  free(c);
}

int collector_port(const collector_t *c) {
  if (!c) {
    return -1;
  }
  return receiver_port(c->receiver);
}

void collector_get_stats(const collector_t *c, collector_stats_t *stats) {
  if (!c || !stats) {
    return;
  }

  memset(stats, 0, sizeof(collector_stats_t));

  /* Get receiver stats */
  receiver_get_stats(c->receiver, stats);

  /* Get trace map drop stats */
  trace_map_get_drop_stats(c->trace_map, &stats->traces_dropped,
                           &stats->spans_dropped);

  /* Get collector-level stats */
  stats->traces_completed = atomic_load(&c->traces_completed);
  stats->traces_timed_out = atomic_load(&c->traces_timed_out);
  stats->storage_writes = atomic_load(&c->storage_writes);
  stats->storage_errors = atomic_load(&c->storage_errors);
}

/* ============================================================
 * Standalone daemon entry point
 * (guarded so test binaries can link collector functions)
 * ============================================================ */

#ifndef COLLECTOR_NO_MAIN

static volatile sig_atomic_t g_shutdown = 0;

static void signal_handler(int sig) {
  (void)sig;
  g_shutdown = 1;
}

int main(int argc, char *argv[]) {
  collector_config_t config = collector_default_config();

  /* Simple arg parsing:
   *   -b bind_addr  -p port  -t timeout  -s storage_path
   *   -a allowed_sources (comma-separated IPv4 list)
   *   -r max_packets_per_sec (0 = unlimited) */
  for (int i = 1; i < argc - 1; i++) {
    if (strcmp(argv[i], "-b") == 0) {
      config.bind_addr = argv[++i];
    } else if (strcmp(argv[i], "-p") == 0) {
      config.port = atoi(argv[++i]);
    } else if (strcmp(argv[i], "-t") == 0) {
      config.timeout_sec = atoi(argv[++i]);
    } else if (strcmp(argv[i], "-s") == 0) {
      config.storage_path = argv[++i];
    } else if (strcmp(argv[i], "-a") == 0) {
      config.allowed_sources = argv[++i];
    } else if (strcmp(argv[i], "-r") == 0) {
      config.max_packets_per_sec = atoi(argv[++i]);
    }
  }

  printf("dapper-lite collector starting on %s:%d\n", config.bind_addr,
         config.port);
  printf("  timeout: %d sec, storage: %s\n", config.timeout_sec,
         config.storage_path);
  if (config.allowed_sources) {
    printf("  allowed sources: %s\n", config.allowed_sources);
  }
  if (config.max_packets_per_sec > 0) {
    printf("  rate limit: %d packets/sec\n", config.max_packets_per_sec);
  }

  collector_t *c = collector_create(&config);
  if (!c) {
    fprintf(stderr, "error: failed to create collector\n");
    return 1;
  }

  /* Install signal handlers */
  struct sigaction sa;
  memset(&sa, 0, sizeof(sa));
  sa.sa_handler = signal_handler;
  sigaction(SIGINT, &sa, NULL);
  sigaction(SIGTERM, &sa, NULL);

  if (collector_start(c) < 0) {
    fprintf(stderr, "error: failed to start collector\n");
    collector_destroy(c);
    return 1;
  }

  printf("collector running (press Ctrl-C to stop)\n");

  /* Wait for shutdown signal */
  while (!g_shutdown) {
    sleep(1);
  }

  printf("\nshutting down...\n");
  collector_stop(c);

  /* Print final stats */
  collector_stats_t stats;
  collector_get_stats(c, &stats);
  printf("final stats:\n");
  printf("  packets received: %llu\n",
         (unsigned long long)stats.packets_received);
  printf("  packets invalid:  %llu\n",
         (unsigned long long)stats.packets_invalid);
  printf("  packets unauthorized: %llu\n",
         (unsigned long long)stats.packets_unauthorized);
  printf("  packets rate limited: %llu\n",
         (unsigned long long)stats.packets_rate_limited);
  printf("  spans processed:  %llu\n",
         (unsigned long long)stats.spans_processed);
  printf("  spans dropped:    %llu\n", (unsigned long long)stats.spans_dropped);
  printf("  traces dropped:   %llu\n",
         (unsigned long long)stats.traces_dropped);
  printf("  traces completed: %llu\n",
         (unsigned long long)stats.traces_completed);
  printf("  traces timed out: %llu\n",
         (unsigned long long)stats.traces_timed_out);
  printf("  storage writes:   %llu\n",
         (unsigned long long)stats.storage_writes);
  printf("  storage errors:   %llu\n",
         (unsigned long long)stats.storage_errors);

  collector_destroy(c);
  printf("collector stopped.\n");
  return 0;
}

#endif /* COLLECTOR_NO_MAIN */
