/**
 * byteorder.h - Shared host<->big-endian (network order) helpers
 *
 * Centralizes the 64-bit byte-swap logic that was previously copied
 * into the context, wire, storage, and query modules. Implemented as
 * static inline so there is no extra translation unit to link.
 *
 * 16- and 32-bit conversions use the standard htons/htonl from
 * <arpa/inet.h>; only 64-bit needs a portable helper.
 */

#ifndef DAPPER_BYTEORDER_H
#define DAPPER_BYTEORDER_H

#include <arpa/inet.h>
#include <stdint.h>

/** Host -> big-endian (network order) for a 64-bit value. */
static inline uint64_t dapper_hton64(uint64_t value) {
  static const int probe = 42;
  if (*(const char *)&probe == 42) {
    /* Little-endian host: swap the two 32-bit halves and their bytes. */
    return ((uint64_t)htonl((uint32_t)(value & 0xFFFFFFFFu)) << 32) |
           (uint64_t)htonl((uint32_t)(value >> 32));
  }
  return value; /* Big-endian host: already network order. */
}

/** Big-endian (network order) -> host for a 64-bit value. */
static inline uint64_t dapper_ntoh64(uint64_t value) {
  /* The conversion is symmetric. */
  return dapper_hton64(value);
}

#endif /* DAPPER_BYTEORDER_H */
