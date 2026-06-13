/**
 * receiver.c - UDP socket receiver for the trace collector
 *
 * Binds a UDP socket on a configurable address/port and receives
 * span datagrams in a loop. Each datagram passes source-allowlist,
 * rate-limit, and optional authentication checks before being
 * decoded via collector_decode_span() and inserted into the trace
 * map.
 *
 * The bind address defaults to loopback (COLLECTOR_DEFAULT_BIND_ADDR);
 * exposing the collector on other interfaces is an explicit opt-in.
 *
 * Runs in its own thread; stopped by setting the running flag
 * to false (the recv will unblock via socket timeout).
 */

#include "dapper/wire.h"
#include "internal.h"
#include <arpa/inet.h>
#include <errno.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

/* ---- Internal receiver state ---- */

typedef struct receiver {
  int sockfd;
  int port;
  trace_map_t *trace_map;
  pthread_t thread;
  _Atomic bool running;

  /* Source allowlist (network byte order); 0 entries = allow all */
  in_addr_t allowed[COLLECTOR_MAX_ALLOWED_SOURCES];
  int allowed_count;

  /* Optional packet authentication hook */
  collector_auth_fn auth_fn;
  void *auth_user_data;

  /* Rate limiting (0 = unlimited) */
  int max_packets_per_sec;

  /* Stats (updated atomically) */
  _Atomic uint64_t packets_received;
  _Atomic uint64_t packets_invalid;
  _Atomic uint64_t packets_unauthorized;
  _Atomic uint64_t packets_rate_limited;
  _Atomic uint64_t spans_processed;
} receiver_t;

/* ---- Allowlist parsing ---- */

/**
 * Parse a comma-separated list of IPv4 literals into r->allowed.
 * Returns 0 on success, -1 on malformed input or too many entries.
 */
static int parse_allowed_sources(receiver_t *r, const char *list) {
  r->allowed_count = 0;
  if (!list) {
    return 0; /* No allowlist — accept any source */
  }

  char buf[COLLECTOR_MAX_ALLOWED_SOURCES * 16];
  if (strlen(list) >= sizeof(buf)) {
    return -1;
  }
  strncpy(buf, list, sizeof(buf) - 1);
  buf[sizeof(buf) - 1] = '\0';

  char *saveptr = NULL;
  for (char *tok = strtok_r(buf, ",", &saveptr); tok;
       tok = strtok_r(NULL, ",", &saveptr)) {
    if (r->allowed_count >= COLLECTOR_MAX_ALLOWED_SOURCES) {
      return -1;
    }
    struct in_addr addr;
    if (inet_pton(AF_INET, tok, &addr) != 1) {
      return -1;
    }
    r->allowed[r->allowed_count++] = addr.s_addr;
  }

  /* An allowlist string that parses to zero entries (e.g. ",") is
   * almost certainly a configuration error — reject it rather than
   * silently accepting all sources. */
  return r->allowed_count > 0 ? 0 : -1;
}

static bool source_allowed(const receiver_t *r, in_addr_t source) {
  if (r->allowed_count == 0) {
    return true;
  }
  for (int i = 0; i < r->allowed_count; i++) {
    if (r->allowed[i] == source) {
      return true;
    }
  }
  return false;
}

/* ---- Receiver thread function ---- */

static void *receiver_thread_func(void *arg) {
  receiver_t *r = (receiver_t *)arg;
  uint8_t packet[COLLECTOR_UDP_MAX_PACKET];

  /* Rate limiting window: packets counted in the current second */
  time_t window_sec = 0;
  int window_count = 0;

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

    /* Rate limit */
    if (r->max_packets_per_sec > 0) {
      struct timespec now;
      clock_gettime(CLOCK_MONOTONIC, &now);
      if (now.tv_sec != window_sec) {
        window_sec = now.tv_sec;
        window_count = 0;
      }
      if (++window_count > r->max_packets_per_sec) {
        atomic_fetch_add(&r->packets_rate_limited, 1);
        continue;
      }
    }

    /* Source allowlist */
    if (!source_allowed(r, sender_addr.sin_addr.s_addr)) {
      atomic_fetch_add(&r->packets_unauthorized, 1);
      continue;
    }

    /* Optional packet authentication */
    if (r->auth_fn) {
      char source_ip[INET_ADDRSTRLEN];
      if (!inet_ntop(AF_INET, &sender_addr.sin_addr, source_ip,
                     sizeof(source_ip)) ||
          !r->auth_fn(source_ip, packet, (size_t)n, r->auth_user_data)) {
        atomic_fetch_add(&r->packets_unauthorized, 1);
        continue;
      }
    }

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

receiver_t *receiver_create(const collector_config_t *config,
                            trace_map_t *trace_map) {
  /* port 0 is allowed: the OS assigns an ephemeral port, which the
   * caller can read back via receiver_port(). */
  if (!config || !trace_map || config->port < 0 || config->port > 65535) {
    return NULL;
  }

  /* Resolve bind address (default: loopback) */
  const char *bind_str =
      config->bind_addr ? config->bind_addr : COLLECTOR_DEFAULT_BIND_ADDR;
  struct in_addr bind_ip;
  if (inet_pton(AF_INET, bind_str, &bind_ip) != 1) {
    return NULL;
  }

  receiver_t *r = calloc(1, sizeof(receiver_t));
  if (!r) {
    return NULL;
  }

  if (parse_allowed_sources(r, config->allowed_sources) < 0) {
    free(r);
    return NULL;
  }

  r->auth_fn = config->auth_fn;
  r->auth_user_data = config->auth_user_data;
  r->max_packets_per_sec = config->max_packets_per_sec;

  /* Create UDP socket */
  int sockfd = socket(AF_INET, SOCK_DGRAM, 0);
  if (sockfd < 0) {
    free(r);
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

  /* Bind to the configured address and port */
  struct sockaddr_in bind_addr;
  memset(&bind_addr, 0, sizeof(bind_addr));
  bind_addr.sin_family = AF_INET;
  bind_addr.sin_addr = bind_ip;
  bind_addr.sin_port = htons((uint16_t)config->port);

  if (bind(sockfd, (struct sockaddr *)&bind_addr, sizeof(bind_addr)) < 0) {
    close(sockfd);
    free(r);
    return NULL;
  }

  /* Read back the actual bound port (resolves an ephemeral port 0). */
  int bound_port = config->port;
  struct sockaddr_in actual;
  socklen_t actual_len = sizeof(actual);
  if (getsockname(sockfd, (struct sockaddr *)&actual, &actual_len) == 0) {
    bound_port = ntohs(actual.sin_port);
  }

  r->sockfd = sockfd;
  r->port = bound_port;
  r->trace_map = trace_map;
  atomic_store(&r->running, false);

  return r;
}

int receiver_port(const receiver_t *r) { return r ? r->port : -1; }

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

void receiver_get_stats(const receiver_t *r, collector_stats_t *stats) {
  if (!r || !stats) {
    return;
  }
  stats->packets_received = atomic_load(&r->packets_received);
  stats->packets_invalid = atomic_load(&r->packets_invalid);
  stats->packets_unauthorized = atomic_load(&r->packets_unauthorized);
  stats->packets_rate_limited = atomic_load(&r->packets_rate_limited);
  stats->spans_processed = atomic_load(&r->spans_processed);
}
