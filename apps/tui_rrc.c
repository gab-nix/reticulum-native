#include "tui_rrc.h"

#include "tui_text.h"

#include "reticulum/status.h"

#include <stdio.h>
#include <string.h>

static void set_status(tui_rrc_model_t *model, const char *text) {
    (void)snprintf(model->status, sizeof model->status, "%s", text);
}

void tui_rrc_init(tui_rrc_model_t *model) {
    if (model == NULL) return;
    memset(model, 0, sizeof *model);
    model->info.state = RNS_RRC_SESSION_DISCONNECTED;
    model->info.welcome.max_nick_bytes = RNS_RRC_DEFAULT_MAX_NICK_BYTES;
    model->info.welcome.max_room_bytes = RNS_RRC_DEFAULT_MAX_ROOM_BYTES;
    model->info.welcome.max_message_bytes = RNS_RRC_DEFAULT_MAX_MESSAGE_BYTES;
    model->info.welcome.max_rooms = RNS_RRC_DEFAULT_MAX_ROOMS;
    model->info.welcome.rate_per_minute = RNS_RRC_DEFAULT_RATE_PER_MINUTE;
    set_status(model, "Enter a verified hub address and public identity");
}

void tui_rrc_close(tui_rrc_model_t *model) {
    if (model == NULL) return;
    rns_rrc_session_destroy(model->session);
    model->session = NULL;
    model->info.state = RNS_RRC_SESSION_DISCONNECTED;
}

void tui_rrc_move(tui_rrc_model_t *model, int delta) {
    if (model == NULL) return;
    int selected = (int)model->selected + (delta < 0 ? -1 : 1);
    if (selected < 0) selected = (int)TUI_RRC_ITEM_COUNT - 1;
    if (selected >= (int)TUI_RRC_ITEM_COUNT) selected = 0;
    model->selected = (tui_rrc_item_t)selected;
}

size_t tui_rrc_edit_capacity(tui_rrc_item_t item) {
    switch (item) {
        case TUI_RRC_ITEM_HUB_ADDRESS: return TUI_RRC_HUB_ADDRESS_HEX;
        case TUI_RRC_ITEM_HUB_IDENTITY: return TUI_RRC_PUBLIC_IDENTITY_HEX;
        case TUI_RRC_ITEM_NICK: return RNS_RRC_DEFAULT_MAX_NICK_BYTES;
        case TUI_RRC_ITEM_ROOM: return RNS_RRC_DEFAULT_MAX_ROOM_BYTES;
        case TUI_RRC_ITEM_MESSAGE: return RNS_RRC_DEFAULT_MAX_MESSAGE_BYTES;
        case TUI_RRC_ITEM_CONNECT:
        case TUI_RRC_ITEM_JOIN:
        case TUI_RRC_ITEM_PART:
        case TUI_RRC_ITEM_SEND:
        case TUI_RRC_ITEM_COUNT: return 0u;
    }
    return 0u;
}

const char *tui_rrc_edit_value(const tui_rrc_model_t *model,
                               tui_rrc_item_t item) {
    if (model == NULL) return NULL;
    switch (item) {
        case TUI_RRC_ITEM_HUB_ADDRESS: return model->hub_address;
        case TUI_RRC_ITEM_HUB_IDENTITY: return model->hub_identity;
        case TUI_RRC_ITEM_NICK: return model->nick;
        case TUI_RRC_ITEM_ROOM: return model->room;
        case TUI_RRC_ITEM_MESSAGE: return model->outgoing;
        case TUI_RRC_ITEM_CONNECT:
        case TUI_RRC_ITEM_JOIN:
        case TUI_RRC_ITEM_PART:
        case TUI_RRC_ITEM_SEND:
        case TUI_RRC_ITEM_COUNT: return NULL;
    }
    return NULL;
}

static char *edit_target(tui_rrc_model_t *model, tui_rrc_item_t item,
                         size_t *capacity) {
    *capacity = tui_rrc_edit_capacity(item);
    switch (item) {
        case TUI_RRC_ITEM_HUB_ADDRESS: return model->hub_address;
        case TUI_RRC_ITEM_HUB_IDENTITY: return model->hub_identity;
        case TUI_RRC_ITEM_NICK: return model->nick;
        case TUI_RRC_ITEM_ROOM: return model->room;
        case TUI_RRC_ITEM_MESSAGE: return model->outgoing;
        case TUI_RRC_ITEM_CONNECT:
        case TUI_RRC_ITEM_JOIN:
        case TUI_RRC_ITEM_PART:
        case TUI_RRC_ITEM_SEND:
        case TUI_RRC_ITEM_COUNT: return NULL;
    }
    return NULL;
}

bool tui_rrc_edit_apply(tui_rrc_model_t *model, tui_rrc_item_t item,
                        const char *value, size_t value_length) {
    if (model == NULL || value == NULL || !tui_utf8_valid(
            (const uint8_t *)value, value_length))
        return false;
    size_t capacity = 0u;
    char *target = edit_target(model, item, &capacity);
    if (target == NULL || value_length > capacity) return false;
    if ((item == TUI_RRC_ITEM_HUB_ADDRESS ||
         item == TUI_RRC_ITEM_HUB_IDENTITY) && value_length != capacity) {
        set_status(model, item == TUI_RRC_ITEM_HUB_ADDRESS
                              ? "Hub address must be exactly 32 hex characters"
                              : "Hub identity must be exactly 128 hex characters");
        return false;
    }
    memcpy(target, value, value_length);
    target[value_length] = '\0';
    set_status(model, model->session == NULL
                          ? "RRC value updated for this session"
                          : "RRC value updated; reconnect to apply it");
    return true;
}

static void state_callback(rns_rrc_session_t *session,
                           const rns_rrc_session_info_t *info, void *opaque) {
    tui_rrc_model_t *model = opaque;
    (void)session;
    model->info = *info;
    switch (info->state) {
        case RNS_RRC_SESSION_DISCONNECTED: set_status(model, "Disconnected"); break;
        case RNS_RRC_SESSION_DISCOVERING: set_status(model, "Discovering hub path"); break;
        case RNS_RRC_SESSION_LINKING: set_status(model, "Authenticating hub link"); break;
        case RNS_RRC_SESSION_HELLO: set_status(model, "Identified; waiting for WELCOME"); break;
        case RNS_RRC_SESSION_CONNECTED: set_status(model, "Connected to RRC hub"); break;
        case RNS_RRC_SESSION_RECONNECT_WAIT: set_status(model, "Waiting to reconnect"); break;
        case RNS_RRC_SESSION_FAILED:
            (void)snprintf(model->status, sizeof model->status,
                           "RRC failed: %s", rns_status_string(info->last_error));
            break;
    }
}

void tui_rrc_apply_envelope(tui_rrc_model_t *model,
                            const rns_rrc_envelope_t *envelope) {
    if (model == NULL || envelope == NULL ||
        (envelope->type != RNS_RRC_MESSAGE &&
         envelope->type != RNS_RRC_NOTICE &&
         envelope->type != RNS_RRC_ERROR))
        return;
    rns_rrc_slice_t text = {0};
    if (envelope->body_cbor.length == 0u ||
        rns_rrc_cbor_text_parse(envelope->body_cbor.data,
                                envelope->body_cbor.length, &text) != RNS_OK)
        return;
    if (model->message_count == TUI_RRC_MAX_MESSAGES) {
        memmove(&model->messages[0], &model->messages[1],
                (TUI_RRC_MAX_MESSAGES - 1u) * sizeof model->messages[0]);
        model->message_count--;
    }
    tui_rrc_message_t *message = &model->messages[model->message_count++];
    memset(message, 0, sizeof *message);
    message->type = envelope->type;
    message->timestamp_ms = envelope->timestamp_ms;
    memcpy(message->source, envelope->source, sizeof message->source);
    (void)tui_text_sanitize(envelope->room.data, envelope->room.length,
                            message->room, sizeof message->room);
    (void)tui_text_sanitize(envelope->nick.data, envelope->nick.length,
                            message->nick, sizeof message->nick);
    (void)tui_text_sanitize(text.data, text.length, message->body,
                            sizeof message->body);
    set_status(model, "RRC message received");
}

static void envelope_callback(rns_rrc_session_t *session,
                              const rns_rrc_envelope_t *envelope,
                              void *opaque) {
    (void)session;
    tui_rrc_apply_envelope(opaque, envelope);
}

bool tui_rrc_connect_toggle(tui_rrc_model_t *model, rns_runtime_t *runtime,
                            const rns_identity *local_identity,
                            uint64_t now_ms) {
    if (model == NULL || local_identity == NULL) return false;
    if (model->session != NULL) {
        tui_rrc_close(model);
        set_status(model, "Disconnected from RRC hub");
        return true;
    }
    if (runtime == NULL) {
        set_status(model, "RRC needs an online Reticulum configuration");
        return false;
    }
    uint8_t destination[RNS_TRUNCATED_HASH_SIZE];
    uint8_t public_key[RNS_IDENTITY_PUBLIC_SIZE];
    if (!tui_hex_parse(model->hub_address, destination, sizeof destination)) {
        set_status(model, "Hub address must be exactly 32 hexadecimal characters");
        return false;
    }
    if (!tui_hex_parse(model->hub_identity, public_key, sizeof public_key)) {
        set_status(model, "Hub public identity must be exactly 128 hexadecimal characters");
        return false;
    }
    rns_identity hub;
    if (!rns_identity_from_public(&hub, public_key)) {
        set_status(model, "Hub public identity is invalid");
        return false;
    }
    rns_rrc_session_options_t options = {
        .runtime = runtime,
        .local_identity = local_identity,
        .hub_identity = &hub,
        .nick = (const uint8_t *)model->nick,
        .nick_length = strlen(model->nick),
        .auto_reconnect = true,
        .path_timeout_ms = 5000u,
        .hello_interval_ms = 3000u,
        .hello_max_attempts = 5u,
        .state_callback = state_callback,
        .envelope_callback = envelope_callback,
        .callback_context = model};
    memcpy(options.hub_destination, destination, sizeof destination);
    rns_status_t status = rns_rrc_session_create(&model->session, &options);
    if (status == RNS_OK)
        status = rns_rrc_session_connect(model->session, now_ms);
    if (status != RNS_OK) {
        rns_rrc_session_destroy(model->session);
        model->session = NULL;
        (void)snprintf(model->status, sizeof model->status,
                       "Cannot connect: %s", rns_status_string(status));
        return false;
    }
    return true;
}

bool tui_rrc_join(tui_rrc_model_t *model) {
    if (model == NULL || model->session == NULL) return false;
    rns_status_t status = rns_rrc_session_join(
        model->session, (const uint8_t *)model->room, strlen(model->room),
        NULL, 0u);
    if (status != RNS_OK) {
        (void)snprintf(model->status, sizeof model->status,
                       "Join failed: %s", rns_status_string(status));
        return false;
    }
    set_status(model, "Join request sent; room tracking is not implemented yet");
    return true;
}

bool tui_rrc_part(tui_rrc_model_t *model) {
    if (model == NULL || model->session == NULL) return false;
    rns_status_t status = rns_rrc_session_part(
        model->session, (const uint8_t *)model->room, strlen(model->room));
    if (status != RNS_OK) {
        (void)snprintf(model->status, sizeof model->status,
                       "Part failed: %s", rns_status_string(status));
        return false;
    }
    set_status(model, "Part request sent; room tracking is not implemented yet");
    return true;
}

bool tui_rrc_send(tui_rrc_model_t *model) {
    if (model == NULL || model->session == NULL) return false;
    uint8_t message_id[RNS_RRC_MESSAGE_ID_SIZE];
    rns_status_t status = rns_rrc_session_send_message(
        model->session, (const uint8_t *)model->room, strlen(model->room),
        (const uint8_t *)model->outgoing, strlen(model->outgoing), message_id);
    if (status != RNS_OK) {
        (void)snprintf(model->status, sizeof model->status,
                       "Send failed: %s", rns_status_string(status));
        return false;
    }
    model->outgoing[0] = '\0';
    set_status(model, "RRC message sent (hub acknowledgement is not tracked)");
    return true;
}

void tui_rrc_poll(tui_rrc_model_t *model, uint64_t now_ms) {
    if (model == NULL || model->session == NULL) return;
    rns_status_t status = rns_rrc_session_poll(model->session, now_ms);
    rns_rrc_session_get_info(model->session, &model->info);
    if (status != RNS_OK && model->info.state != RNS_RRC_SESSION_FAILED)
        (void)snprintf(model->status, sizeof model->status,
                       "RRC poll failed: %s", rns_status_string(status));
}
