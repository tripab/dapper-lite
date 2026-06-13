/**
 * clock.h - Internal monotonic clock and sleep helpers
 *
 * Private to the implementation (not installed). Declares the
 * monotonic timestamp source and a sleep helper so modules don't
 * rely on an ad hoc `extern` or shadow the libc usleep symbol.
 */

#ifndef DAPPER_CORE_CLOCK_H
#define DAPPER_CORE_CLOCK_H

#include <stdint.h>

/** Monotonic timestamp in nanoseconds (immune to wall-clock changes). */
uint64_t clock_monotonic_ns(void);

/** Sleep for the given number of microseconds (restarts on EINTR). */
void dapper_sleep_us(uint64_t usec);

#endif /* DAPPER_CORE_CLOCK_H */
