#include "reticulum/hal.h"

#include <stdint.h>
#include <string.h>

#ifdef RNS_PLATFORM_POSIX_DEFAULT
const rns_platform_ops_t *rns_posix_platform_ops(void);
#endif

struct rns_hal_mutex {
    const rns_platform_ops_t *provider;
    void *native;
};

struct rns_hal_thread {
    const rns_platform_ops_t *provider;
    void *native;
};

typedef union rns_hal_allocation_header {
    struct {
        const rns_platform_ops_t *provider;
    } metadata;
    max_align_t alignment;
} rns_hal_allocation_header_t;

static const rns_platform_ops_t *installed_provider = NULL;

static const rns_platform_ops_t *current_provider(void) {
#ifdef RNS_PLATFORM_POSIX_DEFAULT
    return installed_provider != NULL ? installed_provider : rns_posix_platform_ops();
#else
    return installed_provider;
#endif
}

rns_status_t rns_platform_install(const rns_platform_ops_t *ops) {
    if (ops == NULL || ops->monotonic_ms == NULL || ops->wallclock_ms == NULL ||
        ops->random_bytes == NULL || ops->secure_zero == NULL ||
        ops->allocate == NULL || ops->deallocate == NULL) {
        return RNS_ERROR_INVALID_ARGUMENT;
    }
    if (installed_provider != NULL && installed_provider != ops)
        return RNS_ERROR_INVALID_STATE;
    installed_provider = ops;
    return RNS_OK;
}

void rns_platform_restore_default(void) { installed_provider = NULL; }

const rns_platform_ops_t *rns_platform_current(void) { return current_provider(); }

rns_status_t rns_hal_monotonic_ms(uint64_t *milliseconds) {
    const rns_platform_ops_t *ops = current_provider();
    if (milliseconds == NULL) return RNS_ERROR_INVALID_ARGUMENT;
    if (ops == NULL) return RNS_ERROR_INVALID_STATE;
    return ops->monotonic_ms(ops->context, milliseconds);
}

rns_status_t rns_hal_wallclock_ms(uint64_t *milliseconds) {
    const rns_platform_ops_t *ops = current_provider();
    if (milliseconds == NULL) return RNS_ERROR_INVALID_ARGUMENT;
    if (ops == NULL) return RNS_ERROR_INVALID_STATE;
    return ops->wallclock_ms(ops->context, milliseconds);
}

rns_status_t rns_hal_random_bytes(void *output, size_t length) {
    const rns_platform_ops_t *ops = current_provider();
    if (output == NULL && length != 0U) return RNS_ERROR_INVALID_ARGUMENT;
    if (ops == NULL) return RNS_ERROR_INVALID_STATE;
    return ops->random_bytes(ops->context, output, length);
}

void rns_hal_secure_zero(void *memory, size_t length) {
    const rns_platform_ops_t *ops = current_provider();
    if (memory == NULL) return;
    if (ops != NULL) {
        ops->secure_zero(ops->context, memory, length);
    } else {
        volatile uint8_t *cursor = memory;
        while (cursor != NULL && length != 0U) {
            *cursor++ = 0U;
            length--;
        }
    }
}

rns_status_t rns_hal_sleep_ms(uint64_t milliseconds) {
    const rns_platform_ops_t *ops = current_provider();
    if (ops == NULL) return RNS_ERROR_INVALID_STATE;
    return ops->sleep_ms != NULL ? ops->sleep_ms(ops->context, milliseconds)
                                 : RNS_ERROR_UNSUPPORTED;
}

void *rns_hal_allocate(size_t size) {
    const rns_platform_ops_t *ops = current_provider();
    rns_hal_allocation_header_t *allocation;
    if (size == 0U || ops == NULL ||
        size > SIZE_MAX - sizeof(*allocation)) return NULL;
    allocation = ops->allocate(ops->context, sizeof(*allocation) + size);
    if (allocation == NULL) return NULL;
    allocation->metadata.provider = ops;
    return allocation + 1;
}

void rns_hal_deallocate(void *memory) {
    rns_hal_allocation_header_t *allocation;
    const rns_platform_ops_t *provider;
    if (memory == NULL) return;
    allocation = (rns_hal_allocation_header_t *)memory - 1;
    provider = allocation->metadata.provider;
    if (provider != NULL && provider->deallocate != NULL)
        provider->deallocate(provider->context, allocation);
}

void rns_hal_log(rns_log_level_t level, const char *message) {
    const rns_platform_ops_t *ops = current_provider();
    if (ops != NULL && ops->log != NULL && message != NULL)
        ops->log(ops->context, level, message);
}

rns_status_t rns_hal_mutex_create(rns_hal_mutex_t **mutex) {
    const rns_platform_ops_t *ops = current_provider();
    rns_hal_mutex_t *created;
    rns_status_t status;
    if (mutex == NULL) return RNS_ERROR_INVALID_ARGUMENT;
    *mutex = NULL;
    if (ops == NULL) return RNS_ERROR_INVALID_STATE;
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
    if (ops == NULL) return RNS_ERROR_INVALID_STATE;
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
