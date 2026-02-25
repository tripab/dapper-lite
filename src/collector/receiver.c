/**
 * receiver.c - UDP socket receiver for the trace collector
 *
 * Binds a UDP socket on a configurable port and receives span
 * datagrams in a loop. Each datagram is decoded via
 * collector_decode_span() and inserted into the trace map.
 *
 * Runs in its own thread; stopped by setting the running flag
 * to false (the recv will unblock via socket timeout).
 */

#include "dapper/collector.h"
#include "dapper/exporter.h"
#include <arpa/inet.h>
#include <errno.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

/* ---- Internal receiver state ---- */

typedef struct receiver {
  int sockfd;
  int port;
  trace_map_t *trace_map;
  pthread_t thread;
  _Atomic bool running;

  /* Stats (updated atomically) */
  _Atomic uint64_t packets_received;
  _Atomic uint64_t packets_invalid;
  _Atomic uint64_t spans_processed;
} receiver_t;

/* ---- Receiver thread function ---- */

static void *receiver_thread_func(void *arg) {
  receiver_t *r = (receiver_t *)arg;
  uint8_t packet[COLLECTOR_UDP_MAX_PACKET];

  while (atomic_load(&r->running)) {
    struct sockaddr_in sender_addr;
    socklen_t addr_len = sizeof(sender_addr);

    ssize_t n = recvfrom(r->sockfd, packet, sizeof(packet), 0,
                         (struct sockaddr *)&sender_addr, &addr_len);

    if (n < 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        /* Timeout — check running flag and loop */
        continue;
      }
      if (errno == EINTR) {
        continue;
      }
      /* Unexpected error — stop */
      break;
    }

    if (n == 0) {
      continue;
    }

    atomic_fetch_add(&r->packets_received, 1);

    /* Decode the datagram */
    span_t span;
    bool sampled;
    if (collector_decode_span(packet, (size_t)n, &span, &sampled) < 0) {
      atomic_fetch_add(&r->packets_invalid, 1);
      continue;
    }

    /* Insert into trace map */
    if (trace_map_insert(r->trace_map, &span, sampled) == 0) {
      atomic_fetch_add(&r->spans_processed, 1);
    }
  }

  return NULL;
}

/* ---- Public API ---- */

receiver_t *receiver_create(int port, trace_map_t *trace_map) {
  if (port <= 0 || port > 65535 || !trace_map) {
    return NULL;
  }

  /* Create UDP socket */
  int sockfd = socket(AF_INET, SOCK_DGRAM, 0);
  if (sockfd < 0) {
    return NULL;
  }

  /* Allow address reuse */
  int optval = 1;
  setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval));

  /* Set receive timeout so the thread can check the running flag */
  struct timeval tv;
  tv.tv_sec = 0;
  tv.tv_usec = 100000; /* 100ms */
  setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

  /* Bind to port */
  struct sockaddr_in bind_addr;
  memset(&bind_addr, 0, sizeof(bind_addr));
  bind_addr.sin_family = AF_INET;
  bind_addr.sin_addr.s_addr = htonl(INADDR_ANY);
  bind_addr.sin_port = htons((uint16_t)port);

  if (bind(sockfd, (struct sockaddr *)&bind_addr, sizeof(bind_addr)) < 0) {
    close(sockfd);
    return NULL;
  }

  receiver_t *r = calloc(1, sizeof(receiver_t));
  if (!r) {
    close(sockfd);
    return NULL;
  }

  r->sockfd = sockfd;
  r->port = port;
  r->trace_map = trace_map;
  atomic_store(&r->running, false);

  return r;
}

int receiver_start(receiver_t *r) {
  if (!r) {
    return -1;
  }

  atomic_store(&r->running, true);

  if (pthread_create(&r->thread, NULL, receiver_thread_func, r) != 0) {
    atomic_store(&r->running, false);
    return -1;
  }

  return 0;
}

void receiver_stop(receiver_t *r) {
  if (!r) {
    return;
  }

  atomic_store(&r->running, false);
  pthread_join(r->thread, NULL);
}

void receiver_destroy(receiver_t *r) {
  if (!r) {
    return;
  }

  if (atomic_load(&r->running)) {
    receiver_stop(r);
  }

  if (r->sockfd >= 0) {
    close(r->sockfd);
  }

  free(r);
}

void receiver_get_stats(const receiver_t *r, uint64_t *packets_received,
                        uint64_t *packets_invalid, uint64_t *spans_processed) {
  if (!r) {
    return;
  }
  if (packets_received) {
    *packets_received = atomic_load(&r->packets_received);
  }
  if (packets_invalid) {
    *packets_invalid = atomic_load(&r->packets_invalid);
  }
  if (spans_processed) {
    *spans_processed = atomic_load(&r->spans_processed);
  }
}
