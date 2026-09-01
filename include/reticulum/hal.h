#ifndef RETICULUM_HAL_H
#define RETICULUM_HAL_H

#include <stddef.h>
#include <stdint.h>

#include "reticulum/status.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct rns_hal_mutex rns_hal_mutex_t;
typedef struct rns_hal_thread rns_hal_thread_t;
typedef void *(*rns_hal_thread_fn)(void *context);

rns_status_t rns_hal_monotonic_ms(uint64_t *milliseconds);
rns_status_t rns_hal_wallclock_ms(uint64_t *milliseconds);
rns_status_t rns_hal_random_bytes(void *output, size_t length);
void rns_hal_secure_zero(void *memory, size_t length);
rns_status_t rns_hal_sleep_ms(uint64_t milliseconds);

rns_status_t rns_hal_mutex_create(rns_hal_mutex_t **mutex);
void rns_hal_mutex_destroy(rns_hal_mutex_t *mutex);
rns_status_t rns_hal_mutex_lock(rns_hal_mutex_t *mutex);
rns_status_t rns_hal_mutex_unlock(rns_hal_mutex_t *mutex);

rns_status_t rns_hal_thread_create(rns_hal_thread_t **thread,
                                   rns_hal_thread_fn function,
                                   void *context);
rns_status_t rns_hal_thread_join(rns_hal_thread_t *thread, void **result);
void rns_hal_thread_destroy(rns_hal_thread_t *thread);

#ifdef __cplusplus
}
#endif

#endif

