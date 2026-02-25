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

#include "dapper/collector.h"
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* ---- Forward declarations for receiver (internal module) ---- */

typedef struct receiver receiver_t;
receiver_t *receiver_create(int port, trace_map_t *trace_map);
int receiver_start(receiver_t *r);
void receiver_stop(receiver_t *r);
void receiver_destroy(receiver_t *r);
void receiver_get_stats(const receiver_t *r, uint64_t *packets_received,
                        uint64_t *packets_invalid, uint64_t *spans_processed);

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
  int count =
      trace_map_flush(c->trace_map, batch, MAX_FLUSH_BATCH, timeout_sec);

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

  if (count > 0) {
    storage_flush(c->storage);
  }
}

static void *flush_thread_func(void *arg) {
  collector_t *c = (collector_t *)arg;

  while (atomic_load(&c->running)) {
    usleep((useconds_t)(c->config.flush_interval_sec * 1000000));
    if (atomic_load(&c->running)) {
      flush_traces(c, c->config.timeout_sec);
    }
  }

  return NULL;
}

/* ---- Public API ---- */

collector_config_t collector_default_config(void) {
  collector_config_t config;
  config.port = COLLECTOR_DEFAULT_PORT;
  config.timeout_sec = COLLECTOR_DEFAULT_TIMEOUT_SEC;
  config.flush_count = COLLECTOR_DEFAULT_FLUSH_COUNT;
  config.flush_interval_sec = COLLECTOR_DEFAULT_FLUSH_INTERVAL_SEC;
  config.map_buckets = TRACE_MAP_DEFAULT_BUCKETS;
  config.storage_path = "traces.log";
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

  /* Create trace map */
  c->trace_map = trace_map_create(config->map_buckets);
  if (!c->trace_map) {
    free(c);
    return NULL;
  }

  /* Open storage */
  c->storage = storage_open(config->storage_path);
  if (!c->storage) {
    trace_map_destroy(c->trace_map);
    free(c);
    return NULL;
  }

  /* Create receiver */
  c->receiver = receiver_create(config->port, c->trace_map);
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

void collector_get_stats(const collector_t *c, collector_stats_t *stats) {
  if (!c || !stats) {
    return;
  }

  memset(stats, 0, sizeof(collector_stats_t));

  /* Get receiver stats */
  receiver_get_stats(c->receiver, &stats->packets_received,
                     &stats->packets_invalid, &stats->spans_processed);

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

  /* Simple arg parsing: -p port -t timeout -s storage_path */
  for (int i = 1; i < argc - 1; i++) {
    if (strcmp(argv[i], "-p") == 0) {
      config.port = atoi(argv[++i]);
    } else if (strcmp(argv[i], "-t") == 0) {
      config.timeout_sec = atoi(argv[++i]);
    } else if (strcmp(argv[i], "-s") == 0) {
      config.storage_path = argv[++i];
    }
  }

  printf("dapper-lite collector starting on port %d\n", config.port);
  printf("  timeout: %d sec, storage: %s\n", config.timeout_sec,
         config.storage_path);

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
  printf("  spans processed:  %llu\n",
         (unsigned long long)stats.spans_processed);
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
