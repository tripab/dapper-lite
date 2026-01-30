/**
 * clock.c - Monotonic clock abstraction
 */

#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <stdint.h>
#include <time.h>
#include <unistd.h>

/**
 * Get current monotonic timestamp in nanoseconds
 *
 * Uses CLOCK_MONOTONIC which is immune to:
 * - NTP adjustments
 * - Leap seconds
 * - Manual clock changes
 *
 * Returns: Nanoseconds since an arbitrary point (usually boot time)
 */
uint64_t clock_monotonic_ns(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

int usleep(useconds_t usec) {
  struct timespec ts = {.tv_sec = usec / 1000000,
                        .tv_nsec = (usec % 1000000) * 1000};
  while (nanosleep(&ts, &ts) == -1 && errno == EINTR) {
    // Restart if interrupted by a signal
  }
  return 0;
}