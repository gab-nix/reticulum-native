#include "reticulum/hal.h"

#include <string.h>

const rns_platform_ops_t *rns_posix_platform_ops(void);

struct rns_hal_mutex {
    const rns_platform_ops_t *provider;
    void *native;
};

struct rns_hal_thread {
    const rns_platform_ops_t *provider;
    void *native;
};

static const rns_platform_ops_t *installed_provider = NULL;

static const rns_platform_ops_t *current_provider(void) {
    return installed_provider != NULL ? installed_provider : rns_posix_platform_ops();
}

rns_status_t rns_platform_install(const rns_platform_ops_t *ops) {
    if (ops == NULL || ops->monotonic_ms == NULL || ops->wallclock_ms == NULL ||
        ops->random_bytes == NULL || ops->secure_zero == NULL ||
        ops->allocate == NULL || ops->deallocate == NULL) {
        return RNS_ERROR_INVALID_ARGUMENT;
    }
    installed_provider = ops;
    return RNS_OK;
}

void rns_platform_restore_default(void) { installed_provider = NULL; }

const rns_platform_ops_t *rns_platform_current(void) { return current_provider(); }

rns_status_t rns_hal_monotonic_ms(uint64_t *milliseconds) {
    const rns_platform_ops_t *ops = current_provider();
    return ops->monotonic_ms(ops->context, milliseconds);
}

rns_status_t rns_hal_wallclock_ms(uint64_t *milliseconds) {
    const rns_platform_ops_t *ops = current_provider();
    return ops->wallclock_ms(ops->context, milliseconds);
}

rns_status_t rns_hal_random_bytes(void *output, size_t length) {
    const rns_platform_ops_t *ops = current_provider();
    return ops->random_bytes(ops->context, output, length);
}

void rns_hal_secure_zero(void *memory, size_t length) {
    const rns_platform_ops_t *ops = current_provider();
    ops->secure_zero(ops->context, memory, length);
}

rns_status_t rns_hal_sleep_ms(uint64_t milliseconds) {
    const rns_platform_ops_t *ops = current_provider();
    return ops->sleep_ms != NULL ? ops->sleep_ms(ops->context, milliseconds)
                                 : RNS_ERROR_UNSUPPORTED;
}

void *rns_hal_allocate(size_t size) {
    const rns_platform_ops_t *ops = current_provider();
    return size != 0U ? ops->allocate(ops->context, size) : NULL;
}

void rns_hal_deallocate(void *memory) {
    const rns_platform_ops_t *ops = current_provider();
    if (memory != NULL) ops->deallocate(ops->context, memory);
}

void rns_hal_log(rns_log_level_t level, const char *message) {
    const rns_platform_ops_t *ops = current_provider();
    if (ops->log != NULL && message != NULL) ops->log(ops->context, level, message);
}

rns_status_t rns_hal_mutex_create(rns_hal_mutex_t **mutex) {
    const rns_platform_ops_t *ops = current_provider();
    rns_hal_mutex_t *created;
    rns_status_t status;
    if (mutex == NULL) return RNS_ERROR_INVALID_ARGUMENT;
    *mutex = NULL;
    if (ops->mutex_create == NULL || ops->mutex_destroy == NULL ||
        ops->mutex_lock == NULL || ops->mutex_unlock == NULL) return RNS_ERROR_UNSUPPORTED;
    created = ops->allocate(ops->context, sizeof(*created));
    if (created == NULL) return RNS_ERROR_NO_MEMORY;
    memset(created, 0, sizeof(*created));
    created->provider = ops;
    status = ops->mutex_create(ops->context, &created->native);
    if (status != RNS_OK) { ops->deallocate(ops->context, created); return status; }
    *mutex = created;
    return RNS_OK;
}

void rns_hal_mutex_destroy(rns_hal_mutex_t *mutex) {
    if (mutex == NULL) return;
    mutex->provider->mutex_destroy(mutex->provider->context, mutex->native);
    mutex->provider->deallocate(mutex->provider->context, mutex);
}

rns_status_t rns_hal_mutex_lock(rns_hal_mutex_t *mutex) {
    if (mutex == NULL) return RNS_ERROR_INVALID_ARGUMENT;
    return mutex->provider->mutex_lock(mutex->provider->context, mutex->native);
}

rns_status_t rns_hal_mutex_unlock(rns_hal_mutex_t *mutex) {
    if (mutex == NULL) return RNS_ERROR_INVALID_ARGUMENT;
    return mutex->provider->mutex_unlock(mutex->provider->context, mutex->native);
}

rns_status_t rns_hal_thread_create(rns_hal_thread_t **thread,
                                   rns_hal_thread_fn function, void *context) {
    const rns_platform_ops_t *ops = current_provider();
    rns_hal_thread_t *created;
    rns_status_t status;
    if (thread == NULL || function == NULL) return RNS_ERROR_INVALID_ARGUMENT;
    *thread = NULL;
    if (ops->thread_create == NULL || ops->thread_join == NULL ||
        ops->thread_destroy == NULL) return RNS_ERROR_UNSUPPORTED;
    created = ops->allocate(ops->context, sizeof(*created));
    if (created == NULL) return RNS_ERROR_NO_MEMORY;
    memset(created, 0, sizeof(*created));
    created->provider = ops;
    status = ops->thread_create(ops->context, &created->native, function, context);
    if (status != RNS_OK) { ops->deallocate(ops->context, created); return status; }
    *thread = created;
    return RNS_OK;
}

rns_status_t rns_hal_thread_join(rns_hal_thread_t *thread, void **result) {
    if (thread == NULL) return RNS_ERROR_INVALID_ARGUMENT;
    return thread->provider->thread_join(thread->provider->context, thread->native, result);
}

void rns_hal_thread_destroy(rns_hal_thread_t *thread) {
    if (thread == NULL) return;
    thread->provider->thread_destroy(thread->provider->context, thread->native);
    thread->provider->deallocate(thread->provider->context, thread);
}
