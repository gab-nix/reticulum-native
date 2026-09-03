#include "reticulum/destination.h"
#include "reticulum/hal.h"
#include "reticulum/lxmf_router.h"
#include "reticulum/packet.h"
#include "reticulum/runtime.h"
#include "reticulum/udp.h"

#include <assert.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef struct {
    lxmf_router_t *router;
    rns_identity *local_identity;
    bool received;
} receiver_context_t;

typedef struct {
    const rns_identity *identity;
    uint8_t destination[16];
} resolver_context_t;

typedef struct {
    size_t events;
    lxmf_router_event_t last;
} event_context_t;

static uint16_t reserve_udp_port(void) {
    rns_udp_endpoint_t *endpoint = NULL;
    rns_udp_address_t address;
    assert(rns_udp_endpoint_create(&endpoint, RNS_UDP_IPV4) == RNS_OK);
    assert(rns_udp_bind(endpoint, "127.0.0.1", 0U) == RNS_OK);
    assert(rns_udp_local_address(endpoint, &address) == RNS_OK);
    rns_udp_endpoint_destroy(endpoint);
    return address.port;
}

static void configure_udp(rns_config_t *config, const char *name,
                          uint16_t listen_port, uint16_t forward_port) {
    rns_config_init(config);
    config->interface_count = 1U;
    rns_config_interface_t *interface = &config->interfaces[0];
    (void)strcpy(interface->name, name);
    interface->type = RNS_CONFIG_UDP;
    interface->type_set = true;
    interface->enabled = true;
    (void)strcpy(interface->listen_ip, "127.0.0.1");
    (void)strcpy(interface->forward_ip, "127.0.0.1");
    interface->listen_port = listen_port;
    interface->forward_port = forward_port;
}

static const rns_identity *resolve(void *context, const uint8_t destination[16]) {
    resolver_context_t *resolver = context;
    return memcmp(resolver->destination, destination, 16U) == 0
               ? resolver->identity : NULL;
}

static void event_received(void *context, const lxmf_router_event_t *event) {
    event_context_t *events = context;
    events->events++;
    events->last = *event;
}

static void receive_and_prove(rns_runtime_t *runtime, const uint8_t *packet,
                              size_t packet_length,
                              const rns_node_result *result, void *context) {
    receiver_context_t *receiver = context;
    rns_packet decoded;
    assert(rns_packet_decode(&decoded, packet, packet_length));
    if (decoded.packet_type != 0U) return;
    assert(lxmf_router_receive_packet(receiver->router, packet,
                                      packet_length) == LXMF_OK);
    assert(rns_runtime_prove_packet(runtime, result, receiver->local_identity,
                                    true) == RNS_OK);
    receiver->received = true;
}

int main(void) {
    uint16_t alice_port = reserve_udp_port();
    uint16_t bob_port = reserve_udp_port();
    assert(alice_port != bob_port);
    rns_config_t alice_config, bob_config;
    configure_udp(&alice_config, "alice", alice_port, bob_port);
    configure_udp(&bob_config, "bob", bob_port, alice_port);

    rns_identity alice, bob;
    assert(rns_identity_generate(&alice));
    assert(rns_identity_generate(&bob));
    static const char *const aspects[] = {"delivery"};
    uint8_t alice_destination[16], bob_destination[16];
    assert(rns_destination_hash(&alice, "lxmf", aspects, 1U,
                                alice_destination));
    assert(rns_destination_hash(&bob, "lxmf", aspects, 1U, bob_destination));

    char alice_path[] = "/tmp/lxmf-router-receipt-a-XXXXXX";
    char bob_path[] = "/tmp/lxmf-router-receipt-b-XXXXXX";
    int fd = mkstemp(alice_path);
    assert(fd >= 0);
    close(fd);
    unlink(alice_path);
    fd = mkstemp(bob_path);
    assert(fd >= 0);
    close(fd);
    unlink(bob_path);
    lxmf_store_t alice_store = {0}, bob_store = {0};
    assert(lxmf_store_open(&alice_store, alice_path) == LXMF_OK);
    assert(lxmf_store_open(&bob_store, bob_path) == LXMF_OK);

    resolver_context_t alice_resolver = {.identity = &bob};
    resolver_context_t bob_resolver = {.identity = &alice};
    memcpy(alice_resolver.destination, bob_destination, 16U);
    memcpy(bob_resolver.destination, alice_destination, 16U);
    event_context_t alice_events = {0};
    lxmf_router_t alice_router, bob_router;
    receiver_context_t receiver = {
        .router = &bob_router,
        .local_identity = &bob
    };
    rns_runtime_options_t bob_options = {
        .packet_callback = receive_and_prove,
        .callback_context = &receiver
    };
    rns_runtime_t *alice_runtime = NULL, *bob_runtime = NULL;
    assert(rns_runtime_create(&alice_runtime, &alice_config, NULL) == RNS_OK);
    assert(rns_runtime_create(&bob_runtime, &bob_config, &bob_options) == RNS_OK);

    lxmf_router_config_t alice_router_config = {
        .identity = &alice,
        .store = &alice_store,
        .runtime = alice_runtime,
        .resolve_identity = resolve,
        .resolve_context = &alice_resolver,
        .event_callback = event_received,
        .event_context = &alice_events
    };
    lxmf_router_config_t bob_router_config = {
        .identity = &bob,
        .store = &bob_store,
        .runtime = bob_runtime,
        .resolve_identity = resolve,
        .resolve_context = &bob_resolver
    };
    assert(lxmf_router_init(&alice_router, &alice_router_config) == LXMF_OK);
    assert(lxmf_router_init(&bob_router, &bob_router_config) == LXMF_OK);
    assert(rns_runtime_register_destination(bob_runtime, bob_destination) == RNS_OK);
    assert(rns_runtime_announce(bob_runtime, &bob, "lxmf", aspects, 1U,
                                NULL, 0U) == RNS_OK);

    uint64_t start = 0U, now = 0U;
    assert(rns_hal_monotonic_ms(&start) == RNS_OK);
    bool path_known = false;
    do {
        size_t processed = 0U;
        assert(rns_runtime_poll(alice_runtime, 8U, &processed) == RNS_OK);
        path_known = rns_runtime_path_lookup(alice_runtime, bob_destination,
                                             &(rns_path_entry){0}) == RNS_OK;
        assert(rns_hal_monotonic_ms(&now) == RNS_OK);
    } while (!path_known && now - start < 1000U);
    assert(path_known);

    lxmf_store_message_t outbound = {0};
    outbound.message_id[0] = 0x42U;
    memcpy(outbound.destination, bob_destination, 16U);
    memcpy(outbound.source, alice_destination, 16U);
    outbound.timestamp = 42.0;
    outbound.status = LXMF_DELIVERY_QUEUED;
    outbound.signature_state = LXMF_SIGNATURE_VERIFIED;
    outbound.content = (lxmf_slice_t){(const uint8_t *)"receipt", 7U};
    bool inserted = false;
    assert(lxmf_store_put(&alice_store, &outbound, &inserted) == LXMF_OK &&
           inserted);
    assert(lxmf_router_send_message(&alice_router, outbound.message_id) ==
           LXMF_OK);
    uint8_t content[32];
    lxmf_store_message_t stored;
    assert(lxmf_store_read(&alice_store, outbound.message_id, &stored, content,
                           sizeof content) == LXMF_OK);
    assert(stored.status == LXMF_DELIVERY_SENT);

    assert(rns_hal_monotonic_ms(&start) == RNS_OK);
    bool delivered = false;
    do {
        size_t processed = 0U;
        assert(rns_runtime_poll(bob_runtime, 8U, &processed) == RNS_OK);
        assert(rns_runtime_poll(alice_runtime, 8U, &processed) == RNS_OK);
        assert(lxmf_store_read(&alice_store, outbound.message_id, &stored,
                               content, sizeof content) == LXMF_OK);
        delivered = stored.status == LXMF_DELIVERY_DELIVERED;
        assert(rns_hal_monotonic_ms(&now) == RNS_OK);
    } while (!delivered && now - start < 1000U);
    assert(receiver.received && delivered);
    assert(alice_events.last.state == LXMF_DELIVERY_DELIVERED);
    assert(alice_events.last.result == LXMF_OK);

    lxmf_router_destroy(&bob_router);
    lxmf_router_destroy(&alice_router);
    rns_runtime_destroy(bob_runtime);
    rns_runtime_destroy(alice_runtime);
    lxmf_store_close(&bob_store);
    lxmf_store_close(&alice_store);
    unlink(bob_path);
    unlink(alice_path);
    return 0;
}
