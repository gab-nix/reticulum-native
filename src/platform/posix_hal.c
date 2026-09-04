#define _POSIX_C_SOURCE 200809L

#include "reticulum/hal.h"

#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdlib.h>
#include <time.h>

#include <openssl/crypto.h>
#include <openssl/rand.h>

typedef struct posix_mutex { pthread_mutex_t native; } posix_mutex_t;
typedef struct posix_thread { pthread_t native; int joined; } posix_thread_t;

const rns_platform_ops_t *rns_posix_platform_ops(void);

static rns_status_t clock_ms(clockid_t clock_id, uint64_t *milliseconds) {
    struct timespec value;
    uint64_t seconds;
    if (milliseconds == NULL) return RNS_ERROR_INVALID_ARGUMENT;
    if (clock_gettime(clock_id, &value) != 0) return RNS_ERROR_IO;
    seconds = (uint64_t)value.tv_sec;
    if (seconds > UINT64_MAX / 1000U) return RNS_ERROR_OVERFLOW;
    *milliseconds = seconds * 1000U + (uint64_t)value.tv_nsec / 1000000U;
    return RNS_OK;
}

static rns_status_t posix_monotonic(void *context, uint64_t *milliseconds) {
    (void)context; return clock_ms(CLOCK_MONOTONIC, milliseconds);
}
static rns_status_t posix_wallclock(void *context, uint64_t *milliseconds) {
    (void)context; return clock_ms(CLOCK_REALTIME, milliseconds);
}
static rns_status_t posix_random(void *context, void *output, size_t length) {
    unsigned char *cursor = output;
    (void)context;
    if (output == NULL && length != 0U) return RNS_ERROR_INVALID_ARGUMENT;
    while (length != 0U) {
        int chunk = length > (size_t)INT_MAX ? INT_MAX : (int)length;
        if (RAND_bytes(cursor, chunk) != 1) return RNS_ERROR_CRYPTO;
        cursor += (size_t)chunk; length -= (size_t)chunk;
    }
    return RNS_OK;
}
static void posix_zero(void *context, void *memory, size_t length) {
    (void)context;
    if (memory != NULL && length != 0U) OPENSSL_cleanse(memory, length);
}
static rns_status_t posix_sleep(void *context, uint64_t milliseconds) {
    struct timespec requested, remaining;
    (void)context;
    if (milliseconds / 1000U > (uint64_t)INT64_MAX) return RNS_ERROR_OVERFLOW;
    requested.tv_sec = (time_t)(milliseconds / 1000U);
    requested.tv_nsec = (long)((milliseconds % 1000U) * 1000000U);
    while (nanosleep(&requested, &remaining) != 0) {
        if (errno != EINTR) return RNS_ERROR_IO;
        requested = remaining;
    }
    return RNS_OK;
}
static void *posix_allocate(void *context, size_t size) {
    (void)context; return malloc(size);
}
static void posix_deallocate(void *context, void *memory) {
    (void)context; free(memory);
}
static rns_status_t posix_mutex_create(void *context, void **mutex) {
    posix_mutex_t *created;
    (void)context;
    if (mutex == NULL) return RNS_ERROR_INVALID_ARGUMENT;
    created = malloc(sizeof(*created));
    if (created == NULL) return RNS_ERROR_NO_MEMORY;
    if (pthread_mutex_init(&created->native, NULL) != 0) {
        free(created); return RNS_ERROR_IO;
    }
    *mutex = created; return RNS_OK;
}
static void posix_mutex_destroy(void *context, void *mutex) {
    posix_mutex_t *value = mutex;
    (void)context;
    if (value != NULL) {
        (void)pthread_mutex_destroy(&value->native); free(value);
    }
}
static rns_status_t posix_mutex_lock(void *context, void *mutex) {
    posix_mutex_t *value = mutex;
    (void)context;
    if (value == NULL) return RNS_ERROR_INVALID_ARGUMENT;
    return pthread_mutex_lock(&value->native) == 0 ? RNS_OK : RNS_ERROR_IO;
}
static rns_status_t posix_mutex_unlock(void *context, void *mutex) {
    posix_mutex_t *value = mutex;
    (void)context;
    if (value == NULL) return RNS_ERROR_INVALID_ARGUMENT;
    return pthread_mutex_unlock(&value->native) == 0 ? RNS_OK : RNS_ERROR_IO;
}
static rns_status_t posix_thread_create(void *context, void **thread,
                                        rns_hal_thread_fn function,
                                        void *function_context) {
    posix_thread_t *created;
    (void)context;
    if (thread == NULL || function == NULL) return RNS_ERROR_INVALID_ARGUMENT;
    created = malloc(sizeof(*created));
    if (created == NULL) return RNS_ERROR_NO_MEMORY;
    created->joined = 0;
    if (pthread_create(&created->native, NULL, function, function_context) != 0) {
        free(created); return RNS_ERROR_IO;
    }
    *thread = created; return RNS_OK;
}
static rns_status_t posix_thread_join(void *context, void *thread, void **result) {
    posix_thread_t *value = thread;
    (void)context;
    if (value == NULL || value->joined != 0) return RNS_ERROR_INVALID_ARGUMENT;
    if (pthread_join(value->native, result) != 0) return RNS_ERROR_IO;
    value->joined = 1; return RNS_OK;
}
static void posix_thread_destroy(void *context, void *thread) {
    posix_thread_t *value = thread;
    (void)context;
    if (value != NULL) {
        if (value->joined == 0) (void)pthread_detach(value->native);
        free(value);
    }
}

const rns_platform_ops_t *rns_posix_platform_ops(void) {
    static const rns_platform_ops_t ops = {
        NULL, posix_monotonic, posix_wallclock, posix_random, posix_zero,
        posix_sleep, posix_allocate, posix_deallocate,
        posix_mutex_create, posix_mutex_destroy, posix_mutex_lock,
        posix_mutex_unlock, posix_thread_create, posix_thread_join,
        posix_thread_destroy, NULL
    };
    return &ops;
}
