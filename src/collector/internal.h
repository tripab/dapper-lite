/**
 * internal.h - Private collector module interfaces
 *
 * Shared between the collector daemon (main.c) and its internal
 * modules (receiver.c). Not installed as a public header.
 */

#ifndef DAPPER_COLLECTOR_INTERNAL_H
#define DAPPER_COLLECTOR_INTERNAL_H

#include "dapper/collector.h"

/* ---- UDP receiver (receiver.c) ---- */

typedef struct receiver receiver_t;

/**
 * Create a UDP receiver bound to config->bind_addr:config->port.
 * Decoded spans are inserted into trace_map.
 *
 * Returns NULL on invalid arguments, invalid bind address, or
 * socket/bind failure.
 */
receiver_t *receiver_create(const collector_config_t *config,
                            trace_map_t *trace_map);

int receiver_start(receiver_t *r);
void receiver_stop(receiver_t *r);
void receiver_destroy(receiver_t *r);

/** Return the actual bound UDP port (resolves an ephemeral port 0). */
int receiver_port(const receiver_t *r);

/**
 * Fill the packet-level fields of stats (packets_received,
 * packets_invalid, packets_unauthorized, packets_rate_limited,
 * spans_processed). Other fields are left untouched.
 */
void receiver_get_stats(const receiver_t *r, collector_stats_t *stats);

#endif /* DAPPER_COLLECTOR_INTERNAL_H */
