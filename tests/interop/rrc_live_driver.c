#define _POSIX_C_SOURCE 200809L

#include "reticulum/config.h"
#include "reticulum/destination.h"
#include "reticulum/hal.h"
#include "reticulum/rrc_session.h"

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct live_state {
    uint8_t hub_source[RNS_RRC_SOURCE_SIZE];
    uint8_t ping_nonce[8];
    bool ping_sent;
    bool invalid;
    bool welcome;
    bool joined;
    bool message;
    bool pong;
    bool parted;
    size_t envelope_count;
} live_state_t;

static bool parse_port(const char *text, uint16_t *port) {
    char *end = NULL;
    errno = 0;
    unsigned long value = strtoul(text, &end, 10);
    if (errno != 0 || text[0] == '\0' || end == NULL || *end != '\0' ||
        value == 0u || value > UINT16_MAX) return false;
    *port = (uint16_t)value;
    return true;
}

static bool parse_hex(const char *text, uint8_t *output, size_t length) {
    if (text == NULL || strlen(text) != length * 2u) return false;
    for (size_t i = 0u; i < length; ++i) {
        unsigned value = 0u;
        for (size_t half = 0u; half < 2u; ++half) {
            char byte = text[i * 2u + half];
            unsigned nibble;
            if (byte >= '0' && byte <= '9') nibble = (unsigned)(byte - '0');
            else if (byte >= 'a' && byte <= 'f')
                nibble = (unsigned)(byte - 'a') + 10u;
            else if (byte >= 'A' && byte <= 'F')
                nibble = (unsigned)(byte - 'A') + 10u;
            else return false;
            value = (value << 4u) | nibble;
        }
        output[i] = (uint8_t)value;
    }
    return true;
}

static void print_hex(const uint8_t *value, size_t length) {
    for (size_t i = 0u; i < length; ++i) (void)printf("%02x", value[i]);
}

static bool nonzero_id(const uint8_t id[RNS_RRC_MESSAGE_ID_SIZE]) {
    uint8_t combined = 0u;
    for (size_t i = 0u; i < RNS_RRC_MESSAGE_ID_SIZE; ++i) combined |= id[i];
    return combined != 0u;
}

static bool span_equals(rns_rrc_slice_t span, const char *text) {
    size_t length = strlen(text);
    return span.length == length && memcmp(span.data, text, length) == 0;
}

static void envelope_received(rns_rrc_session_t *session,
                              const rns_rrc_envelope_t *envelope,
                              void *opaque) {
    live_state_t *state = opaque;
    (void)session;
    state->envelope_count++;
    if (envelope->version != RNS_RRC_VERSION || envelope->timestamp_ms == 0u ||
        !nonzero_id(envelope->message_id) ||
        memcmp(envelope->source, state->hub_source,
               sizeof state->hub_source) != 0) {
        state->invalid = true;
        return;
    }
    switch (envelope->type) {
        case RNS_RRC_WELCOME:
            if (envelope->room.length != 0u || envelope->nick.length != 0u ||
                envelope->body_cbor.length == 0u) state->invalid = true;
            else state->welcome = true;
            break;
        case RNS_RRC_JOINED:
            if (!span_equals(envelope->room, "lobby") ||
                envelope->nick.length != 0u || envelope->body_cbor.length == 0u)
                state->invalid = true;
            else state->joined = true;
            break;
        case RNS_RRC_MESSAGE: {
            rns_rrc_slice_t text = {0};
            if (!span_equals(envelope->room, "lobby") ||
                !span_equals(envelope->nick, "PythonHub") ||
                rns_rrc_cbor_text_parse(envelope->body_cbor.data,
                    envelope->body_cbor.length, &text) != RNS_OK ||
                !span_equals(text, "python-to-c")) state->invalid = true;
            else state->message = true;
            break;
        }
        case RNS_RRC_PONG:
            if (!state->ping_sent || envelope->room.length != 0u ||
                envelope->nick.length != 0u ||
                envelope->body_cbor.length != 9u ||
                envelope->body_cbor.data[0] != 0x48u ||
                memcmp(envelope->body_cbor.data + 1u, state->ping_nonce,
                       sizeof state->ping_nonce) != 0) state->invalid = true;
            else state->pong = true;
            break;
        case RNS_RRC_PARTED:
            if (!span_equals(envelope->room, "lobby") ||
                envelope->nick.length != 0u || envelope->body_cbor.length == 0u)
                state->invalid = true;
            else state->parted = true;
            break;
        default:
            state->invalid = true;
            break;
    }
}

static bool poll_until(rns_runtime_t *runtime, rns_rrc_session_t *session,
                       live_state_t *state, bool *condition,
                       uint64_t timeout_ms) {
    uint64_t start = 0u, now = 0u;
    if (rns_hal_monotonic_ms(&start) != RNS_OK) return false;
    do {
        size_t processed = 0u;
        if (rns_runtime_poll(runtime, 32u, &processed) != RNS_OK ||
            rns_hal_monotonic_ms(&now) != RNS_OK) return false;
        rns_status_t status = rns_rrc_session_poll(session, now);
        if (status != RNS_OK && status != RNS_ERROR_TIMEOUT) return false;
        if (*condition || state->invalid) return *condition && !state->invalid;
        (void)rns_hal_sleep_ms(5u);
    } while (now - start < timeout_ms);
    return false;
}

int main(int argc, char **argv) {
    uint16_t listen_port = 0u, forward_port = 0u;
    uint8_t hub_destination[RNS_TRUNCATED_HASH_SIZE];
    uint8_t hub_public[RNS_IDENTITY_PUBLIC_SIZE];
    if (argc != 5 || !parse_port(argv[1], &listen_port) ||
        !parse_port(argv[2], &forward_port) ||
        !parse_hex(argv[3], hub_destination, sizeof hub_destination) ||
        !parse_hex(argv[4], hub_public, sizeof hub_public)) return 2;
    setvbuf(stdout, NULL, _IOLBF, 0);

    rns_identity local, hub;
    if (!rns_identity_generate(&local) ||
        !rns_identity_from_public(&hub, hub_public)) return 3;
    static const char *const aspects[] = {"hub"};
    uint8_t expected[RNS_TRUNCATED_HASH_SIZE];
    if (!rns_destination_hash(&hub, "rrc", aspects, 1u, expected) ||
        memcmp(expected, hub_destination, sizeof expected) != 0) return 4;

    rns_config_t config;
    rns_config_init(&config);
    config.interface_count = 1u;
    rns_config_interface_t *interface = &config.interfaces[0];
    (void)strcpy(interface->name, "pinned-rrc-live");
    interface->type = RNS_CONFIG_UDP;
    interface->type_set = true;
    interface->enabled = true;
    (void)strcpy(interface->listen_ip, "127.0.0.1");
    (void)strcpy(interface->forward_ip, "127.0.0.1");
    interface->listen_port = listen_port;
    interface->forward_port = forward_port;
    rns_runtime_t *runtime = NULL;
    if (rns_runtime_create(&runtime, &config, NULL) != RNS_OK) return 5;

    live_state_t state = {0};
    memcpy(state.hub_source, hub.hash, sizeof state.hub_source);
    rns_rrc_session_options_t options = {
        .runtime = runtime,
        .local_identity = &local,
        .hub_identity = &hub,
        .nick = (const uint8_t *)"Rei",
        .nick_length = 3u,
        .auto_reconnect = false,
        .path_timeout_ms = 10000u,
        .hello_interval_ms = 3000u,
        .hello_max_attempts = 5u,
        .link_timeout_seconds = 10.0,
        .envelope_callback = envelope_received,
        .callback_context = &state};
    memcpy(options.hub_destination, hub_destination, sizeof hub_destination);
    rns_rrc_session_t *session = NULL;
    if (rns_rrc_session_create(&session, &options) != RNS_OK) {
        rns_runtime_destroy(runtime);
        return 6;
    }
    uint64_t now = 0u;
    if (rns_hal_monotonic_ms(&now) != RNS_OK ||
        rns_rrc_session_connect(session, now) != RNS_OK) {
        rns_rrc_session_destroy(session);
        rns_runtime_destroy(runtime);
        return 7;
    }

    bool connected = false;
    uint64_t start = now;
    do {
        size_t processed = 0u;
        if (rns_runtime_poll(runtime, 32u, &processed) != RNS_OK ||
            rns_hal_monotonic_ms(&now) != RNS_OK ||
            rns_rrc_session_poll(session, now) != RNS_OK) break;
        rns_rrc_session_info_t info;
        rns_rrc_session_get_info(session, &info);
        connected = info.state == RNS_RRC_SESSION_CONNECTED;
        if (connected || state.invalid) break;
        (void)rns_hal_sleep_ms(5u);
    } while (now - start < 20000u);

    rns_rrc_session_info_t info;
    rns_rrc_session_get_info(session, &info);
    static const char expected_hub_name[] = "Pinned fixture hub";
    static const char expected_hub_version[] = "NomadNet-1.2.0-schema";
    bool welcome_valid = connected && state.welcome &&
        info.welcome.resource_envelopes &&
        info.welcome.hub_name_length == sizeof expected_hub_name - 1u &&
        memcmp(info.welcome.hub_name, expected_hub_name,
               sizeof expected_hub_name - 1u) == 0 &&
        info.welcome.hub_version_length == sizeof expected_hub_version - 1u &&
        memcmp(info.welcome.hub_version, expected_hub_version,
               sizeof expected_hub_version - 1u) == 0 &&
        info.welcome.max_nick_bytes == 32u &&
        info.welcome.max_room_bytes == 64u &&
        info.welcome.max_message_bytes == 350u &&
        info.welcome.max_rooms == 32u && info.welcome.rate_per_minute == 240u;
    uint8_t message_id[8] = {0};
    if (!welcome_valid ||
        rns_rrc_session_join(session, (const uint8_t *)" LOBBY ", 7u,
            (const uint8_t *)"synthetic-key", 13u) != RNS_OK ||
        rns_rrc_session_send_message(session, (const uint8_t *)"lobby", 5u,
            (const uint8_t *)"c-to-python", 11u, message_id) != RNS_OK ||
        rns_rrc_session_ping(session, state.ping_nonce) != RNS_OK) {
        state.invalid = true;
    } else {
        state.ping_sent = true;
    }

    bool responses = false;
    if (!state.invalid) {
        uint64_t response_start = 0u;
        (void)rns_hal_monotonic_ms(&response_start);
        do {
            size_t processed = 0u;
            if (rns_runtime_poll(runtime, 32u, &processed) != RNS_OK ||
                rns_hal_monotonic_ms(&now) != RNS_OK ||
                rns_rrc_session_poll(session, now) != RNS_OK) break;
            responses = state.joined && state.message && state.pong;
            if (responses || state.invalid) break;
            (void)rns_hal_sleep_ms(5u);
        } while (now - response_start < 10000u);
    }
    if (responses &&
        rns_rrc_session_part(session, (const uint8_t *)"lobby", 5u) == RNS_OK)
        (void)poll_until(runtime, session, &state, &state.parted, 10000u);
    else state.invalid = true;
    rns_rrc_session_disconnect(session);
    rns_rrc_session_get_info(session, &info);
    bool success = !state.invalid && welcome_valid && responses && state.parted &&
                   info.state == RNS_RRC_SESSION_DISCONNECTED &&
                   state.envelope_count == 5u && nonzero_id(message_id);
    (void)printf("{\"ok\":%s,\"connected\":%s,\"identified_flow\":%s,"
                 "\"joined\":%s,\"message_received\":%s,\"pong\":%s,"
                 "\"parted\":%s,\"envelopes\":%zu,\"message_id\":\"",
                 success ? "true" : "false", connected ? "true" : "false",
                 welcome_valid ? "true" : "false", state.joined ? "true" : "false",
                 state.message ? "true" : "false", state.pong ? "true" : "false",
                 state.parted ? "true" : "false", state.envelope_count);
    print_hex(message_id, sizeof message_id);
    (void)puts("\"}");
    rns_rrc_session_destroy(session);
    rns_runtime_destroy(runtime);
    return success ? 0 : 1;
}
