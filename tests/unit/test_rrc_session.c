#include "reticulum/destination.h"
#include "reticulum/config.h"
#include "reticulum/rrc_session.h"

#include "../fixtures/nomadnet_rrc_vectors.h"

#include <assert.h>
#include <string.h>

int main(void) {
    const nomadnet_rrc_fixture *welcome_fixture =
        &nomadnet_rrc_fixtures[1];
    rns_rrc_welcome_t welcome;
    assert(rns_rrc_welcome_parse(welcome_fixture->body,
                                 welcome_fixture->body_len,
                                 &welcome) == RNS_OK);
    static const uint8_t expected_name[] = "hub \xf0\x9f\x8c\xb8";
    assert(welcome.hub_name_length == sizeof expected_name - 1u);
    assert(memcmp(welcome.hub_name, expected_name,
                  sizeof expected_name - 1u) == 0);
    assert(welcome.hub_version_length == 5u);
    assert(memcmp(welcome.hub_version, "1.2.0", 5u) == 0);
    assert(welcome.resource_envelopes);
    assert(welcome.max_nick_bytes == 32u);
    assert(welcome.max_room_bytes == 64u);
    assert(welcome.max_message_bytes == 350u);
    assert(welcome.max_rooms == 32u);
    assert(welcome.rate_per_minute == 240u);

    static const uint8_t defaults[] = {0xa0u};
    assert(rns_rrc_welcome_parse(defaults, sizeof defaults, &welcome) ==
           RNS_OK);
    assert(welcome.max_nick_bytes == RNS_RRC_DEFAULT_MAX_NICK_BYTES);
    assert(welcome.max_room_bytes == RNS_RRC_DEFAULT_MAX_ROOM_BYTES);
    assert(welcome.max_message_bytes == RNS_RRC_DEFAULT_MAX_MESSAGE_BYTES);
    assert(welcome.max_rooms == RNS_RRC_DEFAULT_MAX_ROOMS);
    assert(welcome.rate_per_minute == RNS_RRC_DEFAULT_RATE_PER_MINUTE);

    static const uint8_t invalid[] = {0xbf, 0xff};
    assert(rns_rrc_welcome_parse(invalid, sizeof invalid, &welcome) ==
           RNS_ERROR_PROTOCOL);
    static const uint8_t invalid_utf8[] = {0xa1u, 0x00u, 0x61u, 0xffu};
    assert(rns_rrc_welcome_parse(invalid_utf8, sizeof invalid_utf8,
                                 &welcome) == RNS_ERROR_PROTOCOL);
    assert(rns_rrc_welcome_parse(NULL, 0u, &welcome) ==
           RNS_ERROR_INVALID_ARGUMENT);

    /* Destination names are authenticated before a session is allocated and
     * a missing route concludes deterministically on the caller's clock. */
    rns_config_t config;
    rns_config_init(&config);
    rns_runtime_t *runtime = NULL;
    assert(rns_runtime_create(&runtime, &config, NULL) == RNS_OK);
    rns_identity local, hub;
    assert(rns_identity_generate(&local));
    assert(rns_identity_generate(&hub));
    static const char *const aspects[] = {"hub"};
    rns_rrc_session_options_t options = {
        .runtime = runtime,
        .local_identity = &local,
        .hub_identity = &hub,
        .path_timeout_ms = 25u};
    assert(rns_destination_hash(&hub, "rrc", aspects, 1u,
                                options.hub_destination));
    rns_rrc_session_t *session = NULL;
    assert(rns_rrc_session_create(&session, &options) == RNS_OK);
    assert(rns_rrc_session_connect(session, 100u) == RNS_OK);
    assert(rns_rrc_session_poll(session, 124u) == RNS_OK);
    assert(rns_rrc_session_poll(session, 125u) == RNS_ERROR_TIMEOUT);
    rns_rrc_session_info_t info;
    rns_rrc_session_get_info(session, &info);
    assert(info.state == RNS_RRC_SESSION_FAILED);
    assert(info.last_error == RNS_ERROR_TIMEOUT);
    rns_rrc_session_destroy(session);

    options.hub_destination[0] ^= 1u;
    assert(rns_rrc_session_create(&session, &options) ==
           RNS_ERROR_INVALID_ARGUMENT);
    options.hub_destination[0] ^= 1u;
    static const uint8_t invalid_nick[] = {0xffu};
    options.nick = invalid_nick;
    options.nick_length = sizeof invalid_nick;
    assert(rns_rrc_session_create(&session, &options) ==
           RNS_ERROR_INVALID_ARGUMENT);
    rns_runtime_destroy(runtime);
    return 0;
}
