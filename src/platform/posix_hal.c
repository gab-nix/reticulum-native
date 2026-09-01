#define _POSIX_C_SOURCE 200809L

#include "reticulum/hal.h"

#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <time.h>

#include <openssl/crypto.h>
#include <openssl/rand.h>

struct rns_hal_mutex {
    pthread_mutex_t native;
};

struct rns_hal_thread {
    pthread_t native;
    int joined;
};

static rns_status_t clock_ms(clockid_t clock_id, uint64_t *milliseconds) {
    struct timespec value;
    uint64_t seconds;

    if (milliseconds == NULL) {
        return RNS_ERROR_INVALID_ARGUMENT;
    }
    if (clock_gettime(clock_id, &value) != 0) {
        return RNS_ERROR_IO;
    }
    seconds = (uint64_t)value.tv_sec;
    if (seconds > UINT64_MAX / 1000U) {
        return RNS_ERROR_OVERFLOW;
    }
    *milliseconds = seconds * 1000U + (uint64_t)value.tv_nsec / 1000000U;
    return RNS_OK;
}

rns_status_t rns_hal_monotonic_ms(uint64_t *milliseconds) {
    return clock_ms(CLOCK_MONOTONIC, milliseconds);
}

rns_status_t rns_hal_wallclock_ms(uint64_t *milliseconds) {
    return clock_ms(CLOCK_REALTIME, milliseconds);
}

rns_status_t rns_hal_random_bytes(void *output, size_t length) {
    unsigned char *cursor = output;

    if (output == NULL && length != 0U) {
        return RNS_ERROR_INVALID_ARGUMENT;
    }
    while (length != 0U) {
        int chunk = length > (size_t)INT_MAX ? INT_MAX : (int)length;
        if (RAND_bytes(cursor, chunk) != 1) {
            return RNS_ERROR_CRYPTO;
        }
        cursor += (size_t)chunk;
        length -= (size_t)chunk;
    }
    return RNS_OK;
}

void rns_hal_secure_zero(void *memory, size_t length) {
    if (memory != NULL && length != 0U) {
        OPENSSL_cleanse(memory, length);
    }
}

rns_status_t rns_hal_sleep_ms(uint64_t milliseconds) {
    struct timespec requested;
    struct timespec remaining;

    if (milliseconds / 1000U > (uint64_t)INT64_MAX) {
        return RNS_ERROR_OVERFLOW;
    }
    requested.tv_sec = (time_t)(milliseconds / 1000U);
    requested.tv_nsec = (long)((milliseconds % 1000U) * 1000000U);
    while (nanosleep(&requested, &remaining) != 0) {
        if (errno != EINTR) {
            return RNS_ERROR_IO;
        }
        requested = remaining;
    }
    return RNS_OK;
}

rns_status_t rns_hal_mutex_create(rns_hal_mutex_t **mutex) {
    rns_hal_mutex_t *created;

    if (mutex == NULL) {
        return RNS_ERROR_INVALID_ARGUMENT;
    }
    created = malloc(sizeof(*created));
    if (created == NULL) {
        return RNS_ERROR_NO_MEMORY;
    }
    if (pthread_mutex_init(&created->native, NULL) != 0) {
        free(created);
        return RNS_ERROR_IO;
    }
    *mutex = created;
    return RNS_OK;
}

void rns_hal_mutex_destroy(rns_hal_mutex_t *mutex) {
    if (mutex != NULL) {
        (void)pthread_mutex_destroy(&mutex->native);
        free(mutex);
    }
}

rns_status_t rns_hal_mutex_lock(rns_hal_mutex_t *mutex) {
    if (mutex == NULL) {
        return RNS_ERROR_INVALID_ARGUMENT;
    }
    return pthread_mutex_lock(&mutex->native) == 0 ? RNS_OK : RNS_ERROR_IO;
}

rns_status_t rns_hal_mutex_unlock(rns_hal_mutex_t *mutex) {
    if (mutex == NULL) {
        return RNS_ERROR_INVALID_ARGUMENT;
    }
    return pthread_mutex_unlock(&mutex->native) == 0 ? RNS_OK : RNS_ERROR_IO;
}

rns_status_t rns_hal_thread_create(rns_hal_thread_t **thread,
                                   rns_hal_thread_fn function,
                                   void *context) {
    rns_hal_thread_t *created;

    if (thread == NULL || function == NULL) {
        return RNS_ERROR_INVALID_ARGUMENT;
    }
    created = malloc(sizeof(*created));
    if (created == NULL) {
        return RNS_ERROR_NO_MEMORY;
    }
    created->joined = 0;
    if (pthread_create(&created->native, NULL, function, context) != 0) {
        free(created);
        return RNS_ERROR_IO;
    }
    *thread = created;
    return RNS_OK;
}

rns_status_t rns_hal_thread_join(rns_hal_thread_t *thread, void **result) {
    if (thread == NULL || thread->joined != 0) {
        return RNS_ERROR_INVALID_ARGUMENT;
    }
    if (pthread_join(thread->native, result) != 0) {
        return RNS_ERROR_IO;
    }
    thread->joined = 1;
    return RNS_OK;
}

void rns_hal_thread_destroy(rns_hal_thread_t *thread) {
    if (thread != NULL) {
        if (thread->joined == 0) {
            (void)pthread_detach(thread->native);
        }
        free(thread);
    }
}

