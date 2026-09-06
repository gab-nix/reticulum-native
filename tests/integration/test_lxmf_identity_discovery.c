#include "reticulum/lxmf_router.h"
#include "reticulum/udp.h"
#include "reticulum/hal.h"
#include "reticulum/destination.h"
#include "reticulum/packet.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>

static uint64_t fake_now(void *context) { return *(uint64_t *)context; }
static const rns_identity *unknown(void *context, const uint8_t destination[16]) {
    (void)destination; return *(const rns_identity **)context;
}
static rns_status_t observed(const uint8_t *raw, size_t length,
    const rns_udp_address_t *source, void *context) {
    (void)source;
    rns_packet packet;
    assert(rns_packet_decode(&packet, raw, length));
    assert(packet.transport_type == 0u && packet.destination_type == 2u);
    assert(packet.packet_type == 0u && packet.context == 0u);
    uint8_t expected[16]; const char *aspects[] = {"path", "request"};
    assert(rns_destination_hash(NULL, "rnstransport", aspects, 2u, expected));
    assert(memcmp(packet.destination_hash, expected, 16u) == 0);
    assert(packet.data_length == 32u && packet.data[0] == 0x21u);
    (*(size_t *)context)++;
    return RNS_OK;
}
static void receive_count(rns_udp_endpoint_t *endpoint, size_t expected) {
    size_t count = 0;
    uint64_t start, now;
    assert(rns_hal_monotonic_ms(&start) == RNS_OK);
    do {
        size_t received;
        assert(rns_udp_poll(endpoint, 8u, observed, &count, &received) == RNS_OK);
        assert(rns_hal_monotonic_ms(&now) == RNS_OK);
    } while (count < expected && now - start < 1000u);
    assert(count == expected);
}
int main(void) {
    rns_udp_endpoint_t *peers[2] = {0};
    rns_config_t config; rns_config_init(&config); config.interface_count = 2u;
    for (size_t i = 0; i < 2u; ++i) {
        rns_udp_address_t address;
        assert(rns_udp_endpoint_create(&peers[i], RNS_UDP_IPV4) == RNS_OK);
        assert(rns_udp_bind(peers[i], "127.0.0.1", 0u) == RNS_OK);
        assert(rns_udp_local_address(peers[i], &address) == RNS_OK);
        config.interfaces[i].enabled = true;
        config.interfaces[i].type_set = true;
        config.interfaces[i].type = RNS_CONFIG_UDP;
        strcpy(config.interfaces[i].name, i == 0 ? "first" : "second");
        strcpy(config.interfaces[i].listen_ip, "127.0.0.1");
        strcpy(config.interfaces[i].forward_ip, "127.0.0.1");
        config.interfaces[i].forward_port = address.port;
    }
    rns_runtime_t *runtime = NULL;
    assert(rns_runtime_create(&runtime, &config, NULL) == RNS_OK);
    rns_identity identity; assert(rns_identity_generate(&identity));
    char path[] = "/tmp/lxmf-discovery-XXXXXX";
    int fd = mkstemp(path); assert(fd >= 0 && close(fd) == 0 && unlink(path) == 0);
    lxmf_store_t store = {0}; assert(lxmf_store_open(&store, path) == LXMF_OK);
    lxmf_store_message_t message = {0};
    message.message_id[0] = 1u; message.destination[0] = 0x21u;
    message.status = LXMF_DELIVERY_QUEUED;
    message.delivery.desired_method = LXMF_DELIVERY_METHOD_DIRECT;
    message.timestamp = 1.0;
    bool inserted;
    assert(lxmf_store_put(&store, &message, &inserted) == LXMF_OK && inserted);
    uint64_t now = 1000u;
    uint64_t wall = 100u;
    const rns_identity *known = NULL;
    lxmf_router_config_t options = {.identity = &identity, .store = &store,
        .runtime = runtime, .resolve_identity = unknown,
        .resolve_context = &known,
        .monotonic_clock = fake_now, .monotonic_clock_context = &now,
        .wall_clock = fake_now, .wall_clock_context = &wall,
        .identity_discovery_timeout_seconds = 30u};
    lxmf_router_t router; assert(lxmf_router_init(&router, &options) == LXMF_OK);
    assert(lxmf_router_send_message(&router, message.message_id) == LXMF_ERR_PENDING);
    for (size_t i = 0; i < 2u; ++i) receive_count(peers[i], 1u);
    assert(lxmf_router_send_message(&router, message.message_id) == LXMF_ERR_PENDING);
    for (size_t i = 0; i < 2u; ++i) receive_count(peers[i], 0u);
    now += 15000u;
    assert(lxmf_router_send_message(&router, message.message_id) == LXMF_ERR_PENDING);
    for (size_t i = 0; i < 2u; ++i) receive_count(peers[i], 1u);
    known = &identity;
    now += 1u; /* Identity arrival bypasses the remaining discovery interval. */
    assert(lxmf_router_send_message(&router, message.message_id) == LXMF_ERR_PENDING);
    uint8_t content[1];
    lxmf_store_message_t resumed;
    assert(lxmf_store_read(&store, message.message_id, &resumed, content, sizeof content) == LXMF_OK);
    assert(resumed.delivery.queue_reason == LXMF_QUEUE_REASON_PATH);
    assert(resumed.delivery.identity_deadline == 0u);
    known = NULL;
    message.message_id[0] = 2u;
    assert(lxmf_store_put(&store, &message, &inserted) == LXMF_OK && inserted);
    assert(lxmf_router_send_message(&router, message.message_id) == LXMF_ERR_PENDING);
    assert(lxmf_store_read(&store, message.message_id, &resumed, content, sizeof content) == LXMF_OK);
    assert(resumed.delivery.identity_deadline == 130u);
    lxmf_router_destroy(&router); lxmf_store_close(&store);
    wall = 129u; now = 1u; /* A reboot cannot replenish the discovery budget. */
    assert(lxmf_store_open(&store, path) == LXMF_OK);
    assert(lxmf_router_init(&router, &options) == LXMF_OK);
    assert(lxmf_router_send_message(&router, message.message_id) == LXMF_ERR_PENDING);
    wall = 130u;
    assert(lxmf_router_send_message(&router, message.message_id) == LXMF_ERR_TIMEOUT);
    assert(lxmf_store_read(&store, message.message_id, &resumed, content, sizeof content) == LXMF_OK);
    assert(resumed.status == LXMF_DELIVERY_FAILED);
    assert(resumed.delivery.queue_reason == LXMF_QUEUE_REASON_IDENTITY_TIMEOUT);
    assert(resumed.delivery.attempts == 0u);
    /* A crash between terminal metadata and status writes is repaired, not
     * interpreted as permission to retry. */
    assert(lxmf_store_update_status(&store, message.message_id, LXMF_DELIVERY_QUEUED) == LXMF_OK);
    lxmf_router_destroy(&router); lxmf_store_close(&store);
    assert(lxmf_store_open(&store, path) == LXMF_OK);
    assert(lxmf_router_init(&router, &options) == LXMF_OK);
    assert(lxmf_store_read(&store, message.message_id, &resumed, content, sizeof content) == LXMF_OK);
    assert(resumed.status == LXMF_DELIVERY_FAILED);
    lxmf_router_poll_result_t result;
    assert(lxmf_router_poll(&router, 8u, &result) == LXMF_OK);
    assert(lxmf_store_read(&store, message.message_id, &resumed, content, sizeof content) == LXMF_OK);
    assert(resumed.status == LXMF_DELIVERY_FAILED);
    /* Explicit retry starts a new budget without inventing a transmission. */
    assert(lxmf_router_send_message(&router, message.message_id) == LXMF_ERR_PENDING);
    assert(lxmf_store_read(&store, message.message_id, &resumed, content, sizeof content) == LXMF_OK);
    assert(resumed.delivery.identity_deadline == 160u);
    assert(resumed.delivery.attempts == 0u);
    /* Wall rollback cannot replenish this session's monotonic budget. */
    wall = 120u;
    assert(lxmf_router_send_message(&router, message.message_id) == LXMF_ERR_PENDING);
    assert(lxmf_store_read(&store, message.message_id, &resumed, content, sizeof content) == LXMF_OK);
    assert(resumed.delivery.identity_deadline == 160u);
    now += 30000u;
    assert(lxmf_router_send_message(&router, message.message_id) == LXMF_ERR_TIMEOUT);
    assert(lxmf_router_send_message(&router, message.message_id) == LXMF_ERR_PENDING);
    now += 20000u;
    known = &identity;
    struct stat size_before;
    assert(stat(path, &size_before) == 0);
    int full = open(path, O_WRONLY);
    assert(full >= 0 && ftruncate(full, LXMF_STORE_MAX_FILE_SIZE) == 0);
    assert(lxmf_router_send_message(&router, message.message_id) == LXMF_ERR_CRYPTO);
    assert(ftruncate(full, size_before.st_size) == 0 && close(full) == 0);
    known = NULL;
    wall = 0u;
    now += 10000u;
    assert(lxmf_router_send_message(&router, message.message_id) == LXMF_ERR_TIMEOUT);
    assert(lxmf_store_read(&store, message.message_id, &resumed, content, sizeof content) == LXMF_OK);
    assert(resumed.status == LXMF_DELIVERY_FAILED && resumed.delivery.attempts == 0u);
    assert(lxmf_router_send_message(&router, message.message_id) == LXMF_ERR_PENDING);
    now -= 1u; /* Regressing monotonic source fails closed. */
    assert(lxmf_router_send_message(&router, message.message_id) == LXMF_ERR_TIMEOUT);
    now = UINT64_MAX - 5u;
    assert(lxmf_router_send_message(&router, message.message_id) == LXMF_ERR_PENDING);
    now += 4u; /* Near-maximum clock must not overflow an absolute deadline. */
    assert(lxmf_router_send_message(&router, message.message_id) == LXMF_ERR_PENDING);
    assert(lxmf_router_cancel_message(&router, message.message_id) == LXMF_OK);
    now = 1000u;
    assert(lxmf_router_send_message(&router, message.message_id) == LXMF_ERR_PENDING);
    lxmf_router_destroy(&router); lxmf_store_close(&store);
    /* Restart at a rolled-back wall clock still grants at most one timeout. */
    wall = 1u; now = 1u;
    assert(lxmf_store_open(&store, path) == LXMF_OK);
    assert(lxmf_router_init(&router, &options) == LXMF_OK);
    assert(lxmf_router_send_message(&router, message.message_id) == LXMF_ERR_PENDING);
    now += 30000u;
    assert(lxmf_router_send_message(&router, message.message_id) == LXMF_ERR_TIMEOUT);
    /* Removed records and successfully timed-out messages must not exhaust
     * the bounded guard capacity over a long-lived router session. */
    assert(lxmf_store_remove(&store, message.message_id) == LXMF_OK);
    for (size_t i = 0; i < 2u * (LXMF_STORE_MAX_MESSAGES + 2u); ++i) {
        memset(message.message_id, 0, sizeof message.message_id);
        message.message_id[0] = 0x70u;
        message.message_id[1] = (uint8_t)i;
        message.message_id[2] = (uint8_t)(i >> 8);
        assert(lxmf_store_put(&store, &message, &inserted) == LXMF_OK && inserted);
        assert(lxmf_router_send_message(&router, message.message_id) == LXMF_ERR_PENDING);
        if ((i & 1u) == 0u) {
            now += 30000u;
            assert(lxmf_router_send_message(&router, message.message_id) == LXMF_ERR_TIMEOUT);
        }
        assert(lxmf_store_remove(&store, message.message_id) == LXMF_OK);
    }
    lxmf_router_destroy(&router); lxmf_store_close(&store);
    rns_runtime_destroy(runtime);
    for (size_t i = 0; i < 2u; ++i) rns_udp_endpoint_destroy(peers[i]);
    assert(unlink(path) == 0);
    return 0;
}
