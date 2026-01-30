/**
 * context.c - Trace context serialization/deserialization
 */

#include "dapper/context.h"
#include "dapper/span.h"
#include "dapper/trace.h"
#include <string.h>
#include <arpa/inet.h> /* For htobe64/be64toh */

/**
 * Convert 64-bit value to network byte order (big-endian)
 */
static uint64_t htobe64(uint64_t value)
{
    /* Check if system is already big-endian */
    static const int num = 42;
    if (*(char *)&num == 42)
    {
        /* Little-endian system, need to swap */
        return ((uint64_t)htonl(value & 0xFFFFFFFF) << 32) | htonl(value >> 32);
    }
    /* Big-endian system, no swap needed */
    return value;
}

/**
 * Convert 64-bit value from network byte order to host byte order
 */
static uint64_t be64toh(uint64_t value)
{
    /* Same logic as htobe64 (conversion is symmetric) */
    static const int num = 42;
    if (*(char *)&num == 42)
    {
        return ((uint64_t)ntohl(value & 0xFFFFFFFF) << 32) | ntohl(value >> 32);
    }
    return value;
}

int context_inject(const span_t *span, uint8_t *buffer, size_t bufsize)
{
    if (!span || !buffer || bufsize < TRACE_CONTEXT_WIRE_SIZE)
    {
        return -1;
    }

    /* Serialize in network byte order (big-endian) */
    uint64_t trace_id_net = htobe64(span->trace_id);
    uint64_t span_id_net = htobe64(span->span_id);

    memcpy(buffer, &trace_id_net, sizeof(uint64_t));
    memcpy(buffer + 8, &span_id_net, sizeof(uint64_t));

    return TRACE_CONTEXT_WIRE_SIZE;
}

int context_extract(trace_context_t *ctx, const uint8_t *buffer, size_t bufsize)
{
    if (!ctx || !buffer || bufsize < TRACE_CONTEXT_WIRE_SIZE)
    {
        return -1;
    }

    /* Deserialize from network byte order */
    uint64_t trace_id_net, span_id_net;

    memcpy(&trace_id_net, buffer, sizeof(uint64_t));
    memcpy(&span_id_net, buffer + 8, sizeof(uint64_t));

    ctx->trace_id = be64toh(trace_id_net);
    ctx->span_id = be64toh(span_id_net);

    return 0;
}

span_t *span_create_from_context(trace_t *trace, const trace_context_t *ctx,
                                 const char *name)
{
    if (!trace || !ctx || !name)
    {
        return NULL;
    }

    /* Update trace with the extracted trace_id */
    trace->id = ctx->trace_id;

    /* Create span as root (no in-process parent) */
    span_t *span = span_create(trace, NULL, name);
    if (!span)
    {
        return NULL;
    }

    /* Set parent_span_id to the remote parent */
    span->parent_span_id = ctx->span_id;

    return span;
}
