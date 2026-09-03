#include "tui_state.h"

#include "tui_text.h"

#include "reticulum/config.h"
#include "reticulum/destination.h"
#include "reticulum/hal.h"
#include "reticulum/lxmf.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define TUI_ROUTER_INTERVAL_MS 1000u
#define TUI_ANNOUNCE_INTERVAL_MS (5u * 60u * 1000u)
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

static void on_announce(rns_runtime_t *runtime, const rns_node_result *announce,
                        void *context) {
    tui_state_t *state = context;
    (void)runtime;
    (void)rns_node_registry_consider_announce(&state->nodes, announce);
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
    (void)runtime;
    (void)result;
    if (state->router_ready)
        (void)lxmf_router_receive_packet(&state->router, packet, length);
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
            .resolve_identity = resolve_peer,
            .resolve_context = state,
            .send_packet = send_via_runtime,
            .send_context = state,
            .message_callback = on_message,
            .message_context = state
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

static const uint8_t tui_home_page[] =
    "# Nomad Browser\n"
    "Native Micron navigation is ready.\n"
    "[Network nodes](/network)\n"
    "[Guide](/guide)\n";

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
    written = snprintf(state->peer_store_path, sizeof state->peer_store_path,
                       "%s.peers", store_path);
    if (written <= 0 || (size_t)written >= sizeof state->peer_store_path ||
        lxmf_peer_store_open(&state->peer_store, state->peer_store_path) != LXMF_OK ||
        lxmf_peer_store_list(&state->peer_store, load_peer, state) != LXMF_OK) goto fail;
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
    if (state->runtime != NULL &&
        rns_browser_create(&state->browser, state->runtime, NULL) != RNS_OK)
        tui_state_set_status(state, "Page browser unavailable; messaging remains active");

    rns_micron_history_init(&state->history);
    (void)snprintf(state->url, sizeof state->url, "nomad://local/home");
    if (!rns_micron_parse(&state->page, tui_home_page, sizeof tui_home_page - 1u) ||
        !rns_micron_history_push(&state->history, state->url)) goto fail;
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
    rns_browser_destroy(state->browser);
    state->browser = NULL;
    rns_runtime_destroy(state->runtime);
    state->runtime = NULL;
    tui_state_persist_contacts(state);
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
        tui_state_set_status(state, "Remote Nomad page loaded");
    } else if (current == RNS_BROWSER_FAILED) {
        tui_state_set_status(state, "Page load failed: %s",
                             rns_status_string(rns_browser_error(state->browser)));
    }
}

void tui_state_poll(tui_state_t *state) {
    uint64_t now = 0u;
    size_t processed = 0u;
    if (state == NULL || state->runtime == NULL) return;
    (void)rns_runtime_poll(state->runtime, TUI_RUNTIME_BATCH, &processed);
    poll_browser(state);
    if (rns_hal_monotonic_ms(&now) == RNS_OK) {
        (void)rns_node_registry_expire(&state->nodes, (double)now / 1000.0);
        /* Announce our inbox so peers can discover us and route replies back. */
        if (state->router_ready &&
            (state->last_announce_ms == 0u ||
             now - state->last_announce_ms >= TUI_ANNOUNCE_INTERVAL_MS)) {
            static const char *const delivery[] = {"delivery"};
            if (rns_runtime_announce(state->runtime, &state->identity, "lxmf",
                                     delivery, 1u, NULL, 0u) == RNS_OK)
                state->last_announce_ms = now;
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
    size_t count = 0u;
    if (state == NULL) return 0u;
    for (size_t i = 0u; i < state->page.count; ++i)
        if (state->page.items[i].kind == RNS_MICRON_LINK) ++count;
    return count;
}

const rns_micron_item *tui_state_link(const tui_state_t *state, size_t index) {
    size_t seen = 0u;
    if (state == NULL) return NULL;
    for (size_t i = 0u; i < state->page.count; ++i) {
        if (state->page.items[i].kind != RNS_MICRON_LINK) continue;
        if (seen++ == index) return &state->page.items[i];
    }
    return NULL;
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
    state->browser_state = rns_browser_state(state->browser);
    tui_state_set_status(state, "Discovering route to Nomad page");
    return true;
}

void tui_state_browse_selected(tui_state_t *state) {
    char url[RNS_MICRON_TEXT_MAX];
    if (state == NULL) return;
    const rns_micron_item *item = tui_state_link(state, state->link_selected);
    if (item == NULL) return;
    if (strncmp(item->target, "lxmf:", 5u) == 0) {
        tui_state_set_status(state, "LXMF browser links require a destination handoff");
        return;
    }
    if (!rns_micron_normalize_url(state->url, item->target, url, sizeof url)) {
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
