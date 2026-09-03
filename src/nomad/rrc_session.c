#include "reticulum/rrc_session.h"

#include "reticulum/destination.h"
#include "reticulum/hal.h"
#include "reticulum/packet.h"

#include <limits.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define RRC_CBOR_MAX_DEPTH 8u
#define RRC_CBOR_MAX_ITEMS 256u

typedef struct cbor_reader {
    const uint8_t *cursor;
    const uint8_t *end;
    size_t items;
} cbor_reader_t;

struct rns_rrc_session {
    rns_rrc_session_options_t options;
    rns_identity local;
    rns_identity hub;
    uint8_t nick[RNS_RRC_MAX_NICK_BYTES];
    rns_runtime_link_t *link;
    rns_rrc_session_info_t info;
    uint64_t deadline_ms;
    uint64_t now_ms;
    bool manual_disconnect;
    bool link_active;
    bool link_closed;
    bool welcome_pending;
};

static void welcome_defaults(rns_rrc_welcome_t *welcome) {
    memset(welcome, 0, sizeof *welcome);
    welcome->max_nick_bytes = RNS_RRC_DEFAULT_MAX_NICK_BYTES;
    welcome->max_room_bytes = RNS_RRC_DEFAULT_MAX_ROOM_BYTES;
    welcome->max_message_bytes = RNS_RRC_DEFAULT_MAX_MESSAGE_BYTES;
    welcome->max_rooms = RNS_RRC_DEFAULT_MAX_ROOMS;
    welcome->rate_per_minute = RNS_RRC_DEFAULT_RATE_PER_MINUTE;
}

static bool cbor_argument(cbor_reader_t *reader, uint8_t additional,
                          uint64_t *value) {
    size_t width = 0u;
    if (additional < 24u) {
        *value = additional;
        return true;
    }
    if (additional == 24u) width = 1u;
    else if (additional == 25u) width = 2u;
    else if (additional == 26u) width = 4u;
    else if (additional == 27u) width = 8u;
    else return false;
    if ((size_t)(reader->end - reader->cursor) < width) return false;
    uint64_t result = 0u;
    for (size_t i = 0u; i < width; ++i)
        result = (result << 8u) | *reader->cursor++;
    if ((width == 1u && result < 24u) ||
        (width == 2u && result <= UINT8_MAX) ||
        (width == 4u && result <= UINT16_MAX) ||
        (width == 8u && result <= UINT32_MAX))
        return false;
    *value = result;
    return true;
}

static bool cbor_head(cbor_reader_t *reader, uint8_t *major,
                      uint64_t *value) {
    if (reader->cursor == reader->end ||
        reader->items++ >= RRC_CBOR_MAX_ITEMS)
        return false;
    uint8_t first = *reader->cursor++;
    *major = first >> 5u;
    return cbor_argument(reader, first & 31u, value);
}

static bool cbor_skip(cbor_reader_t *reader, unsigned depth) {
    uint8_t major = 0u;
    uint64_t value = 0u;
    if (depth > RRC_CBOR_MAX_DEPTH || !cbor_head(reader, &major, &value))
        return false;
    if (major == 0u || major == 1u || major == 7u) return true;
    if (major == 2u || major == 3u) {
        if (value > (uint64_t)(reader->end - reader->cursor)) return false;
        reader->cursor += (size_t)value;
        return true;
    }
    if (major == 4u || major == 5u) {
        uint64_t count = major == 5u ? value * 2u : value;
        if ((major == 5u && value > UINT64_MAX / 2u) ||
            count > RRC_CBOR_MAX_ITEMS)
            return false;
        for (uint64_t i = 0u; i < count; ++i)
            if (!cbor_skip(reader, depth + 1u)) return false;
        return true;
    }
    return major == 6u && cbor_skip(reader, depth + 1u);
}

static bool cbor_uint(cbor_reader_t *reader, uint64_t *value) {
    uint8_t major = 0u;
    return cbor_head(reader, &major, value) && major == 0u;
}

static bool cbor_text(cbor_reader_t *reader, const uint8_t **text,
                      size_t *length) {
    uint8_t major = 0u;
    uint64_t value = 0u;
    if (!cbor_head(reader, &major, &value) || major != 3u ||
        value > (uint64_t)(reader->end - reader->cursor))
        return false;
    *text = reader->cursor;
    *length = (size_t)value;
    reader->cursor += *length;
    return true;
}

static bool valid_utf8_text(const uint8_t *text, size_t length) {
    uint8_t encoded[RNS_RRC_MAX_HUB_NAME_BYTES + 2u];
    size_t encoded_length = 0u;
    return length <= RNS_RRC_MAX_HUB_NAME_BYTES &&
           rns_rrc_cbor_text(text, length, encoded, sizeof encoded,
                             &encoded_length) == RNS_OK;
}

static bool cbor_bool(cbor_reader_t *reader, bool *value) {
    uint8_t major = 0u;
    uint64_t argument = 0u;
    if (!cbor_head(reader, &major, &argument) || major != 7u ||
        (argument != 20u && argument != 21u))
        return false;
    *value = argument == 21u;
    return true;
}

static size_t limited_size(uint64_t value, size_t maximum, size_t fallback) {
    return value > 0u && value <= maximum ? (size_t)value : fallback;
}

static bool parse_caps(cbor_reader_t *reader, rns_rrc_welcome_t *welcome) {
    uint8_t major = 0u;
    uint64_t pairs = 0u;
    if (!cbor_head(reader, &major, &pairs) || major != 5u || pairs > 32u)
        return false;
    for (uint64_t i = 0u; i < pairs; ++i) {
        uint64_t key = 0u;
        if (!cbor_uint(reader, &key)) return false;
        if (key == 0u) {
            bool enabled = false;
            if (!cbor_bool(reader, &enabled)) return false;
            welcome->resource_envelopes = enabled;
        } else if (!cbor_skip(reader, 1u)) return false;
    }
    return true;
}

static bool parse_limits(cbor_reader_t *reader, rns_rrc_welcome_t *welcome) {
    uint8_t major = 0u;
    uint64_t pairs = 0u;
    if (!cbor_head(reader, &major, &pairs) || major != 5u || pairs > 32u)
        return false;
    for (uint64_t i = 0u; i < pairs; ++i) {
        uint64_t key = 0u, value = 0u;
        if (!cbor_uint(reader, &key)) return false;
        if (key > 4u) {
            if (!cbor_skip(reader, 1u)) return false;
            continue;
        }
        if (!cbor_uint(reader, &value)) return false;
        if (key == 0u)
            welcome->max_nick_bytes = limited_size(
                value, RNS_RRC_MAX_NICK_BYTES, welcome->max_nick_bytes);
        else if (key == 1u)
            welcome->max_room_bytes = limited_size(
                value, RNS_RRC_MAX_ROOM_BYTES, welcome->max_room_bytes);
        else if (key == 2u)
            welcome->max_message_bytes = limited_size(
                value, RNS_RRC_MAX_ENVELOPE_SIZE, welcome->max_message_bytes);
        else if (key == 3u)
            welcome->max_rooms = limited_size(value, 1024u,
                                               welcome->max_rooms);
        else
            welcome->rate_per_minute = limited_size(
                value, 1000000u, welcome->rate_per_minute);
    }
    return true;
}

rns_status_t rns_rrc_welcome_parse(const uint8_t *body, size_t body_length,
                                   rns_rrc_welcome_t *welcome) {
    if (body == NULL || body_length == 0u || welcome == NULL ||
        body_length > RNS_RRC_MAX_ENVELOPE_SIZE)
        return RNS_ERROR_INVALID_ARGUMENT;
    welcome_defaults(welcome);
    cbor_reader_t reader = {body, body + body_length, 0u};
    uint8_t major = 0u;
    uint64_t pairs = 0u;
    if (!cbor_head(&reader, &major, &pairs) || major != 5u || pairs > 32u)
        return RNS_ERROR_PROTOCOL;
    for (uint64_t i = 0u; i < pairs; ++i) {
        uint64_t key = 0u;
        if (!cbor_uint(&reader, &key)) return RNS_ERROR_PROTOCOL;
        if (key == 0u || key == 1u) {
            const uint8_t *text = NULL;
            size_t length = 0u;
            if (!cbor_text(&reader, &text, &length)) return RNS_ERROR_PROTOCOL;
            uint8_t *output = key == 0u ? welcome->hub_name
                                        : welcome->hub_version;
            size_t capacity = key == 0u ? sizeof welcome->hub_name
                                        : sizeof welcome->hub_version;
            size_t *output_length = key == 0u ? &welcome->hub_name_length
                                              : &welcome->hub_version_length;
            if (length > capacity) return RNS_ERROR_OVERFLOW;
            if (!valid_utf8_text(text, length)) return RNS_ERROR_PROTOCOL;
            memcpy(output, text, length);
            *output_length = length;
        } else if (key == 2u) {
            if (!parse_caps(&reader, welcome)) return RNS_ERROR_PROTOCOL;
        } else if (key == 3u) {
            if (!parse_limits(&reader, welcome)) return RNS_ERROR_PROTOCOL;
        } else if (!cbor_skip(&reader, 1u)) return RNS_ERROR_PROTOCOL;
    }
    return reader.cursor == reader.end ? RNS_OK : RNS_ERROR_PROTOCOL;
}

static void transition(rns_rrc_session_t *session,
                       rns_rrc_session_state_t state, rns_status_t error) {
    bool changed = session->info.state != state ||
                   session->info.last_error != error;
    session->info.state = state;
    session->info.last_error = error;
    if (changed && session->options.state_callback != NULL)
        session->options.state_callback(session, &session->info,
                                        session->options.callback_context);
}

static void link_state_changed(rns_runtime_link_t *link, rns_link_state state,
                               rns_status_t reason, void *context) {
    rns_rrc_session_t *session = context;
    if (link != session->link) return;
    if (state == RNS_LINK_ACTIVE) session->link_active = true;
    if (state == RNS_LINK_CLOSED) {
        session->link_closed = true;
        if (reason != RNS_OK) session->info.last_error = reason;
    }
}

static rns_status_t send_envelope(rns_rrc_session_t *session,
                                  rns_rrc_message_type_t type,
                                  rns_rrc_slice_t room,
                                  rns_rrc_slice_t body,
                                  bool include_nick,
                                  uint8_t message_id[8]) {
    if (session->link == NULL ||
        rns_runtime_link_state(session->link) != RNS_LINK_ACTIVE)
        return RNS_ERROR_INVALID_STATE;
    rns_rrc_envelope_t envelope = {0};
    envelope.version = RNS_RRC_VERSION;
    envelope.type = type;
    envelope.timestamp_ms = session->now_ms;
    memcpy(envelope.source, session->local.hash, sizeof envelope.source);
    if (rns_hal_random_bytes(envelope.message_id,
                             sizeof envelope.message_id) != RNS_OK)
        return RNS_ERROR_CRYPTO;
    envelope.room = room;
    envelope.body_cbor = body;
    if (include_nick && session->options.nick_length != 0u)
        envelope.nick = (rns_rrc_slice_t){session->nick,
                                         session->options.nick_length};
    uint8_t wire[RNS_MTU];
    size_t wire_length = 0u;
    rns_status_t status = rns_rrc_envelope_encode(
        &envelope, wire, sizeof wire, &wire_length);
    if (status == RNS_OK)
        status = rns_runtime_link_send(session->link, 0u, wire, wire_length);
    if (status == RNS_OK && message_id != NULL)
        memcpy(message_id, envelope.message_id, sizeof envelope.message_id);
    return status;
}

static void packet_received(rns_runtime_link_t *link, uint8_t context,
                            const uint8_t *plaintext, size_t plaintext_length,
                            void *opaque) {
    rns_rrc_session_t *session = opaque;
    if (link != session->link || context != 0u) return;
    rns_rrc_envelope_t envelope;
    if (rns_rrc_envelope_parse(plaintext, plaintext_length, &envelope) != RNS_OK)
        return;
    if (envelope.type == RNS_RRC_WELCOME) {
        rns_rrc_welcome_t parsed;
        if (envelope.body_cbor.length == 0u ||
            rns_rrc_welcome_parse(envelope.body_cbor.data,
                                  envelope.body_cbor.length, &parsed) != RNS_OK)
            welcome_defaults(&parsed);
        session->info.welcome = parsed;
        session->welcome_pending = true;
    } else if (envelope.type == RNS_RRC_PING &&
               envelope.body_cbor.length != 0u) {
        (void)send_envelope(session, RNS_RRC_PONG,
                            (rns_rrc_slice_t){0}, envelope.body_cbor, false,
                            NULL);
    }
    if (session->options.envelope_callback != NULL)
        session->options.envelope_callback(
            session, &envelope, session->options.callback_context);
}

static void cleanup_link(rns_rrc_session_t *session) {
    rns_runtime_link_t *link = session->link;
    session->link = NULL;
    session->link_active = false;
    session->link_closed = false;
    if (link != NULL) rns_runtime_link_destroy(link);
}

rns_status_t rns_rrc_session_create(rns_rrc_session_t **output,
                                    const rns_rrc_session_options_t *options) {
    if (output == NULL || options == NULL || options->runtime == NULL ||
        options->local_identity == NULL ||
        options->hub_identity == NULL ||
        !options->local_identity->has_private ||
        options->nick_length > RNS_RRC_MAX_NICK_BYTES ||
        (options->nick_length != 0u && options->nick == NULL) ||
        options->hello_max_attempts > 32u ||
        !isfinite(options->link_timeout_seconds) ||
        options->link_timeout_seconds < 0.0)
        return RNS_ERROR_INVALID_ARGUMENT;
    *output = NULL;
    static const char *const aspects[] = {"hub"};
    uint8_t expected[RNS_TRUNCATED_HASH_SIZE];
    if (!rns_destination_hash(options->hub_identity, "rrc", aspects, 1u,
                              expected) ||
        memcmp(expected, options->hub_destination, sizeof expected) != 0)
        return RNS_ERROR_INVALID_ARGUMENT;
    rns_rrc_session_t *session = calloc(1u, sizeof *session);
    if (session == NULL) return RNS_ERROR_NO_MEMORY;
    session->options = *options;
    session->local = *options->local_identity;
    uint8_t public_key[RNS_IDENTITY_PUBLIC_SIZE];
    rns_identity_export_public(options->hub_identity, public_key);
    if (!rns_identity_from_public(&session->hub, public_key)) {
        free(session);
        return RNS_ERROR_CRYPTO;
    }
    if (options->nick_length != 0u)
        memcpy(session->nick, options->nick, options->nick_length);
    if (options->nick_length != 0u) {
        uint8_t encoded[RNS_RRC_MAX_NICK_BYTES + 2u];
        size_t encoded_length = 0u;
        if (rns_rrc_cbor_text(session->nick, options->nick_length, encoded,
                              sizeof encoded, &encoded_length) != RNS_OK) {
            rns_hal_secure_zero(session, sizeof *session);
            free(session);
            return RNS_ERROR_INVALID_ARGUMENT;
        }
    }
    session->options.local_identity = &session->local;
    session->options.hub_identity = &session->hub;
    session->options.nick = session->nick;
    if (session->options.path_timeout_ms == 0u)
        session->options.path_timeout_ms = 5000u;
    if (session->options.hello_interval_ms == 0u)
        session->options.hello_interval_ms = 3000u;
    if (session->options.hello_max_attempts == 0u)
        session->options.hello_max_attempts = 5u;
    welcome_defaults(&session->info.welcome);
    *output = session;
    return RNS_OK;
}

void rns_rrc_session_destroy(rns_rrc_session_t *session) {
    if (session == NULL) return;
    session->manual_disconnect = true;
    cleanup_link(session);
    rns_hal_secure_zero(session, sizeof *session);
    free(session);
}

rns_status_t rns_rrc_session_connect(rns_rrc_session_t *session,
                                     uint64_t now_ms) {
    if (session == NULL) return RNS_ERROR_INVALID_ARGUMENT;
    if (session->info.state != RNS_RRC_SESSION_DISCONNECTED &&
        session->info.state != RNS_RRC_SESSION_FAILED &&
        session->info.state != RNS_RRC_SESSION_RECONNECT_WAIT)
        return RNS_ERROR_INVALID_STATE;
    cleanup_link(session);
    session->now_ms = now_ms;
    session->manual_disconnect = false;
    session->welcome_pending = false;
    session->info.hello_attempts = 0u;
    welcome_defaults(&session->info.welcome);
    session->deadline_ms = UINT64_MAX - now_ms < session->options.path_timeout_ms
                               ? UINT64_MAX
                               : now_ms + session->options.path_timeout_ms;
    session->info.next_action_ms = session->deadline_ms;
    transition(session, RNS_RRC_SESSION_DISCOVERING, RNS_OK);
    rns_path_entry path;
    if (rns_runtime_path_lookup(session->options.runtime,
                                session->options.hub_destination,
                                &path) != RNS_OK)
        (void)rns_runtime_request_path(session->options.runtime,
                                      session->options.hub_destination);
    return RNS_OK;
}

static void schedule_reconnect(rns_rrc_session_t *session) {
    cleanup_link(session);
    welcome_defaults(&session->info.welcome);
    session->welcome_pending = false;
    if (!session->options.auto_reconnect || session->manual_disconnect) {
        transition(session, RNS_RRC_SESSION_DISCONNECTED,
                   session->info.last_error);
        return;
    }
    session->info.reconnect_attempts++;
    size_t exponent = session->info.reconnect_attempts;
    if (exponent > 6u) exponent = 6u;
    uint64_t delay = UINT64_C(1000) << exponent;
    if (delay > 60000u) delay = 60000u;
    session->info.next_action_ms = session->now_ms + delay;
    transition(session, RNS_RRC_SESSION_RECONNECT_WAIT,
               session->info.last_error);
}

static rns_status_t send_hello(rns_rrc_session_t *session) {
    static const uint8_t hello[] = {
        0xa3u, 0x00u, 0x68u, 'n', 'o', 'm', 'a', 'd', 'n', 'e', 't',
        0x01u, 0x63u, '0', '.', '1', 0x02u, 0xa1u, 0x00u, 0xf5u};
    rns_status_t status = send_envelope(
        session, RNS_RRC_HELLO, (rns_rrc_slice_t){0},
        (rns_rrc_slice_t){hello, sizeof hello}, true, NULL);
    if (status == RNS_OK) {
        session->info.hello_attempts++;
        session->info.next_action_ms =
            session->now_ms + session->options.hello_interval_ms;
    }
    return status;
}

rns_status_t rns_rrc_session_poll(rns_rrc_session_t *session,
                                  uint64_t now_ms) {
    if (session == NULL) return RNS_ERROR_INVALID_ARGUMENT;
    session->now_ms = now_ms;
    if (session->link_closed) {
        schedule_reconnect(session);
        return RNS_OK;
    }
    if (session->info.state == RNS_RRC_SESSION_RECONNECT_WAIT) {
        if (now_ms >= session->info.next_action_ms)
            return rns_rrc_session_connect(session, now_ms);
        return RNS_OK;
    }
    if (session->info.state == RNS_RRC_SESSION_DISCOVERING) {
        rns_path_entry path;
        if (rns_runtime_path_lookup(session->options.runtime,
                                    session->options.hub_destination,
                                    &path) == RNS_OK) {
            rns_runtime_link_options_t link_options = {
                .timeout_seconds = session->options.link_timeout_seconds,
                .state_callback = link_state_changed,
                .packet_callback = packet_received,
                .callback_context = session};
            rns_status_t status = rns_runtime_link_open(
                session->options.runtime, session->options.hub_destination,
                &session->hub, &link_options, &session->link);
            if (status != RNS_OK) {
                transition(session, RNS_RRC_SESSION_FAILED, status);
                return status;
            }
            transition(session, RNS_RRC_SESSION_LINKING, RNS_OK);
        } else if (now_ms >= session->deadline_ms) {
            transition(session, RNS_RRC_SESSION_FAILED, RNS_ERROR_TIMEOUT);
            return RNS_ERROR_TIMEOUT;
        }
    }
    if (session->info.state == RNS_RRC_SESSION_LINKING &&
        session->link_active) {
        rns_status_t status = rns_runtime_link_identify(session->link,
                                                        &session->local);
        if (status == RNS_OK) status = send_hello(session);
        if (status != RNS_OK) {
            transition(session, RNS_RRC_SESSION_FAILED, status);
            cleanup_link(session);
            return status;
        }
        transition(session, RNS_RRC_SESSION_HELLO, RNS_OK);
    }
    if (session->welcome_pending) {
        session->welcome_pending = false;
        session->info.reconnect_attempts = 0u;
        session->info.next_action_ms = 0u;
        transition(session, RNS_RRC_SESSION_CONNECTED, RNS_OK);
    } else if (session->info.state == RNS_RRC_SESSION_HELLO &&
               now_ms >= session->info.next_action_ms) {
        if (session->info.hello_attempts >=
            session->options.hello_max_attempts) {
            transition(session, RNS_RRC_SESSION_FAILED, RNS_ERROR_TIMEOUT);
            cleanup_link(session);
            return RNS_ERROR_TIMEOUT;
        }
        rns_status_t status = send_hello(session);
        if (status != RNS_OK) {
            transition(session, RNS_RRC_SESSION_FAILED, status);
            cleanup_link(session);
            return status;
        }
    }
    return RNS_OK;
}

void rns_rrc_session_disconnect(rns_rrc_session_t *session) {
    if (session == NULL) return;
    session->manual_disconnect = true;
    session->info.reconnect_attempts = 0u;
    cleanup_link(session);
    session->info.next_action_ms = 0u;
    transition(session, RNS_RRC_SESSION_DISCONNECTED, RNS_OK);
}

void rns_rrc_session_get_info(const rns_rrc_session_t *session,
                              rns_rrc_session_info_t *info) {
    if (session != NULL && info != NULL) *info = session->info;
}

static rns_status_t normalized_room(const rns_rrc_session_t *session,
                                    const uint8_t *input, size_t input_length,
                                    uint8_t output[RNS_RRC_MAX_ROOM_BYTES],
                                    size_t *output_length) {
    if (input == NULL || input_length == 0u || output_length == NULL)
        return RNS_ERROR_INVALID_ARGUMENT;
    size_t first = 0u, last = input_length;
    while (first < last && (input[first] == ' ' || input[first] == '\t' ||
                            input[first] == '\r' || input[first] == '\n'))
        ++first;
    while (last > first &&
           (input[last - 1u] == ' ' || input[last - 1u] == '\t' ||
            input[last - 1u] == '\r' || input[last - 1u] == '\n'))
        --last;
    size_t length = last - first;
    if (length == 0u || length > session->info.welcome.max_room_bytes ||
        length > RNS_RRC_MAX_ROOM_BYTES)
        return RNS_ERROR_OVERFLOW;
    for (size_t i = 0u; i < length; ++i) {
        uint8_t value = input[first + i];
        output[i] = value >= 'A' && value <= 'Z'
                        ? (uint8_t)(value + ('a' - 'A'))
                        : value;
    }
    uint8_t validation[RNS_RRC_MAX_ROOM_BYTES + 2u];
    size_t ignored = 0u;
    if (rns_rrc_cbor_text(output, length, validation, sizeof validation,
                          &ignored) != RNS_OK)
        return RNS_ERROR_INVALID_ARGUMENT;
    *output_length = length;
    return RNS_OK;
}

rns_status_t rns_rrc_session_join(rns_rrc_session_t *session,
                                  const uint8_t *room, size_t room_length,
                                  const uint8_t *key, size_t key_length) {
    if (session == NULL || (key_length != 0u && key == NULL))
        return RNS_ERROR_INVALID_ARGUMENT;
    if (session->info.state != RNS_RRC_SESSION_CONNECTED)
        return RNS_ERROR_INVALID_STATE;
    uint8_t normalized[RNS_RRC_MAX_ROOM_BYTES], body[258u];
    size_t normalized_length = 0u, body_length = 0u;
    rns_status_t status = normalized_room(session, room, room_length,
                                          normalized, &normalized_length);
    if (status == RNS_OK && key_length != 0u)
        status = rns_rrc_cbor_text(key, key_length, body, sizeof body,
                                   &body_length);
    if (status != RNS_OK) return status;
    return send_envelope(
        session, RNS_RRC_JOIN,
        (rns_rrc_slice_t){normalized, normalized_length},
        (rns_rrc_slice_t){body_length ? body : NULL, body_length}, true, NULL);
}

rns_status_t rns_rrc_session_part(rns_rrc_session_t *session,
                                  const uint8_t *room, size_t room_length) {
    if (session == NULL) return RNS_ERROR_INVALID_ARGUMENT;
    if (session->info.state != RNS_RRC_SESSION_CONNECTED)
        return RNS_ERROR_INVALID_STATE;
    uint8_t normalized[RNS_RRC_MAX_ROOM_BYTES];
    size_t normalized_length = 0u;
    rns_status_t status = normalized_room(session, room, room_length,
                                          normalized, &normalized_length);
    if (status != RNS_OK) return status;
    return send_envelope(
        session, RNS_RRC_PART,
        (rns_rrc_slice_t){normalized, normalized_length},
        (rns_rrc_slice_t){0}, false, NULL);
}

rns_status_t rns_rrc_session_send_message(rns_rrc_session_t *session,
                                          const uint8_t *room,
                                          size_t room_length,
                                          const uint8_t *message,
                                          size_t message_length,
                                          uint8_t message_id[8]) {
    if (session == NULL || message == NULL || message_length == 0u)
        return RNS_ERROR_INVALID_ARGUMENT;
    if (session->info.state != RNS_RRC_SESSION_CONNECTED)
        return RNS_ERROR_INVALID_STATE;
    if (message_length > session->info.welcome.max_message_bytes)
        return RNS_ERROR_OVERFLOW;
    uint8_t normalized[RNS_RRC_MAX_ROOM_BYTES];
    uint8_t *body = malloc(message_length + 9u);
    if (body == NULL) return RNS_ERROR_NO_MEMORY;
    size_t normalized_length = 0u, body_length = 0u;
    rns_status_t status = normalized_room(session, room, room_length,
                                          normalized, &normalized_length);
    if (status == RNS_OK)
        status = rns_rrc_cbor_text(message, message_length, body,
                                   message_length + 9u, &body_length);
    if (status == RNS_OK)
        status = send_envelope(
            session, RNS_RRC_MESSAGE,
            (rns_rrc_slice_t){normalized, normalized_length},
            (rns_rrc_slice_t){body, body_length}, true, message_id);
    free(body);
    return status;
}

rns_status_t rns_rrc_session_ping(rns_rrc_session_t *session,
                                  uint8_t nonce[8]) {
    if (session == NULL || nonce == NULL) return RNS_ERROR_INVALID_ARGUMENT;
    if (session->info.state != RNS_RRC_SESSION_CONNECTED)
        return RNS_ERROR_INVALID_STATE;
    if (rns_hal_random_bytes(nonce, 8u) != RNS_OK) return RNS_ERROR_CRYPTO;
    uint8_t body[9] = {0x48u};
    memcpy(body + 1u, nonce, 8u);
    return send_envelope(session, RNS_RRC_PING, (rns_rrc_slice_t){0},
                         (rns_rrc_slice_t){body, sizeof body}, false, NULL);
}
