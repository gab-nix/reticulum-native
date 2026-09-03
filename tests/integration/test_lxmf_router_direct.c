#include "reticulum/destination.h"
#include "reticulum/hal.h"
#include "reticulum/lxmf_router.h"
#include "reticulum/runtime.h"
#include "reticulum/udp.h"

#include <assert.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>

typedef struct {
    const rns_identity *identity;
    uint8_t destination[LXMF_DESTINATION_LENGTH];
} resolver_t;

typedef struct {
    bool received;
    uint8_t content[64];
    size_t content_length;
} inbox_t;

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

static const rns_identity *resolve(void *context,
                                   const uint8_t destination[16]) {
    resolver_t *resolver = context;
    return memcmp(resolver->destination, destination, 16U) == 0
               ? resolver->identity
               : NULL;
}

static void delivered(void *context, const lxmf_store_message_t *message) {
    inbox_t *inbox = context;
    assert(message->content.len <= sizeof inbox->content);
    memcpy(inbox->content, message->content.data, message->content.len);
    inbox->content_length = message->content.len;
    inbox->received = true;
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
    assert(rns_destination_hash(&bob, "lxmf", aspects, 1U,
                                bob_destination));

    rns_runtime_t *alice_runtime = NULL, *bob_runtime = NULL;
    assert(rns_runtime_create(&alice_runtime, &alice_config, NULL) == RNS_OK);
    assert(rns_runtime_create(&bob_runtime, &bob_config, NULL) == RNS_OK);

    char alice_path[] = "/tmp/lxmf-direct-a-XXXXXX";
    char bob_path[] = "/tmp/lxmf-direct-b-XXXXXX";
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

    resolver_t alice_resolver = {.identity = &bob};
    resolver_t bob_resolver = {.identity = &alice};
    memcpy(alice_resolver.destination, bob_destination, 16U);
    memcpy(bob_resolver.destination, alice_destination, 16U);
    inbox_t alice_inbox = {0}, bob_inbox = {0};
    lxmf_router_t alice_router, bob_router;
    lxmf_router_config_t alice_options = {
        .identity = &alice,
        .store = &alice_store,
        .runtime = alice_runtime,
        .resolve_identity = resolve,
        .resolve_context = &alice_resolver,
        .message_callback = delivered,
        .message_context = &alice_inbox,
        .preferred_delivery_method = LXMF_DELIVERY_METHOD_DIRECT,
        .accept_inbound_links = true};
    lxmf_router_config_t bob_options = {
        .identity = &bob,
        .store = &bob_store,
        .runtime = bob_runtime,
        .resolve_identity = resolve,
        .resolve_context = &bob_resolver,
        .message_callback = delivered,
        .message_context = &bob_inbox,
        .preferred_delivery_method = LXMF_DELIVERY_METHOD_DIRECT,
        .accept_inbound_links = true};
    assert(lxmf_router_init(&alice_router, &alice_options) == LXMF_OK);
    assert(lxmf_router_init(&bob_router, &bob_options) == LXMF_OK);
    assert(rns_runtime_announce(bob_runtime, &bob, "lxmf", aspects, 1U,
                                NULL, 0U) == RNS_OK);

    uint64_t start = 0U, now = 0U;
    assert(rns_hal_monotonic_ms(&start) == RNS_OK);
    bool path_known = false;
    do {
        size_t processed = 0U;
        assert(rns_runtime_poll(alice_runtime, 8U, &processed) == RNS_OK);
        path_known = rns_runtime_path_lookup(
                         alice_runtime, bob_destination,
                         &(rns_path_entry){0}) == RNS_OK;
        assert(rns_hal_monotonic_ms(&now) == RNS_OK);
    } while (!path_known && now - start < 1000U);
    assert(path_known);

    static const uint8_t body[] = "direct over an authenticated link";
    static const uint8_t title[] = "wire-preserved title";
    static const uint8_t empty_fields[] = {0x80U};
    uint8_t original_packed[LXMF_STORE_MAX_PACKED];
    size_t original_packed_length = 0U;
    lxmf_message_t original = {0}, decoded;
    memcpy(original.destination, bob_destination, 16U);
    memcpy(original.source, alice_destination, 16U);
    original.timestamp = 91.0;
    original.title = (lxmf_slice_t){title, sizeof title - 1U};
    original.content = (lxmf_slice_t){body, sizeof body - 1U};
    original.fields_msgpack =
        (lxmf_slice_t){empty_fields, sizeof empty_fields};
    assert(lxmf_pack(&original, lxmf_identity_signer, &alice, original_packed,
                     sizeof original_packed, &original_packed_length) ==
           LXMF_OK);
    assert(lxmf_unpack(original_packed, original_packed_length, NULL, NULL,
                       &decoded) == LXMF_OK);
    lxmf_store_message_t outbound = {0};
    memcpy(outbound.message_id, decoded.message_id, LXMF_MESSAGE_ID_LENGTH);
    memcpy(outbound.destination, bob_destination, 16U);
    memcpy(outbound.source, alice_destination, 16U);
    outbound.timestamp = 91.0;
    outbound.status = LXMF_DELIVERY_QUEUED;
    outbound.signature_state = LXMF_SIGNATURE_VERIFIED;
    outbound.content = (lxmf_slice_t){body, sizeof body - 1U};
    outbound.packed =
        (lxmf_slice_t){original_packed, original_packed_length};
    bool inserted = false;
    assert(lxmf_store_put(&alice_store, &outbound, &inserted) == LXMF_OK &&
           inserted);

    assert(rns_hal_monotonic_ms(&start) == RNS_OK);
    bool confirmed = false;
    do {
        lxmf_router_poll_result_t result;
        size_t processed = 0U;
        assert(lxmf_router_poll(&alice_router, 1U, &result) == LXMF_OK);
        assert(rns_runtime_poll(bob_runtime, 8U, &processed) == RNS_OK);
        assert(rns_runtime_poll(alice_runtime, 8U, &processed) == RNS_OK);
        uint8_t content[64];
        lxmf_store_message_t stored;
        assert(lxmf_store_read(&alice_store, outbound.message_id, &stored,
                               content, sizeof content) == LXMF_OK);
        confirmed = stored.status == LXMF_DELIVERY_DELIVERED;
        assert(rns_hal_monotonic_ms(&now) == RNS_OK);
    } while (!confirmed && now - start < 3000U);
    assert(confirmed && bob_inbox.received);

    /* Delivery confirmation triggers private identification. The responder
     * indexes that same link as a reply backchannel. */
    bool backchannel = false;
    for (size_t attempt = 0U; attempt < 1000U && !backchannel; ++attempt) {
        size_t processed = 0U;
        assert(rns_runtime_poll(bob_runtime, 8U, &processed) == RNS_OK);
        for (size_t i = 0U; i < LXMF_ROUTER_MAX_LINKS; ++i)
            if (bob_router.links[i].used && !bob_router.links[i].inbound &&
                memcmp(bob_router.links[i].destination, alice_destination,
                       LXMF_DESTINATION_LENGTH) == 0)
                backchannel = true;
    }
    assert(backchannel);

    static const uint8_t reply_body[] = "reply over backchannel";
    lxmf_store_message_t reply = {0};
    reply.message_id[0] = 0x51U;
    memcpy(reply.destination, alice_destination, 16U);
    memcpy(reply.source, bob_destination, 16U);
    reply.timestamp = 93.0;
    reply.status = LXMF_DELIVERY_QUEUED;
    reply.signature_state = LXMF_SIGNATURE_VERIFIED;
    reply.content = (lxmf_slice_t){reply_body, sizeof reply_body - 1U};
    inserted = false;
    assert(lxmf_store_put(&bob_store, &reply, &inserted) == LXMF_OK &&
           inserted);
    assert(lxmf_router_send_message(&bob_router, reply.message_id) == LXMF_OK);
    assert(rns_hal_monotonic_ms(&start) == RNS_OK);
    confirmed = false;
    do {
        size_t processed = 0U;
        assert(rns_runtime_poll(alice_runtime, 8U, &processed) == RNS_OK);
        assert(rns_runtime_poll(bob_runtime, 8U, &processed) == RNS_OK);
        uint8_t content[64];
        lxmf_store_message_t stored;
        assert(lxmf_store_read(&bob_store, reply.message_id, &stored, content,
                               sizeof content) == LXMF_OK);
        confirmed = stored.status == LXMF_DELIVERY_DELIVERED;
        assert(rns_hal_monotonic_ms(&now) == RNS_OK);
    } while (!confirmed && now - start < 1000U);
    assert(confirmed && alice_inbox.received);
    assert(alice_inbox.content_length == sizeof reply_body - 1U);
    assert(memcmp(alice_inbox.content, reply_body,
                  sizeof reply_body - 1U) == 0);
    assert(bob_inbox.content_length == sizeof body - 1U);
    assert(memcmp(bob_inbox.content, body, sizeof body - 1U) == 0);
    {
        uint8_t received_content[64];
        lxmf_store_message_t received;
        assert(lxmf_store_read(&bob_store, decoded.message_id, &received,
                               received_content,
                               sizeof received_content) == LXMF_OK);
    }

    rns_runtime_link_t *established = NULL;
    for (size_t i = 0U; i < LXMF_ROUTER_MAX_LINKS; ++i)
        if (alice_router.links[i].used && !alice_router.links[i].inbound)
            established = alice_router.links[i].link;
    assert(established != NULL &&
           rns_runtime_link_state(established) == RNS_LINK_ACTIVE);

    /* A second message uses the already authenticated link rather than
     * opening another handshake. */
    outbound.message_id[0] = 0x92U;
    outbound.timestamp = 92.0;
    outbound.packed = (lxmf_slice_t){0};
    inserted = false;
    bob_inbox.received = false;
    assert(lxmf_store_put(&alice_store, &outbound, &inserted) == LXMF_OK &&
           inserted);
    assert(lxmf_router_send_message(&alice_router, outbound.message_id) ==
           LXMF_OK);
    assert(alice_router.links[0].link == established);
    assert(rns_hal_monotonic_ms(&start) == RNS_OK);
    confirmed = false;
    do {
        size_t processed = 0U;
        assert(rns_runtime_poll(bob_runtime, 8U, &processed) == RNS_OK);
        assert(rns_runtime_poll(alice_runtime, 8U, &processed) == RNS_OK);
        uint8_t content[64];
        lxmf_store_message_t stored;
        assert(lxmf_store_read(&alice_store, outbound.message_id, &stored,
                               content, sizeof content) == LXMF_OK);
        confirmed = stored.status == LXMF_DELIVERY_DELIVERED;
        assert(rns_hal_monotonic_ms(&now) == RNS_OK);
    } while (!confirmed && now - start < 1000U);
    assert(confirmed && bob_inbox.received);

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
