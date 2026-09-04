#include "reticulum/hal.h"
#include "reticulum/interface.h"
#include "reticulum/storage.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>

typedef struct fake_platform {
    unsigned allocations;
    unsigned frees;
    unsigned logs;
} fake_platform_t;

typedef struct fake_store {
    uint8_t value[16];
    size_t length;
    unsigned destroys;
} fake_store_t;

typedef struct fake_interface {
    unsigned starts;
    unsigned stops;
    unsigned destroys;
    uint8_t queued[4];
    size_t queued_length;
} fake_interface_t;

static rns_status_t fake_monotonic(void *context, uint64_t *milliseconds) {
    (void)context;
    if (milliseconds == NULL) return RNS_ERROR_INVALID_ARGUMENT;
    *milliseconds = 123U; return RNS_OK;
}
static rns_status_t fake_wallclock(void *context, uint64_t *milliseconds) {
    (void)context;
    if (milliseconds == NULL) return RNS_ERROR_INVALID_ARGUMENT;
    *milliseconds = 456U; return RNS_OK;
}
static rns_status_t fake_random(void *context, void *output, size_t length) {
    (void)context;
    if (output == NULL && length != 0U) return RNS_ERROR_INVALID_ARGUMENT;
    memset(output, 0xa5, length); return RNS_OK;
}
static void fake_zero(void *context, void *memory, size_t length) {
    (void)context; memset(memory, 0, length);
}
static void *fake_allocate(void *context, size_t size) {
    fake_platform_t *platform = context;
    platform->allocations++;
    return malloc(size);
}
static void fake_deallocate(void *context, void *memory) {
    fake_platform_t *platform = context;
    platform->frees++;
    free(memory);
}
static void fake_log(void *context, rns_log_level_t level, const char *message) {
    fake_platform_t *platform = context;
    (void)level; (void)message; platform->logs++;
}

static rns_status_t store_read(void *context, const char *key, uint8_t *output,
                               size_t capacity, size_t *length) {
    fake_store_t *store = context;
    (void)key;
    if (capacity < store->length) { *length = store->length; return RNS_ERROR_OVERFLOW; }
    memcpy(output, store->value, store->length); *length = store->length; return RNS_OK;
}
static rns_status_t store_write(void *context, const char *key,
                                const uint8_t *data, size_t length) {
    fake_store_t *store = context;
    (void)key;
    if (length > sizeof(store->value)) return RNS_ERROR_OVERFLOW;
    memcpy(store->value, data, length); store->length = length; return RNS_OK;
}
static rns_status_t store_remove(void *context, const char *key) {
    fake_store_t *store = context;
    (void)key; store->length = 0U; return RNS_OK;
}
static void store_destroy(void *context) { ((fake_store_t *)context)->destroys++; }

static rns_status_t interface_start(void *context) {
    ((fake_interface_t *)context)->starts++; return RNS_OK;
}
static rns_status_t interface_poll(void *context, rns_interface_receive_fn receive,
                                   void *receive_context, size_t budget) {
    fake_interface_t *interface_value = context;
    if (budget == 0U || interface_value->queued_length == 0U) return RNS_OK;
    return receive(receive_context, interface_value->queued,
                   interface_value->queued_length);
}
static rns_status_t interface_send(void *context, const uint8_t *packet, size_t length) {
    fake_interface_t *interface_value = context;
    if (length > sizeof(interface_value->queued)) return RNS_ERROR_OVERFLOW;
    memcpy(interface_value->queued, packet, length);
    interface_value->queued_length = length; return RNS_OK;
}
static rns_status_t interface_stats(void *context, rns_interface_stats_t *stats) {
    fake_interface_t *interface_value = context;
    stats->effective_mtu = sizeof(interface_value->queued);
    stats->pending_rx = interface_value->queued_length != 0U ? 1U : 0U;
    stats->online = 1; return RNS_OK;
}
static void interface_stop(void *context) { ((fake_interface_t *)context)->stops++; }
static void interface_destroy(void *context) { ((fake_interface_t *)context)->destroys++; }
static rns_status_t receive_packet(void *context, const uint8_t *packet, size_t length) {
    unsigned *received = context;
    assert(length == 3U && packet[0] == 1U); (*received)++; return RNS_OK;
}

int main(void) {
    fake_platform_t platform_state = {0};
    const rns_platform_ops_t platform_ops = {
        &platform_state, fake_monotonic, fake_wallclock, fake_random, fake_zero,
        NULL, fake_allocate, fake_deallocate,
        NULL, NULL, NULL, NULL, NULL, NULL, NULL, fake_log
    };
    const rns_storage_ops_t storage_ops = {
        store_read, store_write, store_remove, store_destroy
    };
    const rns_interface_ops_t interface_ops = {
        interface_start, interface_poll, interface_send, interface_stats,
        interface_stop, interface_destroy
    };
    fake_store_t store_state = {{0}, 0U, 0U};
    fake_interface_t interface_state = {0};
    rns_storage_t *storage = NULL;
    rns_interface_t *interface_value = NULL;
    rns_interface_stats_t stats;
    uint8_t bytes[4] = {1U, 2U, 3U, 0U};
    uint8_t output[4] = {0};
    size_t length = 0U;
    uint64_t time = 0U;
    unsigned received = 0U;

    assert(rns_platform_install(NULL) == RNS_ERROR_INVALID_ARGUMENT);
    assert(rns_platform_install(&platform_ops) == RNS_OK);
    assert(rns_hal_monotonic_ms(&time) == RNS_OK && time == 123U);
    assert(rns_hal_sleep_ms(1U) == RNS_ERROR_UNSUPPORTED);
    rns_hal_log(RNS_LOG_INFO, "test");
    assert(platform_state.logs == 1U);

    assert(rns_storage_create(&storage_ops, &store_state, &storage) == RNS_OK);
    assert(rns_storage_write_atomic(storage, "identity", bytes, 3U) == RNS_OK);
    assert(rns_storage_read(storage, "identity", output, sizeof(output), &length) == RNS_OK);
    assert(length == 3U && memcmp(bytes, output, length) == 0);
    assert(rns_storage_write_atomic(storage, "", bytes, 3U) == RNS_ERROR_INVALID_ARGUMENT);
    assert(rns_storage_remove(storage, "identity") == RNS_OK);
    rns_storage_destroy(storage);
    assert(store_state.destroys == 1U);

    assert(rns_interface_create(&interface_ops, &interface_state, &interface_value) == RNS_OK);
    assert(rns_interface_send(interface_value, bytes, 3U) == RNS_ERROR_INVALID_STATE);
    assert(rns_interface_start(interface_value) == RNS_OK);
    assert(rns_interface_start(interface_value) == RNS_ERROR_INVALID_STATE);
    assert(rns_interface_send(interface_value, bytes, 3U) == RNS_OK);
    assert(rns_interface_poll(interface_value, receive_packet, &received, 1U) == RNS_OK);
    assert(received == 1U);
    assert(rns_interface_get_stats(interface_value, &stats) == RNS_OK);
    assert(stats.effective_mtu == 4U && stats.pending_rx == 1U && stats.online == 1);
    rns_interface_stop(interface_value);
    rns_interface_destroy(interface_value);
    assert(interface_state.starts == 1U && interface_state.stops == 1U &&
           interface_state.destroys == 1U);
    assert(platform_state.allocations == platform_state.frees);
    rns_platform_restore_default();
    assert(rns_hal_monotonic_ms(&time) == RNS_OK);
    return 0;
}
