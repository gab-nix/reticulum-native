#define _POSIX_C_SOURCE 200809L

#include "reticulum/destination.h"
#include "reticulum/hal.h"
#include "reticulum/lxmf_router.h"
#include "reticulum/runtime.h"
#include "reticulum/udp.h"

#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define RESOURCE_CONTENT_SIZE 2048u
#define RESOURCE_PACKED_SIZE (RESOURCE_CONTENT_SIZE + 256u)
#define SEGMENTED_FIELD_PAYLOAD_SIZE \
    (RNS_RESOURCE_SINGLE_SEGMENT_MAX_SIZE + 4096u)

typedef struct {
    const rns_identity *identity;
    uint8_t destination[LXMF_DESTINATION_LENGTH];
} resolver_t;

typedef struct {
    bool received;
    bool blocked;
    uint8_t content[LXMF_STORE_MAX_CONTENT];
    size_t content_length;
} inbox_t;

static bool source_is_blocked(void *context, const uint8_t source[16]) {
    (void)source;
    return ((inbox_t *)context)->blocked;
}

typedef struct {
    lxmf_delivery_status_t statuses[16];
    lxmf_status_t results[16];
    size_t count;
    bool saw_resource_progress;
} delivery_observation_t;

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

static void delivery_changed(void *context, const uint8_t message_id[32],
                             lxmf_delivery_status_t status,
                             lxmf_status_t result) {
    delivery_observation_t *observation = context;
    assert(message_id != NULL);
    if (observation->count < 16U) {
        observation->statuses[observation->count] = status;
        observation->results[observation->count] = result;
        observation->count++;
    }
}

static void delivery_event(void *context,
                           const lxmf_router_event_t *event) {
    delivery_observation_t *observation = context;
    assert(event != NULL);
    if (event->state == LXMF_DELIVERY_SENDING &&
        event->queue_reason == LXMF_QUEUE_REASON_RESOURCE)
        observation->saw_resource_progress = true;
}

static size_t queue_large_message(
    lxmf_store_t *store, const rns_identity *signer,
    const uint8_t destination[16], const uint8_t source[16], double timestamp,
    uint8_t seed, uint8_t content[RESOURCE_CONTENT_SIZE],
    uint8_t packed[RESOURCE_PACKED_SIZE],
    uint8_t message_id[32]) {
    for (size_t i = 0U; i < RESOURCE_CONTENT_SIZE; ++i)
        content[i] = (uint8_t)(seed + i * 73U + i / 7U);
    /* Unknown extension fields remain in the exact durable representation. */
    static const uint8_t fields[] = {
        0x81U, 0xcdU, 0x04U, 0xd2U, 0xc4U, 0x03U, 0U, 0xffU, 0x42U};
    lxmf_message_t message = {0}, decoded;
    memcpy(message.destination, destination, 16U);
    memcpy(message.source, source, 16U);
    message.timestamp = timestamp;
    message.title = (lxmf_slice_t){(const uint8_t *)"Resource title", 14u};
    message.content = (lxmf_slice_t){content, RESOURCE_CONTENT_SIZE};
    message.fields_msgpack = (lxmf_slice_t){fields, sizeof fields};
    size_t packed_length = 0U;
    assert(lxmf_pack(&message, lxmf_identity_signer, (void *)signer, packed,
                     RESOURCE_PACKED_SIZE, &packed_length) == LXMF_OK);
    assert(packed_length > RESOURCE_CONTENT_SIZE &&
           packed_length <= RESOURCE_PACKED_SIZE);
    assert(lxmf_unpack(packed, packed_length, NULL, NULL, &decoded) == LXMF_OK);
    message.has_stamp = true;
    message.stamp_len = LXMF_POW_STAMP_LENGTH;
    assert(lxmf_pow_stamp_generate(decoded.message_id, 1u, NULL, NULL,
                                   message.stamp, NULL, NULL) == LXMF_OK);
    assert(lxmf_pack(&message, lxmf_identity_signer, (void *)signer, packed,
                     RESOURCE_PACKED_SIZE, &packed_length) == LXMF_OK);
    memcpy(message_id, decoded.message_id, 32U);
    lxmf_store_message_t stored = {0};
    memcpy(stored.message_id, message_id, 32U);
    memcpy(stored.destination, destination, 16U);
    memcpy(stored.source, source, 16U);
    stored.timestamp = timestamp;
    stored.status = LXMF_DELIVERY_QUEUED;
    stored.signature_state = LXMF_SIGNATURE_VERIFIED;
    stored.content = (lxmf_slice_t){content, RESOURCE_CONTENT_SIZE};
    stored.packed = (lxmf_slice_t){packed, packed_length};
    bool inserted = false;
    assert(lxmf_store_put(store, &stored, &inserted) == LXMF_OK && inserted);
    return packed_length;
}

static size_t queue_segmented_message(
    lxmf_store_t *store, const rns_identity *signer,
    const uint8_t destination[16], const uint8_t source[16],
    uint8_t **packed_out, uint8_t message_id[32]) {
    static const uint8_t content[] = "segmented direct message";
    const size_t fields_length = SEGMENTED_FIELD_PAYLOAD_SIZE + 9u;
    uint8_t *fields = malloc(fields_length);
    uint8_t *packed = malloc(fields_length + 256u);
    assert(fields != NULL && packed != NULL);
    const uint8_t prefix[9] = {
        0x81u, 0xcdu, 0x04u, 0xd2u, 0xc6u,
        (uint8_t)(SEGMENTED_FIELD_PAYLOAD_SIZE >> 24u),
        (uint8_t)(SEGMENTED_FIELD_PAYLOAD_SIZE >> 16u),
        (uint8_t)(SEGMENTED_FIELD_PAYLOAD_SIZE >> 8u),
        (uint8_t)SEGMENTED_FIELD_PAYLOAD_SIZE};
    memcpy(fields, prefix, sizeof prefix);
    uint32_t random = 0x6d2b79f5u;
    for (size_t i = sizeof prefix; i < fields_length; ++i) {
        random ^= random << 13u;
        random ^= random >> 17u;
        random ^= random << 5u;
        fields[i] = (uint8_t)random;
    }
    lxmf_message_t message = {0}, decoded;
    memcpy(message.destination, destination, 16u);
    memcpy(message.source, source, 16u);
    message.timestamp = 94.5;
    message.content = (lxmf_slice_t){content, sizeof content - 1u};
    message.fields_msgpack = (lxmf_slice_t){fields, fields_length};
    size_t packed_length = 0u;
    assert(lxmf_pack(&message, lxmf_identity_signer, (void *)signer, packed,
                     fields_length + 256u, &packed_length) == LXMF_OK);
    assert(packed_length > RNS_RESOURCE_SINGLE_SEGMENT_MAX_SIZE);
    assert(lxmf_unpack(packed, packed_length, NULL, NULL, &decoded) == LXMF_OK);
    memcpy(message_id, decoded.message_id, 32u);
    lxmf_store_message_t stored = {0};
    memcpy(stored.message_id, message_id, 32u);
    memcpy(stored.destination, destination, 16u);
    memcpy(stored.source, source, 16u);
    stored.timestamp = message.timestamp;
    stored.status = LXMF_DELIVERY_QUEUED;
    stored.signature_state = LXMF_SIGNATURE_VERIFIED;
    stored.content = message.content;
    stored.packed = (lxmf_slice_t){packed, packed_length};
    bool inserted = false;
    assert(lxmf_store_put(store, &stored, &inserted) == LXMF_OK && inserted);
    free(fields);
    *packed_out = packed;
    return packed_length;
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
    delivery_observation_t alice_delivery = {0};
    lxmf_router_t alice_router, bob_router;
    lxmf_router_config_t alice_options = {
        .identity = &alice,
        .store = &alice_store,
        .runtime = alice_runtime,
        .resolve_identity = resolve,
        .resolve_context = &alice_resolver,
        .message_callback = delivered,
        .message_context = &alice_inbox,
        .delivery_callback = delivery_changed,
        .delivery_context = &alice_delivery,
        .event_callback = delivery_event,
        .event_context = &alice_delivery,
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
        .is_source_blocked = source_is_blocked,
        .source_policy_context = &bob_inbox,
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
    uint8_t original_packed[512u];
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
    /* Keep every synthetic ID distinct from the original random wire hash,
     * even when its first byte already equals one of our test markers. */
    outbound.message_id[LXMF_MESSAGE_ID_LENGTH - 1U] ^= 1U;
    outbound.message_id[0] = 0x92U;
    assert(memcmp(outbound.message_id, decoded.message_id,
                   LXMF_MESSAGE_ID_LENGTH) != 0);
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

    /* A representation that cannot fit in one encrypted link packet takes
     * the Resource path without rebuilding or truncating the retained wire. */
    uint8_t resource_content[RESOURCE_CONTENT_SIZE];
    uint8_t resource_packed[RESOURCE_PACKED_SIZE];
    uint8_t resource_id[LXMF_MESSAGE_ID_LENGTH];
    size_t resource_packed_length = queue_large_message(
        &alice_store, &alice, bob_destination, alice_destination, 94.0, 0x21U,
        resource_content, resource_packed, resource_id);
    memset(&alice_delivery, 0, sizeof alice_delivery);
    bob_inbox.received = false;
    assert(lxmf_router_send_message(&alice_router, resource_id) == LXMF_OK);
    {
        uint8_t content[RESOURCE_CONTENT_SIZE];
        lxmf_store_message_t stored;
        assert(lxmf_store_read(&alice_store, resource_id, &stored, content,
                               sizeof content) == LXMF_OK);
        assert(stored.status == LXMF_DELIVERY_SENDING);
        assert(stored.delivery.queue_reason == LXMF_QUEUE_REASON_RESOURCE);
        assert(stored.delivery.progress == 100000U);
        assert(stored.delivery.has_proof_id);
    }
    assert(rns_hal_monotonic_ms(&start) == RNS_OK);
    confirmed = false;
    do {
        lxmf_router_poll_result_t resource_poll;
        size_t processed = 0U;
        assert(rns_runtime_poll(bob_runtime, 8U, &processed) == RNS_OK);
        assert(rns_runtime_poll(alice_runtime, 8U, &processed) == RNS_OK);
        assert(lxmf_router_poll(&alice_router, 0U, &resource_poll) == LXMF_OK);
        uint8_t content[RESOURCE_CONTENT_SIZE];
        lxmf_store_message_t stored;
        assert(lxmf_store_read(&alice_store, resource_id, &stored, content,
                               sizeof content) == LXMF_OK);
        confirmed = stored.status == LXMF_DELIVERY_DELIVERED;
        assert(rns_hal_monotonic_ms(&now) == RNS_OK);
    } while (!confirmed && now - start < 2000U);
    assert(confirmed && bob_inbox.received);
    assert(bob_inbox.content_length == sizeof resource_content);
    assert(memcmp(bob_inbox.content, resource_content,
                  sizeof resource_content) == 0);
    assert(alice_delivery.saw_resource_progress);
    assert(alice_delivery.count == 3U);
    assert(alice_delivery.statuses[0] == LXMF_DELIVERY_SENDING);
    assert(alice_delivery.statuses[1] == LXMF_DELIVERY_SENT);
    assert(alice_delivery.statuses[2] == LXMF_DELIVERY_DELIVERED);
    uint8_t retained[RESOURCE_PACKED_SIZE];
    size_t retained_length = 0U;
    assert(lxmf_store_read_packed(&alice_store, resource_id, retained,
                                  sizeof retained, &retained_length) ==
           LXMF_OK);
    assert(retained_length == resource_packed_length);
    assert(memcmp(retained, resource_packed, retained_length) == 0);
    assert(lxmf_store_read_packed(&bob_store, resource_id, retained,
                                  sizeof retained, &retained_length) == LXMF_OK);
    assert(retained_length == resource_packed_length &&
           memcmp(retained, resource_packed, retained_length) == 0);
    lxmf_message_t received_resource;
    assert(lxmf_unpack(retained, retained_length, NULL, NULL,
                       &received_resource) == LXMF_OK &&
           received_resource.title.len == 14u &&
           memcmp(received_resource.title.data, "Resource title", 14u) == 0 &&
           received_resource.fields_msgpack.len > 1u &&
           received_resource.has_stamp &&
           received_resource.stamp_len == LXMF_POW_STAMP_LENGTH);
    assert(lxmf_pow_stamp_validate(resource_id, 1u, received_resource.stamp,
                                   NULL) == LXMF_OK);

    /* A representation beyond the one-segment bound advances monotonically
     * and is delivered only after the final segment proof. */
    uint8_t *segmented_packed = NULL;
    uint8_t segmented_id[LXMF_MESSAGE_ID_LENGTH];
    size_t segmented_packed_length = queue_segmented_message(
        &alice_store, &alice, bob_destination, alice_destination,
        &segmented_packed, segmented_id);
    memset(&alice_delivery, 0, sizeof alice_delivery);
    bob_inbox.received = false;
    assert(lxmf_router_send_message(&alice_router, segmented_id) == LXMF_OK);
    uint8_t first_segment_proof[LXMF_MESSAGE_ID_LENGTH];
    {
        uint8_t content[LXMF_STORE_MAX_CONTENT];
        lxmf_store_message_t stored;
        assert(lxmf_store_read(&alice_store, segmented_id, &stored, content,
                               sizeof content) == LXMF_OK);
        assert(stored.status == LXMF_DELIVERY_SENDING &&
               stored.delivery.queue_reason == LXMF_QUEUE_REASON_RESOURCE &&
               stored.delivery.has_proof_id);
        memcpy(first_segment_proof, stored.delivery.proof_id,
               sizeof first_segment_proof);
    }
    assert(rns_hal_monotonic_ms(&start) == RNS_OK);
    confirmed = false;
    bool saw_partial_progress = false;
    uint32_t previous_progress = 100000u;
    do {
        size_t processed = 0u;
        assert(rns_runtime_poll(bob_runtime, 32u, &processed) == RNS_OK);
        assert(rns_runtime_poll(alice_runtime, 32u, &processed) == RNS_OK);
        uint8_t content[LXMF_STORE_MAX_CONTENT];
        lxmf_store_message_t stored;
        assert(lxmf_store_read(&alice_store, segmented_id, &stored, content,
                               sizeof content) == LXMF_OK);
        assert(stored.delivery.progress >= previous_progress);
        previous_progress = stored.delivery.progress;
        if (previous_progress > 100000u &&
            previous_progress < LXMF_DELIVERY_PROGRESS_COMPLETE)
            saw_partial_progress = true;
        confirmed = stored.status == LXMF_DELIVERY_DELIVERED;
        assert(rns_hal_monotonic_ms(&now) == RNS_OK);
        /* Instrumented CI can exceed ten seconds for this >1 MiB transfer.
         * This watchdog bounds the test, not any protocol receipt deadline. */
    } while (!confirmed && now - start < 60000u);
    if (!confirmed || !bob_inbox.received || !saw_partial_progress)
        fprintf(stderr, "segmented transfer: confirmed=%d received=%d partial=%d progress=%u elapsed_ms=%llu\n",
            confirmed, bob_inbox.received, saw_partial_progress, previous_progress,
            (unsigned long long)(now - start));
    assert(confirmed && bob_inbox.received && saw_partial_progress);
    {
        uint8_t content[LXMF_STORE_MAX_CONTENT];
        lxmf_store_message_t stored;
        assert(lxmf_store_read(&alice_store, segmented_id, &stored, content,
                               sizeof content) == LXMF_OK);
        assert(stored.delivery.progress == LXMF_DELIVERY_PROGRESS_COMPLETE &&
               stored.delivery.has_proof_id &&
               memcmp(stored.delivery.proof_id, first_segment_proof,
                      sizeof first_segment_proof) != 0);
    }
    size_t received_packed_length = 0u;
    assert(lxmf_store_packed_size(&bob_store, segmented_id,
                                  &received_packed_length) == LXMF_OK &&
           received_packed_length == segmented_packed_length);
    uint8_t *received_packed = malloc(received_packed_length);
    assert(received_packed != NULL);
    assert(lxmf_store_read_packed(&bob_store, segmented_id, received_packed,
                                  received_packed_length,
                                  &received_packed_length) == LXMF_OK &&
           memcmp(received_packed, segmented_packed,
                  segmented_packed_length) == 0);
    free(received_packed);
    free(segmented_packed);

    /* Remote policy rejection is terminal and never creates a SENT state. */
    bob_router.config.max_incoming_message_size = 400U;
    (void)queue_large_message(
        &alice_store, &alice, bob_destination, alice_destination, 95.0, 0x42U,
        resource_content, resource_packed, resource_id);
    memset(&alice_delivery, 0, sizeof alice_delivery);
    bob_inbox.received = false;
    assert(lxmf_router_send_message(&alice_router, resource_id) == LXMF_OK);
    bool failed = false;
    assert(rns_hal_monotonic_ms(&start) == RNS_OK);
    do {
        lxmf_router_poll_result_t resource_poll;
        size_t processed = 0U;
        assert(rns_runtime_poll(bob_runtime, 8U, &processed) == RNS_OK);
        assert(rns_runtime_poll(alice_runtime, 8U, &processed) == RNS_OK);
        assert(lxmf_router_poll(&alice_router, 0U, &resource_poll) == LXMF_OK);
        uint8_t content[RESOURCE_CONTENT_SIZE];
        lxmf_store_message_t stored;
        assert(lxmf_store_read(&alice_store, resource_id, &stored, content,
                               sizeof content) == LXMF_OK);
        failed = stored.status == LXMF_DELIVERY_FAILED;
        assert(stored.status != LXMF_DELIVERY_SENT);
        assert(rns_hal_monotonic_ms(&now) == RNS_OK);
    } while (!failed && now - start < 1000U);
    assert(failed && !bob_inbox.received);
    assert(alice_delivery.count == 2U);
    assert(alice_delivery.statuses[0] == LXMF_DELIVERY_SENDING);
    assert(alice_delivery.statuses[1] == LXMF_DELIVERY_FAILED);

    /* A stalled Resource remains SENDING until its own deadline, then fails. */
    bob_router.config.max_incoming_message_size = 0U;
    alice_router.config.resource_timeout_seconds = 0.002;
    (void)queue_large_message(
        &alice_store, &alice, bob_destination, alice_destination, 96.0, 0x63U,
        resource_content, resource_packed, resource_id);
    memset(&alice_delivery, 0, sizeof alice_delivery);
    assert(lxmf_router_send_message(&alice_router, resource_id) == LXMF_OK);
    struct timespec pause = {.tv_sec = 0, .tv_nsec = 5000000L};
    assert(nanosleep(&pause, NULL) == 0);
    size_t processed = 0U;
    assert(rns_runtime_poll(alice_runtime, 8U, &processed) == RNS_OK);
    {
        uint8_t content[RESOURCE_CONTENT_SIZE];
        lxmf_store_message_t stored;
        assert(lxmf_store_read(&alice_store, resource_id, &stored, content,
                               sizeof content) == LXMF_OK);
        assert(stored.status == LXMF_DELIVERY_QUEUED);
        assert(stored.delivery.queue_reason == LXMF_QUEUE_REASON_RETRY_BACKOFF);
        assert(stored.delivery.retry_at_ms != 0U);
    }
    assert(alice_delivery.count == 2U);
    assert(alice_delivery.statuses[1] == LXMF_DELIVERY_QUEUED);
    assert(alice_delivery.results[1] == LXMF_ERR_TIMEOUT);
    lxmf_router_poll_result_t resource_poll;
    assert(lxmf_router_poll(&alice_router, 0U, &resource_poll) == LXMF_OK);

    /* Explicit cancellation owns and concludes the active transfer. */
    alice_router.config.resource_timeout_seconds = 1.0;
    (void)queue_large_message(
        &alice_store, &alice, bob_destination, alice_destination, 97.0, 0x84U,
        resource_content, resource_packed, resource_id);
    memset(&alice_delivery, 0, sizeof alice_delivery);
    assert(lxmf_router_send_message(&alice_router, resource_id) == LXMF_OK);
    assert(lxmf_router_cancel_message(&alice_router, resource_id) == LXMF_OK);
    assert(alice_delivery.count == 2U);
    assert(alice_delivery.statuses[1] == LXMF_DELIVERY_FAILED);
    assert(alice_delivery.results[1] == LXMF_ERR_CANCELLED);

    /* Block policy applies on an already authenticated/reused link, before
     * history delivery or an application packet proof. */
    bob_inbox.blocked = true;
    bob_inbox.received = false;
    size_t previous_count = lxmf_store_count(&bob_store);
    outbound.message_id[0] = 0x98U;
    outbound.timestamp = 98.0;
    inserted = false;
    assert(lxmf_store_put(&alice_store, &outbound, &inserted) == LXMF_OK && inserted);
    assert(lxmf_router_send_message(&alice_router, outbound.message_id) == LXMF_OK);
    for (size_t i = 0; i < 100; ++i) {
        assert(rns_runtime_poll(bob_runtime, 8U, &processed) == RNS_OK);
        assert(rns_runtime_poll(alice_runtime, 8U, &processed) == RNS_OK);
    }
    assert(!bob_inbox.received && lxmf_store_count(&bob_store) == previous_count);
    {
        uint8_t content[64];
        lxmf_store_message_t stored;
        assert(lxmf_store_read(&alice_store, outbound.message_id, &stored,
                               content, sizeof content) == LXMF_OK);
        assert(stored.status == LXMF_DELIVERY_SENT);
    }

    /* Normal router teardown preserves an active receipt for restart recovery;
     * it is not an explicit user cancellation. */
    bob_inbox.blocked = false;
    outbound.message_id[0] = 0x99U;
    outbound.timestamp = 99.0;
    inserted = false;
    assert(lxmf_store_put(&alice_store, &outbound, &inserted) == LXMF_OK &&
           inserted);
    assert(lxmf_router_send_message(&alice_router, outbound.message_id) ==
           LXMF_OK);
    lxmf_router_destroy(&alice_router);
    {
        uint8_t content[64];
        lxmf_store_message_t stored;
        assert(lxmf_store_read(&alice_store, outbound.message_id, &stored,
                               content, sizeof content) == LXMF_OK);
        assert(stored.status == LXMF_DELIVERY_SENT);
        assert(stored.delivery.has_proof_id);
        assert(stored.delivery.queue_reason != LXMF_QUEUE_REASON_CANCELLED);
    }
    assert(lxmf_router_init(&alice_router, &alice_options) == LXMF_OK);
    {
        uint8_t content[64];
        lxmf_store_message_t stored;
        assert(lxmf_store_read(&alice_store, outbound.message_id, &stored,
                               content, sizeof content) == LXMF_OK);
        assert(stored.status == LXMF_DELIVERY_QUEUED);
        assert(stored.delivery.queue_reason ==
               LXMF_QUEUE_REASON_RETRY_BACKOFF);
        assert(!stored.delivery.has_proof_id);
    }

    /* Re-establish a direct link, then ensure teardown also preserves an
     * active Resource instead of converting it to a durable cancellation. */
    outbound.message_id[0] = 0x9aU;
    outbound.timestamp = 100.0;
    inserted = false;
    assert(lxmf_store_put(&alice_store, &outbound, &inserted) == LXMF_OK &&
           inserted);
    assert(lxmf_router_send_message(&alice_router, outbound.message_id) ==
           LXMF_ERR_PENDING);
    assert(rns_hal_monotonic_ms(&start) == RNS_OK);
    confirmed = false;
    do {
        lxmf_router_poll_result_t poll_result;
        size_t poll_processed = 0U;
        assert(rns_runtime_poll(bob_runtime, 8U, &poll_processed) == RNS_OK);
        assert(rns_runtime_poll(alice_runtime, 8U, &poll_processed) == RNS_OK);
        assert(lxmf_router_poll(&alice_router, 4U, &poll_result) == LXMF_OK);
        uint8_t content[64];
        lxmf_store_message_t stored;
        assert(lxmf_store_read(&alice_store, outbound.message_id, &stored,
                               content, sizeof content) == LXMF_OK);
        confirmed = stored.status == LXMF_DELIVERY_DELIVERED;
        assert(rns_hal_monotonic_ms(&now) == RNS_OK);
    } while (!confirmed && now - start < 3000U);
    assert(confirmed);

    (void)queue_large_message(
        &alice_store, &alice, bob_destination, alice_destination, 101.0,
        0xa5U, resource_content, resource_packed, resource_id);
    assert(lxmf_router_send_message(&alice_router, resource_id) == LXMF_OK);
    lxmf_router_destroy(&alice_router);
    {
        uint8_t content[RESOURCE_CONTENT_SIZE];
        lxmf_store_message_t stored;
        assert(lxmf_store_read(&alice_store, resource_id, &stored, content,
                               sizeof content) == LXMF_OK);
        assert(stored.status == LXMF_DELIVERY_SENDING);
        assert(stored.delivery.queue_reason == LXMF_QUEUE_REASON_RESOURCE);
    }
    assert(lxmf_router_init(&alice_router, &alice_options) == LXMF_OK);
    {
        uint8_t content[RESOURCE_CONTENT_SIZE];
        lxmf_store_message_t stored;
        assert(lxmf_store_read(&alice_store, resource_id, &stored, content,
                               sizeof content) == LXMF_OK);
        assert(stored.status == LXMF_DELIVERY_QUEUED);
        assert(stored.delivery.queue_reason ==
               LXMF_QUEUE_REASON_RETRY_BACKOFF);
        assert(!stored.delivery.has_proof_id);
    }

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
