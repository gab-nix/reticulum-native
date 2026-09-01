#include "reticulum/hal.h"

#include <assert.h>
#include <stdint.h>
#include <string.h>

static void *thread_entry(void *context) {
    int *value = context;
    *value = 42;
    return context;
}

int main(void) {
    rns_hal_mutex_t *mutex = NULL;
    rns_hal_thread_t *thread = NULL;
    uint8_t random[32] = {0U};
    uint8_t zeros[32] = {0U};
    uint64_t before = 0U;
    uint64_t after = 0U;
    int value = 0;
    void *result = NULL;

    assert(rns_hal_monotonic_ms(&before) == RNS_OK);
    assert(rns_hal_sleep_ms(1U) == RNS_OK);
    assert(rns_hal_monotonic_ms(&after) == RNS_OK);
    assert(after >= before);
    assert(rns_hal_random_bytes(random, sizeof(random)) == RNS_OK);
    assert(memcmp(random, zeros, sizeof(random)) != 0);

    assert(rns_hal_mutex_create(&mutex) == RNS_OK);
    assert(rns_hal_mutex_lock(mutex) == RNS_OK);
    assert(rns_hal_mutex_unlock(mutex) == RNS_OK);
    rns_hal_mutex_destroy(mutex);

    assert(rns_hal_thread_create(&thread, thread_entry, &value) == RNS_OK);
    assert(rns_hal_thread_join(thread, &result) == RNS_OK);
    assert(result == &value && value == 42);
    rns_hal_thread_destroy(thread);
    return 0;
}

