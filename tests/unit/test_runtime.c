#include "reticulum/runtime.h"

#include "reticulum/packet.h"

#include <assert.h>
#include <string.h>

static void accepted_link(rns_runtime_destination_t *destination,
                          rns_runtime_link_t *link, void *context) {
    (void)destination;
    (void)link;
    (void)context;
}

/*
 * Routing and announce argument handling. End-to-end delivery is covered by
 * the two-instance loopback harness, which needs live sockets.
 */
static void test_routed_and_announce(rns_runtime_t *runtime) {
    static const char *const aspects[] = {"delivery"};
    uint8_t unreachable[16] = {0xabU};
    uint8_t packet[64];
    size_t packet_length = 0U;
    rns_packet outer = {0};
    rns_identity identity;
    uint8_t payload[8] = {1U, 2U, 3U, 4U, 5U, 6U, 7U, 8U};

    memcpy(outer.destination_hash, unreachable, sizeof outer.destination_hash);
    outer.data = payload;
    outer.data_length = sizeof payload;
    assert(rns_packet_encode(&outer, packet, sizeof packet, &packet_length));

    assert(rns_runtime_send_routed(NULL, packet, packet_length) ==
           RNS_ERROR_INVALID_ARGUMENT);
    assert(rns_runtime_send_routed(runtime, NULL, packet_length) ==
           RNS_ERROR_INVALID_ARGUMENT);
    /* Undecodable input is rejected before any path lookup. */
    assert(rns_runtime_send_routed(runtime, packet, 1U) == RNS_ERROR_INVALID_ARGUMENT);
    /* A destination with no learned path is reported, not silently dropped. */
    assert(rns_runtime_send_routed(runtime, packet, packet_length) ==
           RNS_ERROR_NOT_FOUND);

    assert(rns_identity_generate(&identity));
    rns_packet_receipt_t *packet_receipt = (rns_packet_receipt_t *)1;
    assert(rns_runtime_send_routed_with_receipt(
               runtime, packet, packet_length, NULL, NULL,
               &packet_receipt) == RNS_ERROR_INVALID_ARGUMENT);
    assert(rns_runtime_send_routed_with_receipt(
               runtime, packet, packet_length, &identity, NULL,
               &packet_receipt) == RNS_ERROR_NOT_FOUND);
    assert(packet_receipt == NULL);
    assert(rns_packet_receipt_state(NULL) == RNS_PACKET_RECEIPT_FAILED);
    assert(rns_packet_receipt_hash(NULL) == NULL);
    assert(rns_packet_receipt_rtt(NULL) == 0.0);
    rns_packet_receipt_cancel(NULL);
    rns_packet_receipt_destroy(NULL);
    assert(rns_runtime_announce(NULL, &identity, "lxmf", aspects, 1U, NULL, 0U) ==
           RNS_ERROR_INVALID_ARGUMENT);
    assert(rns_runtime_announce(runtime, NULL, "lxmf", aspects, 1U, NULL, 0U) ==
           RNS_ERROR_INVALID_ARGUMENT);
    assert(rns_runtime_announce(runtime, &identity, NULL, aspects, 1U, NULL, 0U) ==
           RNS_ERROR_INVALID_ARGUMENT);
    /* No interface is up in this fixture, so announcing cannot succeed. */
    assert(rns_runtime_announce(runtime, &identity, "lxmf", aspects, 1U, NULL, 0U) !=
           RNS_OK);
}

int main(void) {
    rns_config_t config;
    rns_runtime_t *runtime = NULL;
    rns_runtime_interface_info_t info;
    rns_runtime_link_t *link = (rns_runtime_link_t *)1;
    rns_request_receipt_t *receipt = (rns_request_receipt_t *)1;
    rns_runtime_destination_t *registration =
        (rns_runtime_destination_t *)1;
    rns_identity remote;
    uint8_t destination[16] = {1U};
    size_t processed = 99U;

    rns_config_init(&config);
    config.interface_count = 2U;
    (void)strcpy(config.interfaces[0].name, "disabled UDP");
    config.interfaces[0].type = RNS_CONFIG_UDP;
    config.interfaces[0].type_set = true;
    (void)strcpy(config.interfaces[1].name, "radio placeholder");
    config.interfaces[1].type = RNS_CONFIG_RNODE;
    config.interfaces[1].type_set = true;
    config.interfaces[1].enabled = true;

    assert(rns_runtime_create(NULL, &config, NULL) == RNS_ERROR_INVALID_ARGUMENT);
    assert(rns_runtime_create(&runtime, &config, NULL) == RNS_OK);
    assert(rns_identity_generate(&remote));
    assert(rns_runtime_link_open(runtime, destination, &remote, NULL, &link) ==
           RNS_ERROR_NOT_FOUND);
    assert(link == NULL);
    assert(rns_runtime_link_state(NULL) == RNS_LINK_CLOSED);
    assert(rns_runtime_link_id(NULL) == NULL);
    assert(rns_runtime_link_request(NULL, "/page/index.mu", NULL, 0U, NULL,
                                    &receipt) == RNS_ERROR_INVALID_ARGUMENT);
    assert(rns_request_receipt_state(NULL) == RNS_REQUEST_FAILED);
    assert(rns_request_receipt_id(NULL) == NULL);
    rns_request_receipt_cancel(NULL);
    rns_request_receipt_destroy(NULL);
    assert(runtime != NULL);
    assert(rns_runtime_interface_count(runtime) == 2U);
    assert(rns_runtime_interface_info(runtime, 0U, &info) == RNS_OK);
    assert(strcmp(info.name, "disabled UDP") == 0);
    assert(info.state == RNS_RUNTIME_INTERFACE_DISABLED);
    assert(rns_runtime_interface_info(runtime, 1U, &info) == RNS_OK);
    assert(info.state == RNS_RUNTIME_INTERFACE_UNSUPPORTED);
    assert(info.last_error == RNS_ERROR_UNSUPPORTED);
    assert(rns_runtime_interface_info(runtime, 2U, &info) == RNS_ERROR_INVALID_ARGUMENT);
    assert(rns_runtime_register_destination(runtime, destination) == RNS_OK);
    assert(rns_runtime_register_destination(runtime, destination) == RNS_OK);
    assert(rns_runtime_register_link_destination(
               NULL, destination, &remote, NULL, accepted_link, NULL,
               &registration) == RNS_ERROR_INVALID_ARGUMENT);
    assert(rns_runtime_register_link_destination(
               runtime, destination, &remote, NULL, NULL, NULL,
               &registration) == RNS_ERROR_INVALID_ARGUMENT);
    assert(rns_runtime_register_link_destination(
               runtime, destination, &remote, NULL, accepted_link, NULL,
               &registration) == RNS_OK);
    assert(registration != NULL);
    assert(memcmp(rns_runtime_destination_hash(registration), destination,
                  sizeof destination) == 0);
    rns_runtime_destination_t *duplicate = NULL;
    assert(rns_runtime_register_link_destination(
               runtime, destination, &remote, NULL, accepted_link, NULL,
               &duplicate) == RNS_ERROR_INVALID_STATE);
    assert(duplicate == NULL);
    rns_runtime_destination_destroy(registration);
    /* The plain registration for the same hash remains live and removable. */
    assert(rns_runtime_unregister_destination(runtime, destination) == RNS_OK);
    assert(rns_runtime_unregister_destination(runtime, destination) == RNS_ERROR_NOT_FOUND);
    assert(rns_runtime_destination_hash(NULL) == NULL);
    rns_runtime_destination_destroy(NULL);
    assert(rns_runtime_request_path(runtime, destination) == RNS_ERROR_INVALID_STATE);
    assert(rns_runtime_path_lookup(runtime, destination, &(rns_path_entry){0}) == RNS_ERROR_NOT_FOUND);
    assert(rns_runtime_path_snapshot(runtime, NULL, 0U) == 0U);
    assert(rns_runtime_poll(runtime, 0U, &processed) == RNS_OK);
    assert(processed == 0U);
    /* A disabled interface has no startup error, but it cannot send. */
    assert(rns_runtime_send(runtime, 0U, destination, sizeof(destination)) ==
           RNS_ERROR_INVALID_STATE);
    test_routed_and_announce(runtime);
    rns_runtime_destroy(runtime);

    config.panic_on_interface_error = true;
    runtime = NULL;
    assert(rns_runtime_create(&runtime, &config, NULL) == RNS_ERROR_UNSUPPORTED);
    assert(runtime == NULL);
    return 0;
}
