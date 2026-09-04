#include "reticulum/hal.h"
#include "reticulum/interface.h"
#include "reticulum/storage.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>

static unsigned allocations;
static unsigned frees;

static rns_status_t clock_value(void *context, uint64_t *milliseconds) {
    (void)context;
    *milliseconds = 7U;
    return RNS_OK;
}

static rns_status_t random_value(void *context, void *output, size_t length) {
    (void)context;
    memset(output, 0x5a, length);
    return RNS_OK;
}

static void zero_value(void *context, void *memory, size_t length) {
    (void)context;
    memset(memory, 0, length);
}

static void *allocate_value(void *context, size_t size) {
    (void)context;
    allocations++;
    return malloc(size);
}

static void deallocate_value(void *context, void *memory) {
    (void)context;
    frees++;
    free(memory);
}

static rns_status_t interface_start(void *context) {
    (void)context;
    return RNS_OK;
}

static rns_status_t interface_poll(void *context,
                                   rns_interface_receive_fn receive,
                                   void *receive_context, size_t budget) {
    (void)context;
    (void)receive;
    (void)receive_context;
    (void)budget;
    return RNS_OK;
}

static rns_status_t interface_send(void *context, const uint8_t *packet,
                                   size_t length) {
    (void)context;
    (void)packet;
    (void)length;
    return RNS_OK;
}

static rns_status_t interface_stats(void *context,
                                    rns_interface_stats_t *stats) {
    (void)context;
    memset(stats, 0, sizeof(*stats));
    return RNS_OK;
}

static void interface_stop(void *context) { (void)context; }

static rns_status_t storage_read(void *context, const char *key,
                                 uint8_t *output, size_t capacity,
                                 size_t *length) {
    (void)context;
    (void)key;
    (void)output;
    (void)capacity;
    *length = 0U;
    return RNS_ERROR_NOT_FOUND;
}

static rns_status_t storage_write(void *context, const char *key,
                                  const uint8_t *data, size_t length) {
    (void)context;
    (void)key;
    (void)data;
    (void)length;
    return RNS_OK;
}

static rns_status_t storage_remove(void *context, const char *key) {
    (void)context;
    (void)key;
    return RNS_OK;
}

int main(void) {
    static const rns_platform_ops_t first = {
        NULL, clock_value, clock_value, random_value, zero_value, NULL,
        allocate_value, deallocate_value, NULL, NULL, NULL, NULL, NULL, NULL,
        NULL, NULL
    };
    static const rns_platform_ops_t second = {
        NULL, clock_value, clock_value, random_value, zero_value, NULL,
        allocate_value, deallocate_value, NULL, NULL, NULL, NULL, NULL, NULL,
        NULL, NULL
    };
    static const rns_interface_ops_t interface_ops = {
        interface_start, interface_poll, interface_send, interface_stats,
        interface_stop, NULL
    };
    static const rns_storage_ops_t storage_ops = {
        storage_read, storage_write, storage_remove, NULL
    };
    uint8_t bytes[4] = {1U, 2U, 3U, 4U};
    uint64_t now = 0U;
    rns_hal_mutex_t *mutex = NULL;
    rns_interface_t *interface_value = NULL;
    rns_storage_t *storage = NULL;
    void *memory;

    assert(rns_platform_current() == NULL);
    assert(rns_hal_monotonic_ms(&now) == RNS_ERROR_INVALID_STATE);
    assert(rns_hal_monotonic_ms(NULL) == RNS_ERROR_INVALID_ARGUMENT);
    assert(rns_hal_wallclock_ms(&now) == RNS_ERROR_INVALID_STATE);
    assert(rns_hal_random_bytes(bytes, sizeof(bytes)) == RNS_ERROR_INVALID_STATE);
    assert(rns_hal_random_bytes(NULL, 1U) == RNS_ERROR_INVALID_ARGUMENT);
    assert(rns_hal_sleep_ms(1U) == RNS_ERROR_INVALID_STATE);
    assert(rns_hal_allocate(1U) == NULL);
    assert(rns_hal_mutex_create(&mutex) == RNS_ERROR_INVALID_STATE);
    assert(rns_interface_create(&interface_ops, NULL, &interface_value) ==
           RNS_ERROR_INVALID_STATE);
    assert(rns_storage_create(&storage_ops, NULL, &storage) ==
           RNS_ERROR_INVALID_STATE);
    rns_hal_secure_zero(NULL, 0U);
    rns_hal_secure_zero(NULL, 1U);
    rns_hal_secure_zero(bytes, sizeof(bytes));
    assert(bytes[0] == 0U && bytes[3] == 0U);

    assert(rns_platform_install(&first) == RNS_OK);
    assert(rns_platform_install(&first) == RNS_OK);
    assert(rns_platform_install(&second) == RNS_ERROR_INVALID_STATE);
    assert(rns_hal_monotonic_ms(&now) == RNS_OK && now == 7U);
    memory = rns_hal_allocate(8U);
    assert(memory != NULL && allocations == 1U);
    rns_platform_restore_default();
    assert(rns_platform_current() == NULL);
    rns_hal_deallocate(memory);
    assert(frees == 1U);
    return 0;
}
