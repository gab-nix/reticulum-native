#include "reticulum/destination.h"
#include "reticulum/runtime.h"
#include "reticulum/udp.h"

#include <assert.h>
#include <stdbool.h>
#include <string.h>

typedef struct observation {
    rns_runtime_link_t *accepted;
    size_t active;
    size_t packets;
    uint8_t payload[32];
    size_t payload_length;
} observation_t;

static uint16_t reserve_udp_port(void) {
    rns_udp_endpoint_t *endpoint = NULL;
    rns_udp_address_t address;
    assert(rns_udp_endpoint_create(&endpoint, RNS_UDP_IPV4) == RNS_OK);
    assert(rns_udp_bind(endpoint, "127.0.0.1", 0) == RNS_OK);
    assert(rns_udp_local_address(endpoint, &address) == RNS_OK);
    rns_udp_endpoint_destroy(endpoint);
    return address.port;
}

static void add_udp(rns_config_t *config, const char *name,
                    uint16_t listen_port, uint16_t forward_port) {
    size_t index = config->interface_count++;
    assert(index < RNS_CONFIG_MAX_INTERFACES);
    rns_config_interface_t *interface = &config->interfaces[index];
    (void)strcpy(interface->name, name);
    interface->type = RNS_CONFIG_UDP;
    interface->type_set = true;
    interface->enabled = true;
    (void)strcpy(interface->listen_ip, "127.0.0.1");
    (void)strcpy(interface->forward_ip, "127.0.0.1");
    interface->listen_port = listen_port;
    interface->forward_port = forward_port;
}

static void state_changed(rns_runtime_link_t *link, rns_link_state state,
                          rns_status_t reason, void *context) {
    observation_t *observation = context;
    assert(link != NULL && reason == RNS_OK);
    if (state == RNS_LINK_ACTIVE) observation->active++;
}

static void packet_received(rns_runtime_link_t *link, uint8_t context,
                            const uint8_t *payload, size_t payload_length,
                            void *opaque) {
    observation_t *observation = opaque;
    assert(link != NULL && context == 0x31 &&
           payload_length <= sizeof observation->payload);
    memcpy(observation->payload, payload, payload_length);
    observation->payload_length = payload_length;
    observation->packets++;
}

static void accepted(rns_runtime_destination_t *destination,
                     rns_runtime_link_t *link, void *context) {
    observation_t *observation = context;
    assert(destination != NULL && link != NULL);
    observation->accepted = link;
}

static void poll_three(rns_runtime_t *first, rns_runtime_t *transport,
                       rns_runtime_t *second) {
    size_t processed = 0;
    assert(rns_runtime_poll(first, 16, &processed) == RNS_OK);
    assert(rns_runtime_poll(transport, 16, &processed) == RNS_OK);
    assert(rns_runtime_poll(second, 16, &processed) == RNS_OK);
}

int main(void) {
    uint16_t alice_port = reserve_udp_port();
    uint16_t bob_port = reserve_udp_port();
    uint16_t transport_alice_port = reserve_udp_port();
    uint16_t transport_bob_port = reserve_udp_port();
    assert(alice_port != bob_port && alice_port != transport_alice_port &&
           alice_port != transport_bob_port && bob_port != transport_alice_port &&
           bob_port != transport_bob_port &&
           transport_alice_port != transport_bob_port);

    rns_config_t alice_config, bob_config, transport_config;
    rns_config_init(&alice_config);
    rns_config_init(&bob_config);
    rns_config_init(&transport_config);
    add_udp(&alice_config, "alice", alice_port, transport_alice_port);
    add_udp(&bob_config, "bob", bob_port, transport_bob_port);
    add_udp(&transport_config, "towards alice", transport_alice_port,
            alice_port);
    add_udp(&transport_config, "towards bob", transport_bob_port, bob_port);
    transport_config.enable_transport = true;

    rns_runtime_t *alice = NULL, *bob = NULL, *transport = NULL;
    assert(rns_runtime_create(&alice, &alice_config, NULL) == RNS_OK);
    assert(rns_runtime_create(&transport, &transport_config, NULL) == RNS_OK);
    assert(rns_runtime_create(&bob, &bob_config, NULL) == RNS_OK);

    rns_identity bob_identity;
    assert(rns_identity_generate(&bob_identity));
    static const char *const aspects[] = {"delivery"};
    uint8_t bob_destination[16];
    assert(rns_destination_hash(&bob_identity, "lxmf", aspects, 1,
                                bob_destination));

    observation_t alice_observation = {0}, bob_observation = {0};
    rns_runtime_link_options_t bob_link_options = {
        .timeout_seconds = 2,
        .state_callback = state_changed,
        .packet_callback = packet_received,
        .callback_context = &bob_observation};
    rns_runtime_destination_t *registration = NULL;
    assert(rns_runtime_register_link_destination(
               bob, bob_destination, &bob_identity, &bob_link_options,
               accepted, &bob_observation, &registration) == RNS_OK);
    assert(rns_runtime_announce(bob, &bob_identity, "lxmf", aspects, 1,
                                NULL, 0) == RNS_OK);

    bool path_known = false;
    for (size_t attempt = 0; attempt < 2000 && !path_known; ++attempt) {
        poll_three(bob, transport, alice);
        rns_path_entry path;
        path_known = rns_runtime_path_lookup(alice, bob_destination, &path) ==
                     RNS_OK;
    }
    assert(path_known);

    rns_runtime_link_options_t alice_link_options = {
        .timeout_seconds = 2,
        .state_callback = state_changed,
        .packet_callback = packet_received,
        .callback_context = &alice_observation};
    rns_runtime_link_t *outbound = NULL;
    assert(rns_runtime_link_open(alice, bob_destination, &bob_identity,
                                 &alice_link_options, &outbound) == RNS_OK);
    for (size_t attempt = 0; attempt < 4000 &&
         (rns_runtime_link_state(outbound) != RNS_LINK_ACTIVE ||
          bob_observation.accepted == NULL ||
          rns_runtime_link_state(bob_observation.accepted) != RNS_LINK_ACTIVE);
         ++attempt)
        poll_three(alice, transport, bob);
    assert(rns_runtime_link_state(outbound) == RNS_LINK_ACTIVE);
    assert(bob_observation.accepted != NULL && alice_observation.active == 1 &&
           bob_observation.active == 1);

    rns_runtime_interface_info_t before[2], after[2];
    assert(rns_runtime_interface_count(transport) == 2);
    assert(rns_runtime_interface_info(transport, 0, &before[0]) == RNS_OK);
    assert(rns_runtime_interface_info(transport, 1, &before[1]) == RNS_OK);
    static const uint8_t hello[] = "through one hop";
    assert(rns_runtime_link_send(outbound, 0x31, hello, sizeof hello - 1) ==
           RNS_OK);
    for (size_t attempt = 0; attempt < 2000 && bob_observation.packets == 0;
         ++attempt)
        poll_three(alice, transport, bob);
    assert(bob_observation.packets == 1 &&
           bob_observation.payload_length == sizeof hello - 1 &&
           memcmp(bob_observation.payload, hello, sizeof hello - 1) == 0);
    assert(rns_runtime_interface_info(transport, 0, &after[0]) == RNS_OK);
    assert(rns_runtime_interface_info(transport, 1, &after[1]) == RNS_OK);
    assert(after[0].packets_sent == before[0].packets_sent);
    assert(after[1].packets_sent == before[1].packets_sent + 1);

    static const uint8_t reply[] = "and back";
    assert(rns_runtime_link_send(bob_observation.accepted, 0x31, reply,
                                 sizeof reply - 1) == RNS_OK);
    for (size_t attempt = 0; attempt < 2000 && alice_observation.packets == 0;
         ++attempt)
        poll_three(bob, transport, alice);
    assert(alice_observation.packets == 1 &&
           alice_observation.payload_length == sizeof reply - 1 &&
           memcmp(alice_observation.payload, reply, sizeof reply - 1) == 0);

    rns_runtime_link_destroy(outbound);
    rns_runtime_link_destroy(bob_observation.accepted);
    rns_runtime_destination_destroy(registration);
    rns_runtime_destroy(alice);
    rns_runtime_destroy(transport);
    rns_runtime_destroy(bob);
    return 0;
}
