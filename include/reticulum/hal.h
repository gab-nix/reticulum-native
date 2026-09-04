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

typedef enum rns_log_level {
    RNS_LOG_ERROR = 0,
    RNS_LOG_WARNING,
    RNS_LOG_INFO,
    RNS_LOG_DEBUG
} rns_log_level_t;

/* Install once, before creating Reticulum objects. Callback context and
 * handles remain owned by the provider. Optional operations report
 * RNS_ERROR_UNSUPPORTED through their rns_hal_* wrapper. */
typedef struct rns_platform_ops {
    void *context;
    rns_status_t (*monotonic_ms)(void *context, uint64_t *milliseconds);
    rns_status_t (*wallclock_ms)(void *context, uint64_t *milliseconds);
    rns_status_t (*random_bytes)(void *context, void *output, size_t length);
    void (*secure_zero)(void *context, void *memory, size_t length);
    rns_status_t (*sleep_ms)(void *context, uint64_t milliseconds);
    void *(*allocate)(void *context, size_t size);
    void (*deallocate)(void *context, void *memory);
    rns_status_t (*mutex_create)(void *context, void **mutex);
    void (*mutex_destroy)(void *context, void *mutex);
    rns_status_t (*mutex_lock)(void *context, void *mutex);
    rns_status_t (*mutex_unlock)(void *context, void *mutex);
    rns_status_t (*thread_create)(void *context, void **thread,
                                  rns_hal_thread_fn function,
                                  void *function_context);
    rns_status_t (*thread_join)(void *context, void *thread, void **result);
    void (*thread_destroy)(void *context, void *thread);
    void (*log)(void *context, rns_log_level_t level, const char *message);
} rns_platform_ops_t;

rns_status_t rns_platform_install(const rns_platform_ops_t *ops);
void rns_platform_restore_default(void);
const rns_platform_ops_t *rns_platform_current(void);

rns_status_t rns_hal_monotonic_ms(uint64_t *milliseconds);
rns_status_t rns_hal_wallclock_ms(uint64_t *milliseconds);
rns_status_t rns_hal_random_bytes(void *output, size_t length);
void rns_hal_secure_zero(void *memory, size_t length);
rns_status_t rns_hal_sleep_ms(uint64_t milliseconds);
void *rns_hal_allocate(size_t size);
void rns_hal_deallocate(void *memory);
void rns_hal_log(rns_log_level_t level, const char *message);

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
