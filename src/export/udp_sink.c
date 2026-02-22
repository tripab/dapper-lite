/**
 * udp_sink.c - UDP-based span export sink
 *
 * Sends serialized span bytes over UDP to a collector.
 * Each span is sent as a single UDP datagram (no length prefix needed
 * since UDP preserves message boundaries).
 *
 * Non-blocking: sendto() is best-effort. Drops are expected and
 * acceptable per the Dapper design (UDP loss is tolerable).
 */

#include "dapper/exporter.h"
#include <arpa/inet.h>
#include <netdb.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

typedef struct {
  int sockfd;
  struct sockaddr_in dest_addr;
} udp_sink_impl_t;

static int udp_sink_write(sink_t *sink, const uint8_t *data, size_t len) {
  if (!sink || !sink->impl || !data || len == 0) {
    return -1;
  }

  udp_sink_impl_t *impl = (udp_sink_impl_t *)sink->impl;

  ssize_t sent = sendto(impl->sockfd, data, len, 0,
                        (struct sockaddr *)&impl->dest_addr,
                        sizeof(impl->dest_addr));
  if (sent < 0) {
    return -1;
  }

  return 0;
}

static void udp_sink_close(sink_t *sink) {
  if (!sink || !sink->impl) {
    return;
  }

  udp_sink_impl_t *impl = (udp_sink_impl_t *)sink->impl;
  if (impl->sockfd >= 0) {
    close(impl->sockfd);
    impl->sockfd = -1;
  }
}

sink_t *sink_create_udp(const char *host, int port) {
  if (!host || port <= 0 || port > 65535) {
    return NULL;
  }

  /* Resolve hostname */
  struct hostent *he = gethostbyname(host);
  if (!he) {
    return NULL;
  }

  /* Create UDP socket */
  int sockfd = socket(AF_INET, SOCK_DGRAM, 0);
  if (sockfd < 0) {
    return NULL;
  }

  /* Build destination address */
  struct sockaddr_in dest_addr;
  memset(&dest_addr, 0, sizeof(dest_addr));
  dest_addr.sin_family = AF_INET;
  dest_addr.sin_port = htons((uint16_t)port);
  memcpy(&dest_addr.sin_addr, he->h_addr_list[0], (size_t)he->h_length);

  /* Allocate impl */
  udp_sink_impl_t *impl = calloc(1, sizeof(udp_sink_impl_t));
  if (!impl) {
    close(sockfd);
    return NULL;
  }
  impl->sockfd = sockfd;
  impl->dest_addr = dest_addr;

  /* Allocate sink */
  sink_t *sink = calloc(1, sizeof(sink_t));
  if (!sink) {
    close(sockfd);
    free(impl);
    return NULL;
  }

  sink->type = SINK_TYPE_UDP;
  sink->impl = impl;
  sink->write = udp_sink_write;
  sink->close = udp_sink_close;

  return sink;
}
