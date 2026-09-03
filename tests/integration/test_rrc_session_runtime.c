#include "reticulum/config.h"
#include "reticulum/destination.h"
#include "reticulum/rrc_session.h"
#include "reticulum/udp.h"

#include "../fixtures/nomadnet_rrc_vectors.h"

#include <assert.h>
#include <stdbool.h>
#include <string.h>

typedef struct hub_fixture {
    rns_runtime_link_t *link;
    size_t accepted;
    size_t identified;
    size_t hello;
    size_t join;
    size_t part;
    size_t message;
    size_t ping;
    size_t pong;
    uint64_t expected_timestamp_ms;
    bool send_welcome;
} hub_fixture_t;

typedef struct client_fixture {
    size_t state_changes;
    size_t envelopes;
} client_fixture_t;

typedef struct clock_fixture {
    uint64_t now_ms;
    bool fail;
} clock_fixture_t;

static rns_status_t fixture_wallclock(uint64_t *milliseconds, void *opaque) {
    clock_fixture_t *clock = opaque;
    if (milliseconds == NULL || clock == NULL)
        return RNS_ERROR_INVALID_ARGUMENT;
    if (clock->fail) return RNS_ERROR_IO;
    *milliseconds = clock->now_ms;
    return RNS_OK;
}

static uint16_t reserve_udp_port(void) {
    rns_udp_endpoint_t *endpoint = NULL;
    rns_udp_address_t address;
    assert(rns_udp_endpoint_create(&endpoint, RNS_UDP_IPV4) == RNS_OK);
    assert(rns_udp_bind(endpoint, "127.0.0.1", 0u) == RNS_OK);
    assert(rns_udp_local_address(endpoint, &address) == RNS_OK);
    rns_udp_endpoint_destroy(endpoint);
    return address.port;
}

static void configure_udp(rns_config_t *config, const char *name,
                          uint16_t listen_port, uint16_t forward_port) {
    rns_config_init(config);
    config->interface_count = 1u;
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

static void hub_link_state(rns_runtime_link_t *link, rns_link_state state,
                           rns_status_t reason, void *opaque) {
    (void)link;
    (void)state;
    (void)reason;
    (void)opaque;
}

static void hub_identified(rns_runtime_link_t *link,
                           const rns_identity *identity, void *opaque) {
    hub_fixture_t *hub = opaque;
    assert(link == hub->link);
    assert(identity != NULL);
    hub->identified++;
}

static void hub_packet(rns_runtime_link_t *link, uint8_t context,
                       const uint8_t *plaintext, size_t plaintext_length,
                       void *opaque) {
    hub_fixture_t *hub = opaque;
    rns_rrc_envelope_t envelope;
    assert(link == hub->link && context == 0u);
    assert(rns_rrc_envelope_parse(plaintext, plaintext_length, &envelope) ==
           RNS_OK);
    assert(envelope.timestamp_ms == hub->expected_timestamp_ms);
    if (envelope.type == RNS_RRC_HELLO) {
        assert(envelope.body_cbor.length ==
               sizeof nomadnet_rrc_session_hello_body);
        assert(memcmp(envelope.body_cbor.data,
                      nomadnet_rrc_session_hello_body,
                      sizeof nomadnet_rrc_session_hello_body) == 0);
        assert(envelope.nick.length == 3u);
        assert(memcmp(envelope.nick.data, "Rei", 3u) == 0);
        assert(hub->identified == hub->accepted);
        hub->hello++;
        if (hub->send_welcome) {
            const nomadnet_rrc_fixture *welcome = &nomadnet_rrc_fixtures[1];
            assert(rns_runtime_link_send(link, 0u, welcome->wire,
                                         welcome->wire_len) == RNS_OK);
            const nomadnet_rrc_fixture *motd = &nomadnet_rrc_fixtures[7];
            assert(rns_runtime_link_send(link, 0u, motd->wire,
                                         motd->wire_len) == RNS_OK);
        }
    } else if (envelope.type == RNS_RRC_JOIN) {
        assert(envelope.room.length == 5u);
        assert(memcmp(envelope.room.data, "lobby", 5u) == 0);
        hub->join++;
        const nomadnet_rrc_fixture *joined = &nomadnet_rrc_fixtures[3];
        assert(rns_runtime_link_send(link, 0u, joined->wire,
                                     joined->wire_len) == RNS_OK);
    } else if (envelope.type == RNS_RRC_PART) {
        hub->part++;
        const nomadnet_rrc_fixture *parted = &nomadnet_rrc_fixtures[5];
        assert(rns_runtime_link_send(link, 0u, parted->wire,
                                     parted->wire_len) == RNS_OK);
    }
    else if (envelope.type == RNS_RRC_MESSAGE) hub->message++;
    else if (envelope.type == RNS_RRC_PING) hub->ping++;
    else if (envelope.type == RNS_RRC_PONG) hub->pong++;
}

static void hub_accepted(rns_runtime_destination_t *destination,
                         rns_runtime_link_t *link, void *opaque) {
    hub_fixture_t *hub = opaque;
    assert(destination != NULL && link != NULL);
    hub->link = link;
    hub->accepted++;
}

static void client_state(rns_rrc_session_t *session,
                         const rns_rrc_session_info_t *info, void *opaque) {
    client_fixture_t *client = opaque;
    assert(session != NULL && info != NULL);
    client->state_changes++;
}

static void client_envelope(rns_rrc_session_t *session,
                            const rns_rrc_envelope_t *envelope,
                            void *opaque) {
    client_fixture_t *client = opaque;
    assert(session != NULL && envelope != NULL);
    client->envelopes++;
}

int main(void) {
    uint16_t client_port = reserve_udp_port();
    uint16_t hub_port = reserve_udp_port();
    assert(client_port != hub_port);
    rns_config_t client_config, hub_config;
    configure_udp(&client_config, "rrc-client", client_port, hub_port);
    configure_udp(&hub_config, "rrc-hub", hub_port, client_port);
    rns_runtime_t *client_runtime = NULL, *hub_runtime = NULL;
    assert(rns_runtime_create(&client_runtime, &client_config, NULL) == RNS_OK);
    assert(rns_runtime_create(&hub_runtime, &hub_config, NULL) == RNS_OK);

    rns_identity client_identity, hub_identity;
    assert(rns_identity_generate(&client_identity));
    assert(rns_identity_generate(&hub_identity));
    static const char *const aspects[] = {"hub"};
    uint8_t hub_destination[16];
    assert(rns_destination_hash(&hub_identity, "rrc", aspects, 1u,
                                hub_destination));

    clock_fixture_t clock = {.now_ms = UINT64_C(1700000000123)};
    hub_fixture_t hub = {
        .expected_timestamp_ms = clock.now_ms,
        .send_welcome = true};
    rns_runtime_link_options_t hub_options = {
        .timeout_seconds = 2.0,
        .state_callback = hub_link_state,
        .packet_callback = hub_packet,
        .identified_callback = hub_identified,
        .callback_context = &hub};
    rns_runtime_destination_t *registration = NULL;
    assert(rns_runtime_register_link_destination(
               hub_runtime, hub_destination, &hub_identity, &hub_options,
               hub_accepted, &hub, &registration) == RNS_OK);
    assert(rns_runtime_announce(hub_runtime, &hub_identity, "rrc", aspects,
                                1u, NULL, 0u) == RNS_OK);

    client_fixture_t client = {0};
    rns_rrc_session_options_t options = {
        .runtime = client_runtime,
        .local_identity = &client_identity,
        .hub_identity = &hub_identity,
        .nick = (const uint8_t *)"Rei",
        .nick_length = 3u,
        .auto_reconnect = true,
        .path_timeout_ms = 5000u,
        .hello_interval_ms = 3000u,
        .hello_max_attempts = 5u,
        .link_timeout_seconds = 2.0,
        .state_callback = client_state,
        .envelope_callback = client_envelope,
        .callback_context = &client,
        .wallclock_callback = fixture_wallclock,
        .wallclock_context = &clock};
    memcpy(options.hub_destination, hub_destination, sizeof hub_destination);
    rns_rrc_session_t *session = NULL;
    assert(rns_rrc_session_create(&session, &options) == RNS_OK);
    assert(rns_rrc_session_connect(session, 1000u) == RNS_OK);

    rns_rrc_session_info_t info;
    size_t processed = 0u;
    for (size_t attempt = 0u; attempt < 2000u; ++attempt) {
        assert(rns_runtime_poll(client_runtime, 8u, &processed) == RNS_OK);
        assert(rns_runtime_poll(hub_runtime, 8u, &processed) == RNS_OK);
        assert(rns_rrc_session_poll(session, 1000u + attempt) == RNS_OK);
        rns_rrc_session_get_info(session, &info);
        if (info.state == RNS_RRC_SESSION_CONNECTED) break;
    }
    rns_rrc_session_get_info(session, &info);
    assert(info.state == RNS_RRC_SESSION_CONNECTED);
    assert(hub.accepted == 1u && hub.identified == 1u && hub.hello == 1u);
    assert(client.envelopes >= 2u);
    assert(info.welcome.resource_envelopes);
    assert(info.welcome.max_message_bytes == 350u);
    assert(info.motd_length != 0u);
    assert(memcmp(info.motd, "Registered public rooms", 23u) == 0);

    assert(rns_rrc_session_join(session, (const uint8_t *)" LOBBY ", 7u,
                                (const uint8_t *)"secret", 6u) == RNS_OK);
    rns_rrc_room_info_t room_info;
    assert(rns_rrc_session_room_count(session) == 1u);
    assert(rns_rrc_session_room_snapshot(session, 0u, &room_info) == RNS_OK);
    assert(room_info.desired && room_info.join_pending && !room_info.joined);
    for (size_t attempt = 0u; attempt < 1000u; ++attempt) {
        assert(rns_runtime_poll(hub_runtime, 8u, &processed) == RNS_OK);
        assert(rns_runtime_poll(client_runtime, 8u, &processed) == RNS_OK);
        assert(rns_rrc_session_room_snapshot(session, 0u, &room_info) ==
               RNS_OK);
        if (room_info.joined) break;
    }
    assert(room_info.joined && !room_info.join_pending);
    assert(room_info.member_count == 3u);
    uint8_t member[RNS_RRC_SOURCE_SIZE];
    assert(rns_rrc_session_member_snapshot(session, 0u, 2u, member) ==
           RNS_OK);
    assert(memcmp(member, client_identity.hash, sizeof member) == 0);
    uint8_t message_id[8];
    assert(rns_rrc_session_send_message(
               session, (const uint8_t *)"lobby", 5u,
               (const uint8_t *)"hello", 5u, message_id) == RNS_OK);
    uint8_t nonce[8];
    assert(rns_rrc_session_ping(session, nonce) == RNS_OK);
    for (size_t attempt = 0u; attempt < 1000u &&
         (hub.join == 0u || hub.message == 0u || hub.ping == 0u); ++attempt)
        assert(rns_runtime_poll(hub_runtime, 8u, &processed) == RNS_OK);
    assert(hub.join == 1u && hub.message == 1u && hub.ping == 1u);

    /* A hub PING is answered with an exact PONG body. */
    const nomadnet_rrc_fixture *ping = &nomadnet_rrc_fixtures[8];
    assert(rns_runtime_link_send(hub.link, 0u, ping->wire, ping->wire_len) ==
           RNS_OK);
    for (size_t attempt = 0u; attempt < 1000u && hub.pong == 0u; ++attempt) {
        assert(rns_runtime_poll(client_runtime, 8u, &processed) == RNS_OK);
        assert(rns_runtime_poll(hub_runtime, 8u, &processed) == RNS_OK);
    }
    assert(hub.pong == 1u);

    /* Clock-provider failures are surfaced without changing desired rooms. */
    clock.fail = true;
    assert(rns_rrc_session_part(session, (const uint8_t *)"lobby", 5u) ==
           RNS_ERROR_IO);
    clock.fail = false;

    /* Unexpected closure follows the pinned two-second first reconnect delay. */
    rns_runtime_link_destroy(hub.link);
    hub.link = NULL;
    for (size_t attempt = 0u; attempt < 1000u; ++attempt) {
        assert(rns_runtime_poll(client_runtime, 8u, &processed) == RNS_OK);
        assert(rns_rrc_session_poll(session, 3000u) == RNS_OK);
        rns_rrc_session_get_info(session, &info);
        if (info.state == RNS_RRC_SESSION_RECONNECT_WAIT) break;
    }
    assert(info.state == RNS_RRC_SESSION_RECONNECT_WAIT);
    assert(info.reconnect_attempts == 1u && info.next_action_ms == 5000u);
    assert(rns_rrc_session_room_snapshot(session, 0u, &room_info) == RNS_OK);
    assert(room_info.desired && !room_info.joined &&
           room_info.member_count == 0u);

    /* A successful reconnect restores every desired room after WELCOME. */
    assert(rns_rrc_session_poll(session, 5000u) == RNS_OK);
    for (size_t attempt = 0u; attempt < 2000u; ++attempt) {
        assert(rns_runtime_poll(client_runtime, 8u, &processed) == RNS_OK);
        assert(rns_runtime_poll(hub_runtime, 8u, &processed) == RNS_OK);
        assert(rns_rrc_session_poll(session, 5000u + attempt) == RNS_OK);
        rns_rrc_session_get_info(session, &info);
        assert(rns_rrc_session_room_snapshot(session, 0u, &room_info) ==
               RNS_OK);
        if (info.state == RNS_RRC_SESSION_CONNECTED && hub.join >= 2u &&
            room_info.joined)
            break;
    }
    assert(info.state == RNS_RRC_SESSION_CONNECTED && hub.join == 2u);
    assert(rns_rrc_session_room_snapshot(session, 0u, &room_info) == RNS_OK);
    assert(room_info.joined && room_info.desired);

    assert(rns_rrc_session_part(session, (const uint8_t *)"lobby", 5u) ==
           RNS_OK);
    for (size_t attempt = 0u; attempt < 1000u &&
         rns_rrc_session_room_count(session) != 0u; ++attempt) {
        assert(rns_runtime_poll(hub_runtime, 8u, &processed) == RNS_OK);
        assert(rns_runtime_poll(client_runtime, 8u, &processed) == RNS_OK);
    }
    assert(hub.part == 1u && rns_rrc_session_room_count(session) == 0u);
    rns_rrc_session_disconnect(session);
    rns_rrc_session_get_info(session, &info);
    assert(info.state == RNS_RRC_SESSION_DISCONNECTED);

    /* A live hub that never welcomes receives exactly five HELLO attempts. */
    hub.send_welcome = false;
    assert(rns_rrc_session_connect(session, 6000u) == RNS_OK);
    uint64_t clock_ms = 6000u;
    for (size_t attempt = 0u; attempt < 2000u; ++attempt) {
        assert(rns_runtime_poll(client_runtime, 8u, &processed) == RNS_OK);
        assert(rns_runtime_poll(hub_runtime, 8u, &processed) == RNS_OK);
        rns_status_t polled = rns_rrc_session_poll(session, clock_ms);
        assert(polled == RNS_OK || polled == RNS_ERROR_TIMEOUT);
        rns_rrc_session_get_info(session, &info);
        if (info.state == RNS_RRC_SESSION_HELLO &&
            info.next_action_ms > clock_ms)
            clock_ms = info.next_action_ms;
        if (info.state == RNS_RRC_SESSION_FAILED) break;
        ++clock_ms;
    }
    assert(info.state == RNS_RRC_SESSION_FAILED);
    assert(info.last_error == RNS_ERROR_TIMEOUT);
    assert(info.hello_attempts == 5u);
    /* Artificially advancing the caller clock can outrun loopback delivery;
     * the session counter still records all five successful link sends. */

    rns_rrc_session_destroy(session);
    rns_runtime_link_destroy(hub.link);
    rns_runtime_destination_destroy(registration);
    rns_runtime_destroy(client_runtime);
    rns_runtime_destroy(hub_runtime);
    return 0;
}
