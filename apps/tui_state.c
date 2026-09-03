#include "tui_state.h"

#include "tui_text.h"

#include "reticulum/config.h"
#include "reticulum/destination.h"
#include "reticulum/hal.h"
#include "reticulum/lxmf.h"

#include <stdarg.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define TUI_ROUTER_INTERVAL_MS 1000u
#define TUI_ROUTER_BATCH 2u
#define TUI_RUNTIME_BATCH 32u
#define TUI_NODE_LIFETIME 3600.0

static const uint8_t *message_peer(const tui_state_t *state,
                                   const lxmf_store_message_t *message) {
    return memcmp(message->source, state->local, LXMF_DESTINATION_LENGTH) == 0
               ? message->destination : message->source;
}

static size_t contact_index(const tui_state_t *state,
                            const uint8_t peer[LXMF_DESTINATION_LENGTH]) {
    for (size_t i = 0u; i < state->contact_count; ++i)
        if (memcmp(state->contacts[i].peer, peer, LXMF_DESTINATION_LENGTH) == 0) return i;
    return state->contact_count;
}

/* Returns the contact index, or contact_count when the table is full. */
static size_t ensure_contact(tui_state_t *state,
                             const uint8_t peer[LXMF_DESTINATION_LENGTH]) {
    size_t index = contact_index(state, peer);
    if (index < state->contact_count) return index;
    if (state->contact_count >= TUI_MAX_CONTACTS) return state->contact_count;
    tui_contact_t *contact = &state->contacts[state->contact_count];
    memset(contact, 0, sizeof *contact);
    memcpy(contact->peer, peer, LXMF_DESTINATION_LENGTH);
    contact->trust = TUI_TRUST_UNKNOWN;
    state->filter_dirty = true;
    return state->contact_count++;
}

/* Copies one stored message into the in-memory history and its conversation. */
static bool ingest_message(tui_state_t *state, const lxmf_store_message_t *message) {
    if (state->message_count >= TUI_MAX_MESSAGES) return false;
    tui_message_t *copy = &state->messages[state->message_count];
    copy->value = *message;
    memcpy(copy->content, message->content.data, message->content.len);
    copy->value.content.data = copy->content;
    const uint8_t *peer = message_peer(state, message);
    size_t index = ensure_contact(state, peer);
    if (index == state->contact_count) return false;
    state->message_count++;
    tui_contact_t *contact = &state->contacts[index];
    contact->messages++;
    if (message->timestamp > contact->latest) contact->latest = message->timestamp;
    if (memcmp(message->source, state->local, LXMF_DESTINATION_LENGTH) != 0)
        contact->unread++;
    state->filter_dirty = true;
    return true;
}

void tui_state_set_status(tui_state_t *state, const char *format, ...) {
    va_list arguments;
    if (state == NULL || format == NULL) return;
    va_start(arguments, format);
    (void)vsnprintf(state->status, sizeof state->status, format, arguments);
    va_end(arguments);
}

/* ---------------------------------------------------------------- filtering */

static bool contact_matches(const tui_state_t *state, size_t index) {
    const tui_contact_t *contact = &state->contacts[index];
    if (contact->trust != state->tab) return false;
    const char *needle = tui_editor_text(&state->search);
    if (tui_editor_empty(&state->search)) return true;
    char peer[TUI_ADDRESS_DIGITS + 1u];
    tui_hex_format(contact->peer, LXMF_DESTINATION_LENGTH, peer);
    if (tui_text_contains((const uint8_t *)peer, strlen(peer), needle) ||
        tui_text_contains((const uint8_t *)contact->note, strlen(contact->note), needle))
        return true;
    for (size_t i = 0u; i < state->message_count; ++i) {
        const lxmf_store_message_t *message = &state->messages[i].value;
        if (memcmp(message_peer(state, message), contact->peer,
                   LXMF_DESTINATION_LENGTH) == 0 &&
            tui_text_contains(message->content.data, message->content.len, needle))
            return true;
    }
    return false;
}

static bool thread_matches(const tui_state_t *state, const tui_message_t *message) {
    if (state->selected >= state->contact_count) return false;
    if (memcmp(message_peer(state, &message->value),
               state->contacts[state->selected].peer, LXMF_DESTINATION_LENGTH) != 0)
        return false;
    return tui_text_contains(message->value.content.data, message->value.content.len,
                             tui_editor_text(&state->search));
}

void tui_state_refresh(tui_state_t *state) {
    if (state == NULL || !state->filter_dirty) return;
    state->visible_count = 0u;
    for (size_t i = 0u; i < state->contact_count; ++i)
        if (contact_matches(state, i)) state->visible[state->visible_count++] = i;
    state->thread_count = 0u;
    for (size_t i = 0u; i < state->message_count; ++i)
        if (thread_matches(state, &state->messages[i]))
            state->thread[state->thread_count++] = i;
    if (state->scroll > state->thread_count) state->scroll = state->thread_count;
    state->filter_dirty = false;
}

const tui_contact_t *tui_state_contact(const tui_state_t *state, size_t index) {
    return state != NULL && index < state->contact_count ? &state->contacts[index] : NULL;
}

const tui_contact_t *tui_state_selected_contact(const tui_state_t *state) {
    return state != NULL ? tui_state_contact(state, state->selected) : NULL;
}

size_t tui_state_thread_count(const tui_state_t *state) {
    return state != NULL ? state->thread_count : 0u;
}

const tui_message_t *tui_state_thread_message(const tui_state_t *state, size_t index) {
    if (state == NULL || index >= state->thread_count) return NULL;
    return &state->messages[state->thread[index]];
}

bool tui_state_outgoing(const tui_state_t *state, const tui_message_t *message) {
    return state != NULL && message != NULL &&
           memcmp(message->value.source, state->local, LXMF_DESTINATION_LENGTH) == 0;
}

/* -------------------------------------------------------------- persistence */

static bool load_identity(const char *path, rns_identity *identity) {
    uint8_t bytes[64];
    FILE *file = fopen(path, "rb");
    if (file == NULL) return false;
    size_t count = fread(bytes, 1u, sizeof bytes, file);
    int extra = fgetc(file);
    (void)fclose(file);
    return count == sizeof bytes && extra == EOF &&
           rns_identity_from_private(identity, bytes);
}

static lxmf_peer_trust_t to_store_trust(tui_trust_t trust) {
    if (trust == TUI_TRUST_TRUSTED) return LXMF_PEER_TRUST_TRUSTED;
    if (trust == TUI_TRUST_UNTRUSTED) return LXMF_PEER_TRUST_UNTRUSTED;
    return LXMF_PEER_TRUST_UNKNOWN;
}

static tui_trust_t from_store_trust(lxmf_peer_trust_t trust) {
    if (trust == LXMF_PEER_TRUST_TRUSTED) return TUI_TRUST_TRUSTED;
    if (trust == LXMF_PEER_TRUST_UNTRUSTED) return TUI_TRUST_UNTRUSTED;
    return TUI_TRUST_UNKNOWN;
}

static bool load_message(void *context, const lxmf_store_message_t *message) {
    return ingest_message(context, message);
}

static bool load_peer(void *context, const lxmf_peer_t *peer) {
    tui_state_t *state = context;
    size_t index = ensure_contact(state, peer->address);
    if (index == state->contact_count) return true;
    tui_contact_t *contact = &state->contacts[index];
    contact->trust = from_store_trust(peer->trust);
    contact->pinned = peer->pinned;
    contact->blocked = peer->blocked;
    contact->unread = peer->unread_count;
    size_t note_length = peer->note_len;
    if (note_length >= sizeof contact->note) note_length = sizeof contact->note - 1u;
    memcpy(contact->note, peer->note, note_length);
    contact->note[note_length] = '\0';
    state->filter_dirty = true;
    return true;
}

void tui_state_persist_contacts(tui_state_t *state) {
    if (state == NULL || state->peer_store.implementation == NULL) return;
    for (size_t i = 0u; i < state->contact_count; ++i) {
        const tui_contact_t *contact = &state->contacts[i];
        lxmf_peer_t peer = {0};
        bool inserted = false;
        lxmf_status_t status = lxmf_peer_store_get(&state->peer_store,
                                                   contact->peer, &peer);
        if (status != LXMF_OK && status != LXMF_ERR_FORMAT) return;
        if (status == LXMF_ERR_FORMAT)
            memcpy(peer.address, contact->peer, sizeof peer.address);
        peer.trust = to_store_trust(contact->trust);
        peer.blocked = contact->blocked;
        peer.pinned = contact->pinned;
        peer.unread_count = contact->unread > UINT32_MAX ? UINT32_MAX
                                                         : (uint32_t)contact->unread;
        peer.note_len = strlen(contact->note);
        memcpy(peer.note, contact->note, peer.note_len);
        if (lxmf_peer_store_put(&state->peer_store, &peer, &inserted) != LXMF_OK) return;
    }
    (void)lxmf_peer_store_save(&state->peer_store);
}

/* ------------------------------------------------------------ runtime hooks */

static tui_message_t *find_message(tui_state_t *state,
                                   const uint8_t id[LXMF_MESSAGE_ID_LENGTH],
                                   size_t *position) {
    for (size_t i = 0u; i < state->message_count; ++i) {
        if (memcmp(state->messages[i].value.message_id, id,
                   LXMF_MESSAGE_ID_LENGTH) != 0)
            continue;
        if (position != NULL) *position = i;
        return &state->messages[i];
    }
    return NULL;
}

static void recalculate_contact(tui_state_t *state, size_t contact) {
    tui_contact_t *entry = &state->contacts[contact];
    entry->messages = 0u;
    entry->latest = 0.0;
    for (size_t i = 0u; i < state->message_count; ++i) {
        const lxmf_store_message_t *message = &state->messages[i].value;
        if (memcmp(message_peer(state, message), entry->peer,
                   LXMF_DESTINATION_LENGTH) != 0)
            continue;
        entry->messages++;
        if (message->timestamp > entry->latest) entry->latest = message->timestamp;
    }
}

void tui_state_apply_router_event(tui_state_t *state,
                                  const lxmf_router_event_t *event) {
    if (state == NULL || event == NULL) return;
    tui_message_t *message = find_message(state, event->message_id, NULL);
    if (message != NULL) {
        message->value.status = event->state;
        state->filter_dirty = true;
    }
    if (event->state == LXMF_DELIVERY_QUEUED) {
        lxmf_stamp_job_progress_t progress;
        if (event->queue_reason == LXMF_QUEUE_REASON_STAMP &&
            state->router_ready &&
            lxmf_router_stamp_progress(&state->router, event->message_id,
                                       &progress) == LXMF_OK) {
            if (progress.prepared_rounds < LXMF_STAMP_WORKBLOCK_ROUNDS)
                tui_state_set_status(
                    state, "Preparing delivery stamp: %u%%",
                    (unsigned)(progress.prepared_rounds * 100u /
                               LXMF_STAMP_WORKBLOCK_ROUNDS));
            else
                tui_state_set_status(
                    state, "Searching for delivery stamp; %llu attempts",
                    (unsigned long long)progress.attempts);
        } else
            tui_state_set_status(state, "Queued via %s; waiting for %s",
                                 lxmf_delivery_method_string(event->method),
                                 lxmf_queue_reason_string(event->queue_reason));
    } else if (event->state == LXMF_DELIVERY_SENDING) {
        tui_state_set_status(state, "Sending via %s",
                             lxmf_delivery_method_string(event->method));
    } else if (event->state == LXMF_DELIVERY_SENT) {
        tui_state_set_status(state, "Sent via %s; awaiting delivery proof",
                             lxmf_delivery_method_string(event->method));
    } else if (event->state == LXMF_DELIVERY_DELIVERED) {
        tui_state_set_status(state, "Delivered via %s",
                             lxmf_delivery_method_string(event->method));
    } else {
        tui_state_set_status(state, "Delivery via %s failed: %s",
                             lxmf_delivery_method_string(event->method),
                             lxmf_status_string(event->result));
    }
}

void tui_state_apply_signature(tui_state_t *state,
                               const uint8_t id[LXMF_MESSAGE_ID_LENGTH],
                               lxmf_signature_state_t signature) {
    size_t position = 0u;
    if (state == NULL || id == NULL) return;
    tui_message_t *message = find_message(state, id, &position);
    if (message == NULL) return;
    if (signature != LXMF_SIGNATURE_FAILED) {
        message->value.signature_state = signature;
        state->filter_dirty = true;
        tui_state_set_status(state, signature == LXMF_SIGNATURE_VERIFIED
                                        ? "Previously unverified message is now verified"
                                        : "Message sender is not verified yet");
        return;
    }
    uint8_t peer[LXMF_DESTINATION_LENGTH];
    memcpy(peer, message_peer(state, &message->value), sizeof peer);
    bool incoming = memcmp(message->value.source, state->local,
                           LXMF_DESTINATION_LENGTH) != 0;
    if (position + 1u < state->message_count)
        memmove(&state->messages[position], &state->messages[position + 1u],
                (state->message_count - position - 1u) * sizeof state->messages[0]);
    state->message_count--;
    memset(&state->messages[state->message_count], 0, sizeof state->messages[0]);
    size_t contact = contact_index(state, peer);
    if (contact < state->contact_count) {
        if (incoming && state->contacts[contact].unread > 0u)
            state->contacts[contact].unread--;
        recalculate_contact(state, contact);
    }
    state->filter_dirty = true;
    tui_state_set_status(state, "Rejected message with an invalid signature");
}

static void on_delivery(void *context,
                        const uint8_t message_id[LXMF_MESSAGE_ID_LENGTH],
                        lxmf_delivery_status_t status, lxmf_status_t result) {
    tui_state_t *state = context;
    tui_message_t *message = find_message(state, message_id, NULL);
    if (message != NULL) message->value.status = status;
    state->send_attempted = true;
    state->send_ok = result == LXMF_OK;
    state->filter_dirty = true;
}

static void on_router_event(void *context, const lxmf_router_event_t *event) {
    tui_state_apply_router_event(context, event);
}

static void on_signature(void *context,
                         const uint8_t message_id[LXMF_MESSAGE_ID_LENGTH],
                         lxmf_signature_state_t signature) {
    tui_state_apply_signature(context, message_id, signature);
}

static void on_announce(rns_runtime_t *runtime, const rns_node_result *announce,
                        void *context) {
    tui_state_t *state = context;
    (void)runtime;
    if (!rns_node_registry_consider_announce(&state->nodes, announce) ||
        !state->router_ready)
        return;
    lxmf_router_verify_result_t verified;
    (void)lxmf_router_verify_pending(&state->router,
                                     announce->destination_hash, &verified);
}

static const rns_identity *resolve_peer(void *context,
                                        const uint8_t destination[LXMF_DESTINATION_LENGTH]) {
    tui_state_t *state = context;
    for (size_t i = 0u; i < state->nodes.count; ++i) {
        const rns_node_record *node = &state->nodes.records[i];
        if (node->has_message_destination &&
            memcmp(node->message_destination, destination, LXMF_DESTINATION_LENGTH) == 0 &&
            rns_identity_from_public(&state->resolved_identity, node->public_key))
            return &state->resolved_identity;
    }
    return NULL;
}

static bool resolve_peer_ratchet(
    void *context, const uint8_t destination[LXMF_DESTINATION_LENGTH],
    uint8_t ratchet_public[RNS_RATCHET_PUBLIC_SIZE]) {
    tui_state_t *state = context;
    for (size_t i = 0u; i < state->nodes.count; ++i) {
        const rns_node_record *node = &state->nodes.records[i];
        if (node->has_message_destination && node->has_ratchet &&
            memcmp(node->message_destination, destination,
                   LXMF_DESTINATION_LENGTH) == 0) {
            memcpy(ratchet_public, node->ratchet, RNS_RATCHET_PUBLIC_SIZE);
            return true;
        }
    }
    return false;
}

bool tui_state_peer_stamp_cost(
    const tui_state_t *state,
    const uint8_t destination[LXMF_DESTINATION_LENGTH], uint8_t *cost) {
    if (state == NULL || destination == NULL || cost == NULL) return false;
    *cost = 0u;
    for (size_t i = 0u; i < state->nodes.count; ++i) {
        const rns_node_record *node = &state->nodes.records[i];
        if (node->kind == RNS_NODE_KIND_LXMF &&
            node->has_message_destination && node->lxmf_app_data_valid &&
            node->lxmf_has_stamp_cost && node->lxmf_stamp_cost > 0u &&
            node->lxmf_stamp_cost < UINT8_MAX &&
            memcmp(node->message_destination, destination,
                   LXMF_DESTINATION_LENGTH) == 0) {
            *cost = node->lxmf_stamp_cost;
            return true;
        }
    }
    return false;
}

static bool resolve_peer_stamp_cost(
    void *context, const uint8_t destination[LXMF_DESTINATION_LENGTH],
    uint8_t *cost) {
    return tui_state_peer_stamp_cost(context, destination, cost);
}

static uint64_t wall_clock_seconds(void *context) {
    uint64_t milliseconds = 0u;
    (void)context;
    return rns_hal_wallclock_ms(&milliseconds) == RNS_OK
               ? milliseconds / UINT64_C(1000)
               : 0u;
}

static lxmf_status_t send_via_runtime(void *context, const uint8_t *packet,
                                      size_t length) {
    tui_state_t *state = context;
    if (state->runtime == NULL) return LXMF_ERR_ARGUMENT;
    /*
     * Route by the learned path instead of shouting on the first interface:
     * a peer more than one hop away needs a transport header or no node will
     * forward the packet.
     */
    return rns_runtime_send_routed(state->runtime, packet, length) == RNS_OK
               ? LXMF_OK : LXMF_ERR_CRYPTO;
}

static void on_message(void *context, const lxmf_store_message_t *message) {
    tui_state_t *state = context;
    if (!ingest_message(state, message))
        tui_state_set_status(state, "Incoming message saved; conversation list is full");
    else tui_state_set_status(state, "Incoming LXMF message received");
}

static void on_packet(rns_runtime_t *runtime, const uint8_t *packet, size_t length,
                      const rns_node_result *result, void *context) {
    tui_state_t *state = context;
    if (state->router_ready &&
        lxmf_router_receive_packet(&state->router, packet, length) == LXMF_OK)
        (void)rns_runtime_prove_packet(runtime, result, &state->identity, true);
}

static char *read_text_file(const char *path, size_t *length) {
    FILE *file = fopen(path, "rb");
    char *text;
    long size;
    if (file == NULL) return NULL;
    if (fseek(file, 0, SEEK_END) != 0 || (size = ftell(file)) < 0 ||
        fseek(file, 0, SEEK_SET) != 0) {
        (void)fclose(file);
        return NULL;
    }
    text = malloc((size_t)size + 1u);
    if (text == NULL) {
        (void)fclose(file);
        return NULL;
    }
    if (fread(text, 1u, (size_t)size, file) != (size_t)size) {
        free(text);
        (void)fclose(file);
        return NULL;
    }
    text[size] = '\0';
    (void)fclose(file);
    *length = (size_t)size;
    return text;
}

static void start_runtime(tui_state_t *state, const char *config_path) {
    size_t length = 0u;
    char *text = read_text_file(config_path, &length);
    rns_config_t config;
    rns_config_diagnostic_t diagnostic = {0};
    if (text == NULL) {
        tui_state_set_status(state, "Cannot read network configuration %s", config_path);
        return;
    }
    rns_config_init(&config);
    rns_status_t parsed = rns_config_parse(text, length, &config, &diagnostic);
    free(text);
    if (parsed != RNS_OK) {
        /*
         * The parser only fills the diagnostic once it is reading lines, so a
         * file that is not text at all reports the bare status instead.
         */
        if (diagnostic.message[0] != '\0')
            tui_state_set_status(state, "Configuration %s line %zu: %s", config_path,
                                 diagnostic.line, diagnostic.message);
        else
            tui_state_set_status(state, "Configuration %s is not a text configuration (%s)",
                                 config_path, rns_status_string(parsed));
        return;
    }
    if (config.interface_count == 0u) {
        tui_state_set_status(state, "Configuration %s defines no interfaces", config_path);
        return;
    }
    rns_runtime_options_t options = {0};
    options.packet_callback = on_packet;
    options.announce_callback = on_announce;
    options.callback_context = state;
    rns_status_t status = rns_runtime_create(&state->runtime, &config, &options);
    if (status == RNS_OK) {
        lxmf_router_config_t router = {
            .identity = &state->identity,
            .store = &state->store,
            .ticket_store = state->ticket_store,
            .ratchet_store = state->ratchet_store,
            .wall_clock = wall_clock_seconds,
            .wall_clock_context = state,
            .inbound_stamp_cost = state->settings.has_stamp_cost
                                      ? state->settings.stamp_cost
                                      : 0u,
            .runtime = state->runtime,
            .resolve_identity = resolve_peer,
            .resolve_context = state,
            .resolve_ratchet = resolve_peer_ratchet,
            .ratchet_context = state,
            .resolve_stamp_cost = resolve_peer_stamp_cost,
            .stamp_cost_context = state,
            .stamp_work_units = LXMF_STAMP_POLL_MAX_UNITS,
            .send_packet = send_via_runtime,
            .send_context = state,
            .message_callback = on_message,
            .message_context = state,
            .delivery_callback = on_delivery,
            .delivery_context = state,
            .signature_callback = on_signature,
            .signature_context = state,
            .event_callback = on_router_event,
            .event_context = state,
            .preferred_delivery_method = LXMF_DELIVERY_METHOD_DIRECT,
            .accept_inbound_links = true
        };
        state->router_ready = lxmf_router_init(&state->router, &router) == LXMF_OK;
        if (state->router_ready &&
            rns_runtime_register_destination(state->runtime, state->local) != RNS_OK)
            state->router_ready = false;
    }
    if (status != RNS_OK) {
        tui_state_set_status(state, "Network startup failed: %s", rns_status_string(status));
        return;
    }
    if (!state->router_ready) {
        tui_state_set_status(state, "Network started but LXMF delivery is unavailable");
        return;
    }
    size_t down = 0u;
    size_t total = rns_runtime_interface_count(state->runtime);
    rns_status_t reason = RNS_OK;
    char failing[RNS_CONFIG_NAME_MAX] = {0};
    for (size_t i = 0u; i < total; ++i) {
        rns_runtime_interface_info_t info;
        if (rns_runtime_interface_info(state->runtime, i, &info) != RNS_OK) continue;
        if (info.state == RNS_RUNTIME_INTERFACE_UP && info.last_error == RNS_OK) continue;
        if (down++ == 0u) {
            reason = info.last_error;
            (void)snprintf(failing, sizeof failing, "%s", info.name);
        }
    }
    if (down == total)
        tui_state_set_status(state, "No interface is up (%s: %s)", failing,
                             rns_status_string(reason));
    else if (down > 0u)
        tui_state_set_status(state, "Network runtime active; %zu of %zu interfaces down",
                             down, total);
    else tui_state_set_status(state, "Network runtime active");
}

bool tui_state_link_ready(const tui_state_t *state) {
    if (state == NULL || state->runtime == NULL) return false;
    for (size_t i = 0u; i < rns_runtime_interface_count(state->runtime); ++i) {
        rns_runtime_interface_info_t info;
        if (rns_runtime_interface_info(state->runtime, i, &info) != RNS_OK) continue;
        if (info.state == RNS_RUNTIME_INTERFACE_UP && info.last_error == RNS_OK) return true;
    }
    return false;
}

/* ------------------------------------------------------------- lifecycle */

/* Written in Micron so the built-in page exercises the same parser as a
 * remote one: sections, a divider, formatting escapes and links. */
static const uint8_t tui_home_page[] =
    ">Nomad Browser\n"
    "\n"
    "`cNative Micron rendering is active.\n"
    "`a\n"
    "-\n"
    "Select a node on the `!network`! screen and press Enter to browse it.\n"
    "\n"
    "Links on a remote page are `_underlined`_. `!j`! and `!k`! move between\n"
    "them, `!Enter`! follows one, and `!Backspace`! goes back.\n";

int tui_state_open(tui_state_t *state, const char *identity_path,
                   const char *store_path, const char *destination_hex,
                   const char *config_path) {
    static const char *const aspects[] = {"delivery"};
    int written;
    if (state == NULL || identity_path == NULL || store_path == NULL) return -1;
    memset(state, 0, sizeof *state);
    tui_editor_init(&state->composer, TUI_COMPOSER_CAPACITY);
    tui_editor_init(&state->search, TUI_SEARCH_CAPACITY);
    tui_editor_init(&state->address, TUI_ADDRESS_DIGITS);
    tui_editor_init(&state->setting, LXMF_DISPLAY_NAME_MAX);
    tui_settings_defaults(&state->settings);
    state->tab = TUI_TRUST_UNKNOWN;
    state->screen = TUI_SCREEN_CONVERSATIONS;
    state->filter_dirty = true;
    state->messages = calloc(TUI_MAX_MESSAGES, sizeof *state->messages);
    if (state->messages == NULL) return -1;
    if (!load_identity(identity_path, &state->identity) ||
        !rns_destination_hash(&state->identity, "lxmf", aspects, 1u, state->local))
        goto fail;
    if (lxmf_store_open(&state->store, store_path) != LXMF_OK ||
        lxmf_store_list(&state->store, load_message, state) != LXMF_OK) goto fail;
    written = snprintf(state->settings_path, sizeof state->settings_path,
                       "%s.settings", store_path);
    if (written <= 0 || (size_t)written >= sizeof state->settings_path) goto fail;
    if (!tui_settings_load(state->settings_path, &state->settings, NULL)) {
        tui_settings_defaults(&state->settings);
        state->settings_load_error = true;
    }
    written = snprintf(state->peer_store_path, sizeof state->peer_store_path,
                       "%s.peers", store_path);
    if (written <= 0 || (size_t)written >= sizeof state->peer_store_path ||
        lxmf_peer_store_open(&state->peer_store, state->peer_store_path) != LXMF_OK ||
        lxmf_peer_store_list(&state->peer_store, load_peer, state) != LXMF_OK) goto fail;
    written = snprintf(state->ticket_store_path,
                       sizeof state->ticket_store_path, "%s.tickets",
                       store_path);
    if (written <= 0 || (size_t)written >= sizeof state->ticket_store_path ||
        lxmf_ticket_store_open(&state->ticket_store,
                               state->ticket_store_path) != LXMF_OK)
        goto fail;
    written = snprintf(state->ratchet_store_path,
                       sizeof state->ratchet_store_path, "%s.ratchets",
                       store_path);
    if (written <= 0 || (size_t)written >= sizeof state->ratchet_store_path ||
        rns_ratchet_store_open(&state->ratchet_store,
                               state->ratchet_store_path, &state->identity,
                               0u, 0u) != RNS_OK)
        goto fail;
    if (destination_hex != NULL) {
        uint8_t peer[LXMF_DESTINATION_LENGTH];
        if (!tui_hex_parse(destination_hex, peer, sizeof peer)) goto fail;
        (void)ensure_contact(state, peer);
    }
    tui_state_set_status(state, "Offline outbox - network delivery is not connected yet");

    rns_node_registry_init(&state->nodes, TUI_NODE_LIFETIME);
    written = snprintf(state->node_store_path, sizeof state->node_store_path,
                       "%s.nodes", store_path);
    if (written > 0 && (size_t)written < sizeof state->node_store_path)
        (void)rns_node_registry_load(&state->nodes, state->node_store_path,
                                     TUI_NODE_LIFETIME);
    else state->node_store_path[0] = '\0';

    if (config_path != NULL) start_runtime(state, config_path);
    state->startup_announce_pending = state->runtime != NULL &&
                                      state->settings.announce_at_start;
    if (state->runtime != NULL &&
        rns_browser_create(&state->browser, state->runtime, NULL) != RNS_OK)
        tui_state_set_status(state, "Page browser unavailable; messaging remains active");

    rns_micron_history_init(&state->history);
    (void)snprintf(state->url, sizeof state->url, "nomad://local/home");
    if (!rns_micron_parse(&state->page, tui_home_page, sizeof tui_home_page - 1u) ||
        !rns_micron_history_push(&state->history, state->url)) goto fail;
    if (state->settings_load_error)
        tui_state_set_status(state,
                             "Settings file is invalid; safe defaults are active");
    tui_state_refresh(state);
    return 0;
fail:
    tui_state_close(state);
    return -1;
}

void tui_state_close(tui_state_t *state) {
    if (state == NULL) return;
    if (state->node_store_path[0] != '\0')
        (void)rns_node_registry_save(&state->nodes, state->node_store_path);
    if (state->settings_path[0] != '\0')
        (void)tui_settings_save(state->settings_path, &state->settings);
    rns_browser_destroy(state->browser);
    state->browser = NULL;
    if (state->router_ready) lxmf_router_destroy(&state->router);
    state->router_ready = false;
    rns_runtime_destroy(state->runtime);
    state->runtime = NULL;
    tui_state_persist_contacts(state);
    rns_ratchet_store_close(state->ratchet_store);
    state->ratchet_store = NULL;
    lxmf_ticket_store_close(state->ticket_store);
    state->ticket_store = NULL;
    lxmf_peer_store_close(&state->peer_store);
    lxmf_store_close(&state->store);
    free(state->messages);
    state->messages = NULL;
}

/* ------------------------------------------------------------------- polling */

static void poll_browser(tui_state_t *state) {
    if (state->browser == NULL) return;
    (void)rns_browser_poll(state->browser);
    rns_browser_state_t current = rns_browser_state(state->browser);
    if (current == state->browser_state) return;
    state->browser_state = current;
    if (current == RNS_BROWSER_COMPLETE) {
        const rns_micron_page *page = rns_browser_page(state->browser);
        if (page != NULL) state->page = *page;
        state->link_selected = 0u;
        state->page_scroll = 0u;
        tui_state_set_status(state, "Remote Nomad page loaded");
    } else if (current == RNS_BROWSER_FAILED) {
        tui_state_set_status(state, "Page load failed: %s",
                             rns_status_string(rns_browser_error(state->browser)));
    }
}

static bool announce_at(tui_state_t *state, uint64_t now) {
    static const char *const delivery[] = {"delivery"};
    uint8_t app_data[RNS_NODE_APP_DATA_MAX];
    size_t app_data_length = 0u;
    lxmf_status_t encoded = tui_settings_encode_announce(
        &state->settings, app_data, sizeof app_data, &app_data_length);
    if (encoded != LXMF_OK) {
        state->has_announce_result = true;
        state->last_announce_result = RNS_ERROR_INVALID_ARGUMENT;
        tui_state_set_status(state, "Cannot encode LXMF announce (%d)", encoded);
        return false;
    }
    if (state->runtime == NULL || !state->router_ready) {
        state->has_announce_result = true;
        state->last_announce_result = RNS_ERROR_INVALID_STATE;
        tui_state_set_status(state, "Cannot announce while the messaging runtime is offline");
        return false;
    }
    uint64_t wall_seconds = wall_clock_seconds(state);
    uint8_t ratchet_private[RNS_RATCHET_PRIVATE_SIZE];
    uint8_t ratchet_public[RNS_RATCHET_PUBLIC_SIZE];
    uint8_t ratchet_id[RNS_RATCHET_ID_SIZE];
    rns_status_t result = rns_ratchet_store_current(
        state->ratchet_store, wall_seconds, ratchet_private, ratchet_public,
        ratchet_id, NULL);
    if (result == RNS_OK)
        result = rns_runtime_announce_with_ratchet(
            state->runtime, &state->identity, "lxmf", delivery, 1u,
            ratchet_public, app_data, app_data_length);
    rns_hal_secure_zero(ratchet_private, sizeof ratchet_private);
    rns_hal_secure_zero(ratchet_public, sizeof ratchet_public);
    rns_hal_secure_zero(ratchet_id, sizeof ratchet_id);
    state->has_announce_result = true;
    state->last_announce_result = result;
    if (result != RNS_OK) {
        tui_state_set_status(state, "LXMF announce failed: %s",
                             rns_status_string(result));
        return false;
    }
    state->last_announce_ms = now;
    state->next_announce_ms = now + tui_settings_interval_ms(&state->settings);
    tui_state_set_status(state, "LXMF delivery destination announced");
    return true;
}

bool tui_state_announce(tui_state_t *state) {
    uint64_t now = 0u;
    if (state == NULL) return false;
    if (rns_hal_monotonic_ms(&now) != RNS_OK) {
        state->has_announce_result = true;
        state->last_announce_result = RNS_ERROR_IO;
        tui_state_set_status(state, "Cannot read the monotonic clock for announce");
        return false;
    }
    return announce_at(state, now);
}

void tui_state_poll(tui_state_t *state) {
    uint64_t now = 0u;
    size_t processed = 0u;
    if (state == NULL || state->runtime == NULL) return;
    (void)rns_runtime_poll(state->runtime, TUI_RUNTIME_BATCH, &processed);
    poll_browser(state);
    if (rns_hal_monotonic_ms(&now) == RNS_OK) {
        (void)rns_node_registry_expire(&state->nodes, (double)now / 1000.0);
        if (state->next_announce_ms == 0u)
            state->next_announce_ms = now + tui_settings_interval_ms(&state->settings);
        if (tui_settings_announce_due(state->startup_announce_pending,
                                      state->next_announce_ms, now)) {
            state->startup_announce_pending = false;
            (void)announce_at(state, now);
            if (state->next_announce_ms <= now)
                state->next_announce_ms = now +
                                          tui_settings_interval_ms(&state->settings);
        }
        if (state->router_ready &&
            now - state->router_polled_ms >= TUI_ROUTER_INTERVAL_MS) {
            lxmf_router_poll_result_t delivery = {0};
            if (lxmf_router_poll(&state->router, TUI_ROUTER_BATCH, &delivery) == LXMF_OK)
                state->router_polled_ms = now;
        }
    }
}

/* ---------------------------------------------------------------- selection */

void tui_state_select_offset(tui_state_t *state, int delta) {
    if (state == NULL) return;
    tui_state_refresh(state);
    if (state->visible_count == 0u) return;
    size_t position = 0u;
    while (position < state->visible_count && state->visible[position] != state->selected)
        ++position;
    if (position == state->visible_count) position = 0u;
    else if (delta < 0)
        position = position == 0u ? state->visible_count - 1u : position - 1u;
    else position = (position + 1u) % state->visible_count;
    state->selected = state->visible[position];
    state->contacts[state->selected].unread = 0u;
    state->scroll = 0u;
    state->filter_dirty = true;
}

void tui_state_set_tab(tui_state_t *state, tui_trust_t tab) {
    if (state == NULL) return;
    state->tab = tab;
    state->scroll = 0u;
    state->filter_dirty = true;
    tui_state_refresh(state);
    if (state->visible_count > 0u) state->selected = state->visible[0];
    state->filter_dirty = true;
}

void tui_state_scroll_by(tui_state_t *state, int lines) {
    if (state == NULL) return;
    if (state->screen == TUI_SCREEN_BROWSER) {
        /* The thread list grows upward, a page downward, so the same key
         * moves the viewport the opposite way here. */
        size_t limit = state->page.line_count != 0u
                           ? (size_t)state->page.line_count - 1u : 0u;
        if (lines > 0) {
            size_t back = (size_t)lines;
            state->page_scroll = state->page_scroll > back ? state->page_scroll - back
                                                           : 0u;
        } else {
            state->page_scroll += (size_t)(-lines);
        }
        if (state->page_scroll > limit) state->page_scroll = limit;
        return;
    }
    tui_state_refresh(state);
    if (lines < 0) {
        size_t back = (size_t)(-lines);
        state->scroll = state->scroll > back ? state->scroll - back : 0u;
    } else {
        state->scroll += (size_t)lines;
        if (state->scroll > state->thread_count) state->scroll = state->thread_count;
    }
}

bool tui_state_open_conversation(tui_state_t *state,
                                 const uint8_t peer[LXMF_DESTINATION_LENGTH]) {
    if (state == NULL || peer == NULL) return false;
    size_t index = ensure_contact(state, peer);
    if (index == state->contact_count) {
        tui_state_set_status(state, "Conversation limit reached");
        return false;
    }
    state->selected = index;
    state->contacts[index].trust = TUI_TRUST_UNKNOWN;
    state->tab = TUI_TRUST_UNKNOWN;
    state->screen = TUI_SCREEN_CONVERSATIONS;
    state->overlay = TUI_OVERLAY_NONE;
    state->field = TUI_FIELD_COMPOSE;
    state->filter_dirty = true;
    return true;
}

/* ------------------------------------------------------------------ sending */

lxmf_status_t tui_state_queue_message(tui_state_t *state) {
    if (state == NULL || state->selected >= state->contact_count ||
        tui_editor_empty(&state->composer)) return LXMF_ERR_ARGUMENT;
    lxmf_message_t source = {0};
    lxmf_message_t decoded;
    uint8_t packed[LXMF_STORE_MAX_CONTENT + 256u];
    size_t packed_length = 0u;
    memcpy(source.destination, state->contacts[state->selected].peer,
           LXMF_DESTINATION_LENGTH);
    memcpy(source.source, state->local, LXMF_DESTINATION_LENGTH);
    source.timestamp = (double)time(NULL);
    source.content.data = (const uint8_t *)tui_editor_text(&state->composer);
    source.content.len = tui_editor_length(&state->composer);
    lxmf_status_t status = lxmf_pack(&source, lxmf_identity_signer, &state->identity,
                                     packed, sizeof packed, &packed_length);
    if (status != LXMF_OK) return status;
    status = lxmf_unpack(packed, packed_length, NULL, NULL, &decoded);
    if (status != LXMF_OK) return status;

    lxmf_store_message_t stored = {0};
    bool inserted = false;
    memcpy(stored.message_id, decoded.message_id, sizeof stored.message_id);
    memcpy(stored.destination, source.destination, sizeof stored.destination);
    memcpy(stored.source, source.source, sizeof stored.source);
    stored.timestamp = source.timestamp;
    stored.status = LXMF_DELIVERY_QUEUED;
    stored.content = source.content;
    stored.packed = (lxmf_slice_t){packed, packed_length};
    status = lxmf_store_put(&state->store, &stored, &inserted);
    state->send_attempted = false;
    state->send_ok = false;
    if (status != LXMF_OK || !inserted) return status;

    if (state->router_ready) {
        if (resolve_peer(state, stored.destination) == NULL) {
            state->send_attempted =
                rns_runtime_request_path(state->runtime, stored.destination) == RNS_OK;
            tui_state_set_status(state, "Queued; requesting a verified path to recipient");
        } else {
            state->send_attempted = true;
            state->send_ok = lxmf_router_send_message(&state->router,
                                                      decoded.message_id) == LXMF_OK;
        }
    }
    (void)ingest_message(state, &stored);
    return LXMF_OK;
}

/* -------------------------------------------------------- contact mutations */

void tui_state_set_trust(tui_state_t *state, tui_trust_t trust) {
    if (state == NULL || state->selected >= state->contact_count) return;
    state->contacts[state->selected].trust = trust;
    tui_state_set_tab(state, trust);
    tui_state_persist_contacts(state);
    tui_state_set_status(state, trust == TUI_TRUST_TRUSTED ? "Trusted contact saved"
                                                           : "Untrusted contact saved");
}

void tui_state_toggle_pin(tui_state_t *state) {
    if (state == NULL || state->selected >= state->contact_count) return;
    tui_contact_t *contact = &state->contacts[state->selected];
    contact->pinned = !contact->pinned;
    tui_state_persist_contacts(state);
    tui_state_set_status(state, "Pin saved");
}

void tui_state_toggle_block(tui_state_t *state) {
    if (state == NULL || state->selected >= state->contact_count) return;
    tui_contact_t *contact = &state->contacts[state->selected];
    contact->blocked = !contact->blocked;
    tui_state_persist_contacts(state);
    tui_state_set_status(state, "Block preference saved");
}

void tui_state_toggle_note(tui_state_t *state) {
    if (state == NULL || state->selected >= state->contact_count) return;
    tui_contact_t *contact = &state->contacts[state->selected];
    (void)snprintf(contact->note, sizeof contact->note, "%s",
                   contact->note[0] != '\0' ? "" : "Local note");
    state->filter_dirty = true;
    tui_state_persist_contacts(state);
    tui_state_set_status(state, "Contact note saved");
}

/* ---------------------------------------------------------------- settings */

bool tui_state_save_settings(tui_state_t *state) {
    if (state == NULL || state->settings_path[0] == '\0') return false;
    if (!tui_settings_save(state->settings_path, &state->settings)) {
        tui_state_set_status(state, "Could not save settings");
        return false;
    }
    if (state->router_ready)
        (void)lxmf_router_set_inbound_stamp_cost(
            &state->router, state->settings.has_stamp_cost
                                ? state->settings.stamp_cost
                                : 0u);
    return true;
}

void tui_state_setting_move(tui_state_t *state, int delta) {
    if (state == NULL || state->field != TUI_FIELD_NONE) return;
    int selected = (int)state->setting_selected + (delta < 0 ? -1 : 1);
    if (selected < 0) selected = (int)TUI_SETTING_COUNT - 1;
    if (selected >= (int)TUI_SETTING_COUNT) selected = 0;
    state->setting_selected = (tui_setting_item_t)selected;
}

static void begin_setting_edit(tui_state_t *state, const char *value) {
    tui_editor_clear(&state->setting);
    if (value != NULL)
        (void)tui_editor_insert(&state->setting, value, strlen(value));
    state->field = TUI_FIELD_SETTING;
}

void tui_state_setting_activate(tui_state_t *state) {
    char value[TUI_ADDRESS_DIGITS + 1u];
    if (state == NULL) return;
    switch (state->setting_selected) {
        case TUI_SETTING_DISPLAY_NAME:
            begin_setting_edit(state, state->settings.display_name);
            break;
        case TUI_SETTING_STAMP_COST:
            if (state->settings.has_stamp_cost)
                (void)snprintf(value, sizeof value, "%u",
                               (unsigned)state->settings.stamp_cost);
            else
                (void)snprintf(value, sizeof value, "%s", "off");
            begin_setting_edit(state, value);
            break;
        case TUI_SETTING_ANNOUNCE_AT_START: {
            bool previous = state->settings.announce_at_start;
            state->settings.announce_at_start = !state->settings.announce_at_start;
            if (tui_state_save_settings(state))
                tui_state_set_status(state, "Announce-at-start setting saved");
            else
                state->settings.announce_at_start = previous;
            break;
        }
        case TUI_SETTING_ANNOUNCE_INTERVAL:
            (void)snprintf(value, sizeof value, "%u",
                           state->settings.announce_interval_minutes);
            begin_setting_edit(state, value);
            break;
        case TUI_SETTING_PROPAGATION_NODE:
            if (state->settings.has_propagation_node)
                tui_hex_format(state->settings.propagation_node,
                               LXMF_DESTINATION_LENGTH, value);
            else
                (void)snprintf(value, sizeof value, "%s", "none");
            begin_setting_edit(state, value);
            break;
        case TUI_SETTING_ANNOUNCE_NOW:
            (void)tui_state_announce(state);
            break;
        case TUI_SETTING_COUNT:
            break;
    }
}

void tui_state_setting_cancel(tui_state_t *state) {
    if (state == NULL) return;
    tui_editor_clear(&state->setting);
    state->field = TUI_FIELD_NONE;
    tui_state_set_status(state, "Settings edit cancelled");
}

static bool parse_decimal(const char *text, unsigned long maximum,
                          unsigned long *value) {
    char *end = NULL;
    errno = 0;
    unsigned long parsed = strtoul(text, &end, 10);
    if (errno != 0 || text[0] == '\0' || end == NULL || *end != '\0' ||
        parsed > maximum)
        return false;
    *value = parsed;
    return true;
}

bool tui_state_setting_apply(tui_state_t *state) {
    tui_settings_t changed;
    const char *text;
    unsigned long number = 0u;
    if (state == NULL || state->field != TUI_FIELD_SETTING) return false;
    changed = state->settings;
    text = tui_editor_text(&state->setting);
    switch (state->setting_selected) {
        case TUI_SETTING_DISPLAY_NAME:
            changed.display_name_len = tui_editor_length(&state->setting);
            memcpy(changed.display_name, text, changed.display_name_len + 1u);
            break;
        case TUI_SETTING_STAMP_COST:
            if (text[0] == '\0' || strcmp(text, "0") == 0 ||
                strcmp(text, "off") == 0) {
                changed.has_stamp_cost = false;
                changed.stamp_cost = 0u;
            } else if (parse_decimal(text, 254u, &number) && number >= 1u) {
                changed.has_stamp_cost = true;
                changed.stamp_cost = (uint8_t)number;
            } else {
                tui_state_set_status(state, "Stamp cost must be off or 1-254");
                return false;
            }
            break;
        case TUI_SETTING_ANNOUNCE_INTERVAL:
            if (!parse_decimal(text, UINT32_MAX, &number) ||
                number < TUI_SETTINGS_MIN_ANNOUNCE_MINUTES) {
                tui_state_set_status(state, "Announce interval must be at least 30 minutes");
                return false;
            }
            changed.announce_interval_minutes = (uint32_t)number;
            break;
        case TUI_SETTING_PROPAGATION_NODE:
            if (text[0] == '\0' || strcmp(text, "none") == 0) {
                changed.has_propagation_node = false;
                memset(changed.propagation_node, 0,
                       sizeof changed.propagation_node);
            } else if (tui_hex_parse(text, changed.propagation_node,
                                     sizeof changed.propagation_node)) {
                changed.has_propagation_node = true;
            } else {
                tui_state_set_status(state,
                                     "Propagation node must be none or 32 hex characters");
                return false;
            }
            break;
        case TUI_SETTING_ANNOUNCE_AT_START:
        case TUI_SETTING_ANNOUNCE_NOW:
        case TUI_SETTING_COUNT:
            return false;
    }
    if (!tui_settings_valid(&changed) ||
        !tui_settings_save(state->settings_path, &changed)) {
        tui_state_set_status(state, "Could not validate or save settings");
        return false;
    }
    state->settings = changed;
    if (state->router_ready)
        (void)lxmf_router_set_inbound_stamp_cost(
            &state->router, changed.has_stamp_cost ? changed.stamp_cost : 0u);
    state->next_announce_ms = 0u;
    tui_editor_clear(&state->setting);
    state->field = TUI_FIELD_NONE;
    tui_state_set_status(state, "Settings saved");
    return true;
}

/* ------------------------------------------------------------------ network */

size_t tui_state_node_count(const tui_state_t *state) {
    return state != NULL ? state->nodes.count : 0u;
}

size_t tui_state_node_list(const tui_state_t *state, rns_node_record *out,
                           size_t capacity) {
    if (state == NULL || out == NULL || capacity == 0u) return 0u;
    return rns_node_registry_sorted(&state->nodes, out, capacity);
}

bool tui_state_node_serves_pages(const rns_node_record *node) {
    return node != NULL && node->kind == RNS_NODE_KIND_NOMAD;
}

bool tui_state_selected_node(const tui_state_t *state, rns_node_record *record) {
    if (state == NULL || record == NULL || !state->has_node_selection) return false;
    const rns_node_record *found = rns_node_registry_get(&state->nodes,
                                                         state->node_selection);
    if (found == NULL) return false;
    *record = *found;
    return true;
}

size_t tui_state_node_position(const tui_state_t *state) {
    rns_node_record sorted[RNS_NODE_REGISTRY_MAX];
    if (state == NULL || !state->has_node_selection) return 0u;
    size_t count = rns_node_registry_sorted(&state->nodes, sorted, RNS_NODE_REGISTRY_MAX);
    for (size_t i = 0u; i < count; ++i)
        if (memcmp(sorted[i].destination, state->node_selection,
                   LXMF_DESTINATION_LENGTH) == 0) return i;
    return 0u;
}

void tui_state_node_move(tui_state_t *state, int delta) {
    rns_node_record sorted[RNS_NODE_REGISTRY_MAX];
    if (state == NULL) return;
    size_t count = rns_node_registry_sorted(&state->nodes, sorted, RNS_NODE_REGISTRY_MAX);
    if (count == 0u) {
        state->has_node_selection = false;
        return;
    }
    size_t position = tui_state_node_position(state);
    if (!state->has_node_selection) position = 0u;
    else if (delta < 0) {
        size_t back = (size_t)(-delta);
        position = position > back ? position - back : 0u;
    } else {
        position += (size_t)delta;
        if (position >= count) position = count - 1u;
    }
    memcpy(state->node_selection, sorted[position].destination,
           LXMF_DESTINATION_LENGTH);
    state->has_node_selection = true;
}

void tui_state_request_path(tui_state_t *state) {
    rns_node_record node;
    if (state == NULL) return;
    if (state->runtime == NULL) tui_state_set_status(state, "No network runtime");
    else if (!tui_state_selected_node(state, &node))
        tui_state_set_status(state, "No node selected");
    else if (rns_runtime_request_path(state->runtime, node.destination) == RNS_OK)
        tui_state_set_status(state, "Path refresh requested");
    else tui_state_set_status(state, "Path refresh could not be sent");
}

/* ------------------------------------------------------------------ browser */

size_t tui_state_link_count(const tui_state_t *state) {
    return state == NULL ? 0u : rns_micron_link_count(&state->page);
}

const rns_micron_span *tui_state_link(const tui_state_t *state, size_t index) {
    return state == NULL ? NULL : rns_micron_link(&state->page, index);
}

bool tui_state_browse(tui_state_t *state, const char *url, bool push_history) {
    char requested[RNS_MICRON_TEXT_MAX];
    char destination_text[TUI_ADDRESS_DIGITS + 1u];
    uint8_t destination[LXMF_DESTINATION_LENGTH];
    if (state == NULL) return false;
    if (url == NULL || strlen(url) >= sizeof requested) {
        tui_state_set_status(state, "Invalid or oversized link");
        return false;
    }
    memcpy(requested, url, strlen(url) + 1u);
    if (state->browser == NULL) {
        tui_state_set_status(state,
                             "Configure a network interface before browsing remote pages");
        return false;
    }
    if (strlen(requested) < TUI_ADDRESS_DIGITS + 1u) {
        tui_state_set_status(state, "Unsupported browser URL");
        return false;
    }
    memcpy(destination_text, requested, TUI_ADDRESS_DIGITS);
    destination_text[TUI_ADDRESS_DIGITS] = '\0';
    if (!tui_hex_parse(destination_text, destination, sizeof destination)) {
        tui_state_set_status(state, "Invalid Nomad destination");
        return false;
    }
    const rns_node_record *node = rns_node_registry_get(&state->nodes, destination);
    rns_identity identity;
    if (node == NULL || !rns_identity_from_public(&identity, node->public_key)) {
        tui_state_set_status(state, "No verified identity for this Nomad node");
        return false;
    }
    rns_status_t status = rns_browser_open(state->browser, requested, &identity, NULL, 0u);
    if (status != RNS_OK) {
        tui_state_set_status(state, "Page request failed: %s", rns_status_string(status));
        return false;
    }
    if (push_history && !rns_micron_history_push(&state->history, requested)) {
        rns_browser_cancel(state->browser);
        tui_state_set_status(state, "Browser history is full");
        return false;
    }
    (void)snprintf(state->url, sizeof state->url, "%s", requested);
    state->link_selected = 0u;
    state->page_scroll = 0u;
    state->browser_state = rns_browser_state(state->browser);
    tui_state_set_status(state, "Discovering route to Nomad page");
    return true;
}

void tui_state_browse_selected(tui_state_t *state) {
    char url[RNS_MICRON_TEXT_MAX];
    if (state == NULL) return;
    const rns_micron_span *item = tui_state_link(state, state->link_selected);
    if (item == NULL) return;
    const char *target = rns_micron_span_target(&state->page, item);
    /* Pages advertise their author as lxmf@<hash>; following one is a
     * handoff to the conversation screen, not a page fetch. */
    if (strncmp(target, "lxmf@", 5u) == 0) {
        uint8_t peer[LXMF_DESTINATION_LENGTH];
        if (!tui_hex_parse(target + 5u, peer, sizeof peer)) {
            tui_state_set_status(state, "Link carries a malformed LXMF address");
            return;
        }
        if (tui_state_open_conversation(state, peer))
            state->screen = TUI_SCREEN_CONVERSATIONS;
        return;
    }
    if (strncmp(target, "lxmf:", 5u) == 0) {
        tui_state_set_status(state, "LXMF browser links require a destination handoff");
        return;
    }
    if (!rns_micron_normalize_url(state->url, target, url, sizeof url)) {
        tui_state_set_status(state, "Invalid or oversized link");
        return;
    }
    (void)tui_state_browse(state, url, true);
}

void tui_state_browse_back(tui_state_t *state) {
    if (state == NULL) return;
    const char *url = rns_micron_history_back(&state->history);
    if (url != NULL) (void)tui_state_browse(state, url, false);
}

void tui_state_browse_node(tui_state_t *state, const rns_node_record *node) {
    char hash[TUI_ADDRESS_DIGITS + 1u];
    char url[RNS_MICRON_TEXT_MAX];
    if (state == NULL || node == NULL) return;
    /*
     * Most announces are LXMF inboxes or transport nodes that serve no pages;
     * requesting one just times out, so say so instead.
     */
    if (!tui_state_node_serves_pages(node)) {
        tui_state_set_status(state,
                             "This announce is not a Nomad Network node and serves no pages");
        state->overlay = TUI_OVERLAY_NONE;
        return;
    }
    tui_hex_format(node->destination, LXMF_DESTINATION_LENGTH, hash);
    (void)snprintf(url, sizeof url, "%s:/page/index.mu", hash);
    (void)tui_state_browse(state, url, true);
    state->screen = TUI_SCREEN_BROWSER;
    state->overlay = TUI_OVERLAY_NONE;
}

bool tui_state_browse_cancel(tui_state_t *state) {
    if (state == NULL || state->browser == NULL) return false;
    rns_browser_state_t current = rns_browser_state(state->browser);
    if (current == RNS_BROWSER_COMPLETE || current == RNS_BROWSER_FAILED) return false;
    rns_browser_cancel(state->browser);
    state->browser_state = RNS_BROWSER_CANCELLED;
    tui_state_set_status(state, "Page request cancelled");
    return true;
}
