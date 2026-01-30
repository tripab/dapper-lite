/**
 * thread_local.c - Thread-local current span management
 *
 * Provides thread-local storage for the "current" span, allowing
 * automatic parent span detection without explicit passing.
 */

#define _DEFAULT_SOURCE
#include "dapper/span.h"
#include <pthread.h>
#include <stdlib.h>

/* Thread-local key for current span */
static pthread_key_t tls_current_span_key;
static pthread_once_t tls_key_once = PTHREAD_ONCE_INIT;

/**
 * Initialize the thread-local storage key (called once)
 */
static void tls_init(void) { pthread_key_create(&tls_current_span_key, NULL); }

void span_set_current(span_t *span) {
  pthread_once(&tls_key_once, tls_init);
  pthread_setspecific(tls_current_span_key, span);
}

span_t *span_get_current(void) {
  pthread_once(&tls_key_once, tls_init);
  return (span_t *)pthread_getspecific(tls_current_span_key);
}
