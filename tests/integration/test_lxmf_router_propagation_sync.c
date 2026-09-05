#define _POSIX_C_SOURCE 200809L
#include "reticulum/crypto.h"
#include "reticulum/destination.h"
#include "reticulum/lxmf_router.h"
#include "reticulum/udp.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define TEST_MESSAGES 7u
#define TEST_WIRE_MAX 4096u

typedef struct {
    uint8_t wire[TEST_MESSAGES][TEST_WIRE_MAX];
    size_t wire_length[TEST_MESSAGES];
    uint8_t transient_id[TEST_MESSAGES][LXMF_MESSAGE_ID_LENGTH];
    size_t selected;
    size_t wrong_response;
    bool corrupt_response_hash;
    size_t acknowledged;
    rns_runtime_link_t *links[32];
    size_t link_count;
} node_t;

typedef struct {
    const rns_identity *sender;
    uint8_t sender_destination[LXMF_DESTINATION_LENGTH];
    bool blocked;
    size_t delivered;
} client_t;

static uint16_t unused_port(void) {
    rns_udp_endpoint_t *endpoint = NULL;
    rns_udp_address_t address;
    assert(rns_udp_endpoint_create(&endpoint, RNS_UDP_IPV4) == RNS_OK);
    assert(rns_udp_bind(endpoint, "127.0.0.1", 0u) == RNS_OK);
    assert(rns_udp_local_address(endpoint, &address) == RNS_OK);
    rns_udp_endpoint_destroy(endpoint);
    return address.port;
}

static void configure(rns_config_t *config, uint16_t listen, uint16_t forward) {
    rns_config_init(config);
    config->interface_count = 1u;
    rns_config_interface_t *interface = &config->interfaces[0];
    (void)strcpy(interface->name, "router-sync-test");
    interface->type = RNS_CONFIG_UDP;
    interface->type_set = true;
    interface->enabled = true;
    (void)strcpy(interface->listen_ip, "127.0.0.1");
    (void)strcpy(interface->forward_ip, "127.0.0.1");
    interface->listen_port = listen;
    interface->forward_port = forward;
}

static void accepted(rns_runtime_destination_t *destination,
                     rns_runtime_link_t *link, void *context) {
    node_t *node = context;
    (void)destination;
    assert(node->link_count < sizeof node->links / sizeof node->links[0]);
    node->links[node->link_count++] = link;
}

static rns_status_t serve_get(
    rns_runtime_request_handler_t *handler, rns_runtime_link_t *link,
    const rns_request_view_t *request, const rns_identity *identity,
    uint8_t *output, size_t capacity, size_t *output_length, void *context) {
    node_t *node = context;
    (void)handler;
    (void)link;
    assert(identity != NULL);
    lxmf_pn_get_request_t get;
    assert(lxmf_pn_get_request_decode(request->data_msgpack,
        request->data_msgpack_length, &get) == LXMF_OK);
    lxmf_pn_get_response_t response = {0};
    bool listing = get.wants_null && get.haves_null;
    if (listing) {
        response.count = 1u;
        response.items[0] = (lxmf_slice_t){
            node->transient_id[node->selected], LXMF_MESSAGE_ID_LENGTH};
    } else if (get.wants_null) {
        assert(!get.haves_null);
        node->acknowledged += get.haves_count;
    } else {
        assert(get.wants_count == 1u);
        size_t selected = node->corrupt_response_hash
            ? node->wrong_response : node->selected;
        response.count = 1u;
        response.items[0] = (lxmf_slice_t){
            node->wire[selected], node->wire_length[selected]};
    }
    return lxmf_pn_get_response_encode(&response, listing, output, capacity,
        output_length) == LXMF_OK ? RNS_OK : RNS_ERROR_OVERFLOW;
}

static const rns_identity *resolve_sender(
    void *context, const uint8_t destination[LXMF_DESTINATION_LENGTH]) {
    client_t *client = context;
    return memcmp(destination, client->sender_destination,
                  LXMF_DESTINATION_LENGTH) == 0 ? client->sender : NULL;
}

static bool source_blocked(
    void *context, const uint8_t source[LXMF_SOURCE_LENGTH]) {
    client_t *client = context;
    return client->blocked &&
        memcmp(source, client->sender_destination, LXMF_SOURCE_LENGTH) == 0;
}

static void delivered(void *context, const lxmf_store_message_t *message) {
    client_t *client = context;
    assert(message->delivery.actual_method == LXMF_DELIVERY_METHOD_PROPAGATED);
    client->delivered++;
}

static uint64_t wall_clock(void *context) {
    (void)context;
    return 1700000000u;
}

static void queue_propagated(
    lxmf_store_t *store, const rns_identity *sender,
    const uint8_t source[LXMF_SOURCE_LENGTH],
    const uint8_t destination[LXMF_DESTINATION_LENGTH],
    uint8_t message_id[LXMF_MESSAGE_ID_LENGTH]) {
    static const uint8_t content[] = "serialized upload";
    lxmf_message_t message = {0}, decoded;
    memcpy(message.destination, destination, LXMF_DESTINATION_LENGTH);
    memcpy(message.source, source, LXMF_SOURCE_LENGTH);
    message.timestamp = 4242.0;
    message.content = (lxmf_slice_t){content, sizeof content - 1u};
    uint8_t packed[512];
    size_t packed_length = 0u;
    assert(lxmf_pack(&message, lxmf_identity_signer, (void *)sender,
                     packed, sizeof packed, &packed_length) == LXMF_OK);
    assert(lxmf_unpack(packed, packed_length, NULL, NULL, &decoded) == LXMF_OK);
    memcpy(message_id, decoded.message_id, LXMF_MESSAGE_ID_LENGTH);
    lxmf_store_message_t stored = {0};
    memcpy(stored.message_id, message_id, LXMF_MESSAGE_ID_LENGTH);
    memcpy(stored.destination, destination, LXMF_DESTINATION_LENGTH);
    memcpy(stored.source, source, LXMF_SOURCE_LENGTH);
    stored.timestamp = message.timestamp;
    stored.status = LXMF_DELIVERY_QUEUED;
    stored.signature_state = LXMF_SIGNATURE_VERIFIED;
    stored.content = message.content;
    stored.packed = (lxmf_slice_t){packed, packed_length};
    stored.delivery.desired_method = LXMF_DELIVERY_METHOD_PROPAGATED;
    bool inserted = false;
    assert(lxmf_store_put(store, &stored, &inserted) == LXMF_OK && inserted);
}

static void make_wire(
    node_t *node, size_t index, const rns_identity *recipient,
    const uint8_t outer_destination[LXMF_DESTINATION_LENGTH],
    const uint8_t message_destination[LXMF_DESTINATION_LENGTH],
    const uint8_t source[LXMF_SOURCE_LENGTH], const rns_identity *signer) {
    uint8_t content[2048];
    size_t content_length = index == 0u ? sizeof content : 32u;
    memset(content, (int)(index + 1u), content_length);
    lxmf_message_t message = {0};
    memcpy(message.destination, message_destination, LXMF_DESTINATION_LENGTH);
    memcpy(message.source, source, LXMF_SOURCE_LENGTH);
    message.timestamp = 1000.0 + (double)index;
    message.content = (lxmf_slice_t){content, content_length};
    uint8_t packed[2304];
    size_t packed_length = 0u;
    assert(lxmf_pack(&message, lxmf_identity_signer, (void *)signer,
                     packed, sizeof packed, &packed_length) == LXMF_OK);
    memcpy(node->wire[index], outer_destination, LXMF_DESTINATION_LENGTH);
    size_t encrypted_length = 0u;
    assert(rns_identity_encrypt(
        recipient, NULL, packed + LXMF_DESTINATION_LENGTH,
        packed_length - LXMF_DESTINATION_LENGTH,
        node->wire[index] + LXMF_DESTINATION_LENGTH,
        sizeof node->wire[index] - LXMF_DESTINATION_LENGTH,
        &encrypted_length));
    node->wire_length[index] = LXMF_DESTINATION_LENGTH + encrypted_length;
    assert(rns_sha256(node->wire[index], node->wire_length[index],
                      node->transient_id[index]));
}

static void pump(rns_runtime_t *client_runtime, rns_runtime_t *node_runtime,
                 lxmf_router_t *router,
                 lxmf_router_propagation_sync_status_t *status) {
    for (size_t i = 0u; i < 20000u; ++i) {
        size_t processed = 0u;
        lxmf_router_poll_result_t result;
        assert(rns_runtime_poll(client_runtime, 32u, &processed) == RNS_OK);
        assert(rns_runtime_poll(node_runtime, 32u, &processed) == RNS_OK);
        assert(lxmf_router_poll(router, 0u, &result) == LXMF_OK);
        assert(lxmf_router_propagation_sync_status(router, status) == LXMF_OK);
        if (!status->active) return;
    }
    assert(0 && "router propagation sync did not finish");
}

static void start_and_pump(
    rns_runtime_t *client_runtime, rns_runtime_t *node_runtime,
    lxmf_router_t *router, bool retain_on_node,
    lxmf_router_propagation_sync_status_t *status) {
    assert(lxmf_router_propagation_sync_start(router, retain_on_node) == LXMF_OK);
    pump(client_runtime, node_runtime, router, status);
}

int main(void) {
    uint16_t client_port = unused_port(), node_port = unused_port();
    while (client_port == node_port) node_port = unused_port();
    rns_config_t client_config, node_config;
    configure(&client_config, client_port, node_port);
    configure(&node_config, node_port, client_port);
    rns_runtime_t *client_runtime = NULL, *node_runtime = NULL;
    assert(rns_runtime_create(&client_runtime, &client_config, NULL) == RNS_OK);
    assert(rns_runtime_create(&node_runtime, &node_config, NULL) == RNS_OK);

    rns_identity local, node_identity, second_node, sender, forged_signer;
    assert(rns_identity_generate(&local));
    assert(rns_identity_generate(&node_identity));
    assert(rns_identity_generate(&second_node));
    assert(rns_identity_generate(&sender));
    assert(rns_identity_generate(&forged_signer));
    static const char *const delivery_aspects[] = {"delivery"};
    static const char *const propagation_aspects[] = {"propagation"};
    uint8_t local_destination[LXMF_DESTINATION_LENGTH];
    uint8_t wrong_destination[LXMF_DESTINATION_LENGTH] = {0x55u};
    uint8_t node_destination[LXMF_DESTINATION_LENGTH];
    uint8_t second_node_destination[LXMF_DESTINATION_LENGTH];
    client_t client = {.sender = &sender};
    assert(rns_destination_hash(&local, "lxmf", delivery_aspects, 1u,
                                local_destination));
    assert(rns_destination_hash(&sender, "lxmf", delivery_aspects, 1u,
                                client.sender_destination));
    assert(rns_destination_hash(&node_identity, "lxmf", propagation_aspects,
                                1u, node_destination));
    assert(rns_destination_hash(&second_node, "lxmf", propagation_aspects,
                                1u, second_node_destination));

    node_t node = {0};
    for (size_t i = 0u; i < TEST_MESSAGES; ++i)
        make_wire(&node, i, &local, local_destination, local_destination,
                  client.sender_destination, &sender);
    make_wire(&node, 1u, &local, wrong_destination, wrong_destination,
              client.sender_destination, &sender);
    make_wire(&node, 5u, &local, local_destination, local_destination,
              client.sender_destination, &forged_signer);
    node.wrong_response = 6u;

    rns_runtime_link_options_t link_options = {0};
    link_options.callback_context = &node;
    rns_runtime_destination_t *registration = NULL;
    assert(rns_runtime_register_link_destination(
        node_runtime, node_destination, &node_identity, &link_options,
        accepted, &node, &registration) == RNS_OK);
    rns_runtime_request_handler_options_t handler_options = {0};
    handler_options.access = RNS_REQUEST_ALLOW_IDENTIFIED;
    handler_options.max_response_size = 4096u;
    handler_options.callback = serve_get;
    handler_options.callback_context = &node;
    rns_runtime_request_handler_t *handler = NULL;
    assert(rns_runtime_destination_register_request_handler(
        registration, LXMF_PN_GET_PATH, &handler_options, &handler) == RNS_OK);
    assert(rns_runtime_announce(node_runtime, &node_identity, "lxmf",
        propagation_aspects, 1u, NULL, 0u) == RNS_OK);
    size_t processed = 0u;
    assert(rns_runtime_poll(client_runtime, 32u, &processed) == RNS_OK);

    char store_path[] = "/tmp/lxmf-router-sync-XXXXXX";
    int descriptor = mkstemp(store_path);
    assert(descriptor >= 0);
    assert(close(descriptor) == 0 && unlink(store_path) == 0);
    lxmf_store_t store = {0};
    assert(lxmf_store_open(&store, store_path) == LXMF_OK);
    lxmf_router_config_t router_config = {
        .identity = &local,
        .store = &store,
        .runtime = client_runtime,
        .resolve_identity = resolve_sender,
        .resolve_context = &client,
        .wall_clock = wall_clock,
        .is_source_blocked = source_blocked,
        .source_policy_context = &client,
        .message_callback = delivered,
        .message_context = &client,
        .preferred_delivery_method = LXMF_DELIVERY_METHOD_DIRECT,
        .propagation_node_identity = &node_identity,
        .propagation_stamp_cost = 1u};
    memcpy(router_config.propagation_node_destination, node_destination,
           LXMF_DESTINATION_LENGTH);
    lxmf_router_t router;
    assert(lxmf_router_init(&router, &router_config) == LXMF_OK);
    lxmf_router_propagation_sync_status_t status;
    assert(lxmf_router_propagation_sync_status(NULL, &status) ==
           LXMF_ERR_ARGUMENT);
    assert(lxmf_router_set_propagation_node(&router, NULL, NULL, 0u) ==
           LXMF_OK);
    assert(lxmf_router_propagation_sync_start(&router, false) ==
           LXMF_ERR_ARGUMENT);
    uint8_t invalid_node_destination[LXMF_DESTINATION_LENGTH];
    memcpy(invalid_node_destination, node_destination,
           sizeof invalid_node_destination);
    invalid_node_destination[0] ^= 1u;
    assert(lxmf_router_set_propagation_node(
        &router, &node_identity, invalid_node_destination, 1u) ==
           LXMF_ERR_ARGUMENT);
    assert(lxmf_router_set_propagation_node(
        &router, &node_identity, node_destination, 1u) == LXMF_OK);

    node.selected = 0u;
    /* Reserve the upload slot without a network transfer: accepted sync intent
     * must not allocate a second session or change its retain policy on repeat. */
    router.propagation.used = true;
    assert(lxmf_router_propagation_sync_start(&router, true) == LXMF_OK);
    assert(lxmf_router_propagation_sync_status(&router, &status) == LXMF_OK);
    assert(status.active && status.waiting_for_upload && status.retain_on_node);
    assert(router.propagation_sync.session == NULL);
    assert(lxmf_router_propagation_sync_start(&router, false) == LXMF_ERR_PENDING);
    assert(lxmf_router_set_propagation_node(&router, &second_node,
        second_node_destination, 1u) == LXMF_ERR_PENDING);
    assert(lxmf_router_propagation_sync_cancel(&router) == LXMF_OK);
    assert(router.propagation.used); /* Cancelling sync must not cancel upload. */
    assert(lxmf_router_propagation_sync_status(&router, &status) == LXMF_OK);
    assert(!status.active && !status.waiting_for_upload);
    assert(status.state == LXMF_PN_CANCELLED);
    assert(lxmf_router_propagation_sync_start(&router, false) == LXMF_OK);
    router.propagation.used = false;
    /* Polling starts the deferred transaction without another user action. */
    pump(client_runtime, node_runtime, &router, &status);
    assert(!status.waiting_for_upload);
    assert(status.state == LXMF_PN_COMPLETE && status.result == LXMF_OK);
    assert(status.accepted == 1u && status.duplicates == 0u &&
           status.rejected == 0u && status.acknowledged == 1u);
    assert(client.delivered == 1u && node.acknowledged == 1u);

    lxmf_router_destroy(&router);
    lxmf_store_close(&store);
    assert(lxmf_store_open(&store, store_path) == LXMF_OK);
    assert(lxmf_router_init(&router, &router_config) == LXMF_OK);
    start_and_pump(client_runtime, node_runtime, &router, false, &status);
    assert(status.state == LXMF_PN_COMPLETE && status.result == LXMF_OK);
    assert(status.accepted == 0u && status.duplicates == 1u &&
           status.acknowledged == 1u);
    assert(client.delivered == 1u && node.acknowledged == 2u);

    uint8_t outbound_id[LXMF_MESSAGE_ID_LENGTH];
    queue_propagated(&store, &local, local_destination,
                     client.sender_destination, outbound_id);
    router.propagation.used = true;
    assert(lxmf_router_propagation_sync_start(&router, false) == LXMF_OK);
    router.propagation.used = false;
    lxmf_router_poll_result_t scheduled;
    assert(lxmf_router_poll(&router, 1u, &scheduled) == LXMF_OK);
    assert(router.propagation_sync.status.active);
    assert(!router.propagation_sync.status.waiting_for_upload);
    assert(!router.propagation.used); /* The next upload cannot leapfrog sync. */
    assert(lxmf_router_send_message(&router, outbound_id) == LXMF_ERR_PENDING);
    lxmf_delivery_metadata_t outbound_delivery;
    assert(lxmf_store_read_delivery(&store, outbound_id,
                                    &outbound_delivery) == LXMF_OK);
    assert(outbound_delivery.queue_reason ==
           LXMF_QUEUE_REASON_PROPAGATION_NODE);
    assert(!router.propagation.used);
    assert(lxmf_router_set_propagation_node(
        &router, &node_identity, node_destination, 1u) == LXMF_OK);
    assert(lxmf_router_set_propagation_node(
        &router, &second_node, second_node_destination, 1u) ==
           LXMF_ERR_PENDING);
    assert(lxmf_router_set_propagation_node(&router, NULL, NULL, 0u) ==
           LXMF_ERR_PENDING);
    assert(lxmf_router_propagation_sync_cancel(&router) == LXMF_OK);
    assert(lxmf_router_propagation_sync_status(&router, &status) == LXMF_OK);
    assert(status.state == LXMF_PN_CANCELLED &&
           status.result == LXMF_ERR_CANCELLED && !status.active);
    assert(lxmf_router_propagation_sync_cancel(&router) == LXMF_ERR_FORMAT);
    assert(lxmf_router_set_propagation_node(
        &router, &second_node, second_node_destination, 1u) == LXMF_OK);
    assert(lxmf_router_set_propagation_node(
        &router, &node_identity, node_destination, 1u) == LXMF_OK);

    size_t acknowledgements = node.acknowledged;
    node.selected = 1u;
    start_and_pump(client_runtime, node_runtime, &router, false, &status);
    assert(status.state == LXMF_PN_COMPLETE && status.result == LXMF_ERR_FORMAT);
    assert(status.rejected == 1u && status.acknowledged == 0u);
    assert(node.acknowledged == acknowledgements);

    node.selected = 2u;
    client.blocked = true;
    start_and_pump(client_runtime, node_runtime, &router, false, &status);
    assert(status.state == LXMF_PN_COMPLETE && status.result == LXMF_ERR_BLOCKED);
    assert(status.rejected == 1u && node.acknowledged == acknowledgements);
    client.blocked = false;

    node.selected = 3u;
    assert(lxmf_router_set_inbound_stamp_cost(&router, 1u) == LXMF_OK);
    start_and_pump(client_runtime, node_runtime, &router, false, &status);
    assert(status.state == LXMF_PN_COMPLETE && status.result == LXMF_ERR_STAMP);
    assert(status.rejected == 1u && node.acknowledged == acknowledgements);
    assert(lxmf_router_set_inbound_stamp_cost(&router, 0u) == LXMF_OK);

    node.selected = 5u;
    start_and_pump(client_runtime, node_runtime, &router, false, &status);
    assert(status.state == LXMF_PN_COMPLETE &&
           status.result == LXMF_ERR_SIGNATURE);
    assert(status.rejected == 1u && node.acknowledged == acknowledgements);

    node.selected = 4u;
    node.corrupt_response_hash = true;
    start_and_pump(client_runtime, node_runtime, &router, true, &status);
    assert(status.state == LXMF_PN_FAILED && status.result == LXMF_ERR_FORMAT);
    assert(status.rejected == 0u && status.acknowledged == 0u &&
           status.retain_on_node);
    node.corrupt_response_hash = false;
    start_and_pump(client_runtime, node_runtime, &router, true, &status);
    assert(status.state == LXMF_PN_COMPLETE && status.result == LXMF_OK);
    assert(status.accepted == 1u && status.duplicates == 0u &&
           status.acknowledged == 0u && status.retain_on_node);
    assert(node.acknowledged == acknowledgements);

    lxmf_router_destroy(&router);
    lxmf_store_close(&store);
    assert(unlink(store_path) == 0);
    for (size_t i = 0u; i < node.link_count; ++i)
        rns_runtime_link_destroy(node.links[i]);
    rns_runtime_destination_destroy(registration);
    rns_runtime_destroy(client_runtime);
    rns_runtime_destroy(node_runtime);
    return 0;
}
