#include "tui_state.h"

#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static const uint8_t local_address[LXMF_DESTINATION_LENGTH] = {0x11u};

/*
 * Builds a state without touching the store or the network so the pure
 * selection, filtering and scrolling rules can be exercised directly.
 */
static tui_state_t *make_state(void) {
    tui_state_t *state = calloc(1u, sizeof *state);
    assert(state != NULL);
    state->messages = calloc(TUI_MAX_MESSAGES, sizeof *state->messages);
    assert(state->messages != NULL);
    memcpy(state->local, local_address, sizeof state->local);
    tui_editor_init(&state->composer, TUI_COMPOSER_CAPACITY);
    tui_editor_init(&state->search, TUI_SEARCH_CAPACITY);
    tui_editor_init(&state->address, TUI_ADDRESS_DIGITS);
    tui_editor_init(&state->setting, LXMF_DISPLAY_NAME_MAX);
    tui_settings_defaults(&state->settings);
    state->tab = TUI_TRUST_UNKNOWN;
    state->filter_dirty = true;
    return state;
}

static void destroy_state(tui_state_t *state) {
    free(state->messages);
    free(state);
}

static void add_contact(tui_state_t *state, uint8_t tag, tui_trust_t trust) {
    tui_contact_t *contact = &state->contacts[state->contact_count++];
    memset(contact, 0, sizeof *contact);
    contact->peer[0] = tag;
    contact->trust = trust;
    state->filter_dirty = true;
}

static void add_message(tui_state_t *state, uint8_t peer_tag, bool outgoing,
                        const char *content) {
    tui_message_t *message = &state->messages[state->message_count++];
    memset(message, 0, sizeof *message);
    size_t length = strlen(content);
    memcpy(message->content, content, length);
    message->value.content.data = message->content;
    message->value.content.len = length;
    message->value.status = LXMF_DELIVERY_QUEUED;
    if (outgoing) {
        memcpy(message->value.source, local_address, LXMF_DESTINATION_LENGTH);
        message->value.destination[0] = peer_tag;
    } else {
        message->value.source[0] = peer_tag;
        memcpy(message->value.destination, local_address, LXMF_DESTINATION_LENGTH);
    }
    size_t index = 0u;
    while (index < state->contact_count && state->contacts[index].peer[0] != peer_tag)
        ++index;
    assert(index < state->contact_count);
    state->contacts[index].messages++;
    state->filter_dirty = true;
}

static void test_trust_tabs(void) {
    tui_state_t *state = make_state();
    add_contact(state, 0xa1u, TUI_TRUST_UNKNOWN);
    add_contact(state, 0xa2u, TUI_TRUST_TRUSTED);
    add_contact(state, 0xa3u, TUI_TRUST_UNKNOWN);
    tui_state_refresh(state);
    /* Only the current tab is listed. */
    assert(state->visible_count == 2u);
    assert(state->visible[0] == 0u && state->visible[1] == 2u);
    tui_state_set_tab(state, TUI_TRUST_TRUSTED);
    tui_state_refresh(state);
    assert(state->visible_count == 1u);
    assert(state->selected == 1u);
    tui_state_set_tab(state, TUI_TRUST_UNTRUSTED);
    tui_state_refresh(state);
    assert(state->visible_count == 0u);
    destroy_state(state);
}

static void test_thread_and_search(void) {
    tui_state_t *state = make_state();
    add_contact(state, 0xa1u, TUI_TRUST_UNKNOWN);
    add_contact(state, 0xa2u, TUI_TRUST_UNKNOWN);
    add_message(state, 0xa1u, true, "hello from me");
    add_message(state, 0xa1u, false, "reply about reticulum");
    add_message(state, 0xa2u, true, "unrelated thread");
    state->selected = 0u;
    tui_state_refresh(state);
    /* The thread holds only the selected conversation. */
    assert(tui_state_thread_count(state) == 2u);
    assert(tui_state_outgoing(state, tui_state_thread_message(state, 0u)));
    assert(!tui_state_outgoing(state, tui_state_thread_message(state, 1u)));

    /* Search narrows both the contact list and the visible thread. */
    assert(tui_editor_insert(&state->search, "RETICULUM", 9u));
    state->filter_dirty = true;
    tui_state_refresh(state);
    assert(state->visible_count == 1u && state->visible[0] == 0u);
    assert(tui_state_thread_count(state) == 1u);
    assert(tui_state_thread_message(state, 1u) == NULL);

    tui_editor_clear(&state->search);
    state->filter_dirty = true;
    tui_state_refresh(state);
    assert(state->visible_count == 2u);
    destroy_state(state);
}

static void test_selection_cycles(void) {
    tui_state_t *state = make_state();
    add_contact(state, 0xa1u, TUI_TRUST_UNKNOWN);
    add_contact(state, 0xa2u, TUI_TRUST_UNKNOWN);
    state->contacts[1].unread = 3u;
    state->selected = 0u;
    tui_state_refresh(state);
    tui_state_select_offset(state, 1);
    assert(state->selected == 1u);
    /* Selecting a conversation clears its unread marker. */
    assert(state->contacts[1].unread == 0u);
    tui_state_select_offset(state, 1);
    assert(state->selected == 0u);
    tui_state_select_offset(state, -1);
    assert(state->selected == 1u);
    destroy_state(state);
}

static void test_scroll_is_clamped(void) {
    tui_state_t *state = make_state();
    add_contact(state, 0xa1u, TUI_TRUST_UNKNOWN);
    add_message(state, 0xa1u, true, "one");
    add_message(state, 0xa1u, true, "two");
    state->selected = 0u;
    tui_state_refresh(state);
    /* Scrolling back never exceeds the thread length. */
    tui_state_scroll_by(state, 100);
    assert(state->scroll == tui_state_thread_count(state));
    tui_state_scroll_by(state, 100);
    assert(state->scroll == 2u);
    /* Scrolling forward never underflows. */
    tui_state_scroll_by(state, -100);
    assert(state->scroll == 0u);
    tui_state_scroll_by(state, -1);
    assert(state->scroll == 0u);
    destroy_state(state);
}

static void test_open_conversation(void) {
    tui_state_t *state = make_state();
    uint8_t peer[LXMF_DESTINATION_LENGTH] = {0xb7u};
    add_contact(state, 0xa1u, TUI_TRUST_TRUSTED);
    state->tab = TUI_TRUST_TRUSTED;
    assert(tui_state_open_conversation(state, peer));
    /* A new conversation is created, selected, and opens the composer. */
    assert(state->contact_count == 2u);
    assert(state->selected == 1u);
    assert(state->tab == TUI_TRUST_UNKNOWN);
    assert(state->screen == TUI_SCREEN_CONVERSATIONS);
    assert(state->field == TUI_FIELD_COMPOSE);
    /* Reopening the same peer reuses the existing conversation. */
    assert(tui_state_open_conversation(state, peer));
    assert(state->contact_count == 2u);
    destroy_state(state);
}

static void test_contact_table_bound(void) {
    tui_state_t *state = make_state();
    uint8_t peer[LXMF_DESTINATION_LENGTH] = {0};
    for (size_t i = 0u; i < TUI_MAX_CONTACTS; ++i)
        add_contact(state, (uint8_t)(i + 1u), TUI_TRUST_UNKNOWN);
    peer[0] = 0xffu;
    peer[1] = 0xffu;
    assert(!tui_state_open_conversation(state, peer));
    assert(state->contact_count == TUI_MAX_CONTACTS);
    assert(strstr(state->status, "limit") != NULL);
    destroy_state(state);
}

static void set_peer_text(char *target, size_t *target_length,
                          size_t capacity, const char *text) {
    size_t length = strlen(text);
    assert(length < capacity);
    memcpy(target, text, length + 1u);
    *target_length = length;
}

static void test_contact_persistence_preserves_peer_metadata(void) {
    char path[] = "/tmp/nomad-state-peers-XXXXXX";
    int descriptor = mkstemp(path);
    assert(descriptor >= 0);
    assert(close(descriptor) == 0);
    assert(unlink(path) == 0);

    tui_state_t *state = make_state();
    assert(lxmf_peer_store_open(&state->peer_store, path) == LXMF_OK);

    lxmf_peer_t original = {0};
    original.address[0] = 0xacu;
    set_peer_text(original.display_name, &original.display_name_len,
                  sizeof original.display_name, "Remote Nomad");
    set_peer_text(original.note, &original.note_len, sizeof original.note,
                  "old note");
    set_peer_text(original.draft, &original.draft_len, sizeof original.draft,
                  "unfinished remote draft");
    original.propagation = LXMF_PEER_PROPAGATION_PREFERRED;
    original.has_propagation_node = true;
    original.propagation_node[0] = 0x5au;
    original.last_seen_ms = 123456u;
    original.last_announce_ms = 123000u;
    bool inserted = false;
    assert(lxmf_peer_store_put(&state->peer_store, &original, &inserted) == LXMF_OK);
    assert(inserted);

    add_contact(state, original.address[0], TUI_TRUST_TRUSTED);
    tui_contact_t *contact = &state->contacts[0];
    contact->blocked = true;
    contact->pinned = true;
    contact->unread = 7u;
    memcpy(contact->note, "new note", sizeof "new note");
    tui_state_persist_contacts(state);

    lxmf_peer_store_close(&state->peer_store);
    assert(lxmf_peer_store_open(&state->peer_store, path) == LXMF_OK);
    lxmf_peer_t stored = {0};
    assert(lxmf_peer_store_get(&state->peer_store, original.address, &stored) ==
           LXMF_OK);
    assert(stored.trust == LXMF_PEER_TRUST_TRUSTED);
    assert(stored.blocked && stored.pinned && stored.unread_count == 7u);
    assert(strcmp(stored.note, "new note") == 0);
    assert(strcmp(stored.display_name, original.display_name) == 0);
    assert(strcmp(stored.draft, original.draft) == 0);
    assert(stored.propagation == original.propagation);
    assert(stored.has_propagation_node == original.has_propagation_node);
    assert(memcmp(stored.propagation_node, original.propagation_node,
                  sizeof stored.propagation_node) == 0);
    assert(stored.last_seen_ms == original.last_seen_ms);
    assert(stored.last_announce_ms == original.last_announce_ms);

    lxmf_peer_store_close(&state->peer_store);
    assert(unlink(path) == 0);
    destroy_state(state);
}

static void add_node(tui_state_t *state, uint8_t tag, uint8_t hops,
                     rns_node_kind kind, double seen) {
    rns_node_record r;
    memset(&r, 0, sizeof r);
    r.destination[0] = tag;
    r.hops = hops;
    r.kind = kind;
    r.seen_at = seen;
    r.expires_at = seen + 3600.0;
    r.reachable = true;
    assert(rns_node_registry_upsert(&state->nodes, &r) != 0);
}

static void test_node_selection_survives_resort(void) {
    tui_state_t *state = make_state();
    rns_node_registry_init(&state->nodes, 3600.0);
    add_node(state, 0xa1u, 5u, RNS_NODE_KIND_OTHER, 100.0);
    add_node(state, 0xa2u, 3u, RNS_NODE_KIND_NOMAD, 100.0);
    add_node(state, 0xa3u, 9u, RNS_NODE_KIND_LXMF, 100.0);

    tui_state_node_move(state, 0);
    assert(state->has_node_selection);
    tui_state_node_move(state, 1);
    rns_node_record chosen;
    assert(tui_state_selected_node(state, &chosen));
    uint8_t tag = chosen.destination[0];

    /*
     * A closer node arriving re-sorts the list. The cursor must stay on the
     * node the user picked, not on whatever now occupies that row.
     */
    add_node(state, 0xb9u, 1u, RNS_NODE_KIND_NOMAD, 200.0);
    rns_node_record after;
    assert(tui_state_selected_node(state, &after));
    assert(after.destination[0] == tag);
    destroy_state(state);
}

static void test_verified_peer_stamp_cost_resolution(void) {
    tui_state_t *state = make_state();
    rns_node_registry_init(&state->nodes, 3600.0);
    uint8_t destination[LXMF_DESTINATION_LENGTH] = {0x42u};
    uint8_t cost = 99u;
    add_node(state, 0xa1u, 1u, RNS_NODE_KIND_LXMF, 100.0);
    rns_node_record *node = &state->nodes.records[0];
    memcpy(node->message_destination, destination, sizeof destination);
    node->has_message_destination = true;
    node->lxmf_has_stamp_cost = true;
    node->lxmf_stamp_cost = 8u;
    assert(!tui_state_peer_stamp_cost(state, destination, &cost));
    assert(cost == 0u);
    node->lxmf_app_data_valid = true;
    assert(tui_state_peer_stamp_cost(state, destination, &cost));
    assert(cost == 8u);
    node->kind = RNS_NODE_KIND_NOMAD;
    assert(!tui_state_peer_stamp_cost(state, destination, &cost));
    assert(cost == 0u);
    destroy_state(state);
}

static void test_node_move_clamps(void) {
    tui_state_t *state = make_state();
    rns_node_registry_init(&state->nodes, 3600.0);
    add_node(state, 0xa1u, 1u, RNS_NODE_KIND_NOMAD, 100.0);
    add_node(state, 0xa2u, 2u, RNS_NODE_KIND_NOMAD, 100.0);
    add_node(state, 0xa3u, 3u, RNS_NODE_KIND_NOMAD, 100.0);
    tui_state_node_move(state, 0);
    assert(tui_state_node_position(state) == 0u);
    tui_state_node_move(state, -5);
    assert(tui_state_node_position(state) == 0u);
    tui_state_node_move(state, 99);
    assert(tui_state_node_position(state) == 2u);
    tui_state_node_move(state, 1);
    assert(tui_state_node_position(state) == 2u);
    destroy_state(state);
}

static void test_only_nomad_nodes_serve_pages(void) {
    rns_node_record nomad = {0}, lxmf = {0}, other = {0};
    nomad.kind = RNS_NODE_KIND_NOMAD;
    lxmf.kind = RNS_NODE_KIND_LXMF;
    other.kind = RNS_NODE_KIND_OTHER;
    assert(tui_state_node_serves_pages(&nomad));
    assert(!tui_state_node_serves_pages(&lxmf));
    assert(!tui_state_node_serves_pages(&other));
    assert(!tui_state_node_serves_pages(NULL));

    /* Browsing a destination that serves no pages is refused up front. */
    tui_state_t *state = make_state();
    rns_node_registry_init(&state->nodes, 3600.0);
    tui_state_browse_node(state, &lxmf);
    assert(strstr(state->status, "serves no pages") != NULL);
    destroy_state(state);
}

static void test_empty_registry_has_no_selection(void) {
    tui_state_t *state = make_state();
    rns_node_registry_init(&state->nodes, 3600.0);
    rns_node_record record;
    tui_state_node_move(state, 1);
    assert(!state->has_node_selection);
    assert(!tui_state_selected_node(state, &record));
    assert(tui_state_node_position(state) == 0u);
    destroy_state(state);
}

/* Link selection and page scrolling work on the parsed page, not on raw text. */
static void test_browser_links_and_scroll(void) {
    static const char markup[] =
        ">Index\n"
        "intro `[First`/page/a.mu] middle `[Second`/page/b.mu]\n"
        "tail\n";
    tui_state_t *state = make_state();
    assert(rns_micron_parse(&state->page, (const uint8_t *)markup,
                            sizeof markup - 1u) == 1);
    assert(tui_state_link_count(state) == 2u);
    const rns_micron_span *first = tui_state_link(state, 0u);
    assert(first != NULL);
    assert(strcmp(rns_micron_span_text(&state->page, first), "First") == 0);
    assert(strcmp(rns_micron_span_target(&state->page, first), "/page/a.mu") == 0);
    assert(tui_state_link(state, 2u) == NULL);

    /* Page scrolling runs the opposite way to the upward-growing thread list. */
    state->screen = TUI_SCREEN_BROWSER;
    tui_state_scroll_by(state, -2);
    assert(state->page_scroll == 2u);
    tui_state_scroll_by(state, -50);
    assert(state->page_scroll == (size_t)state->page.line_count - 1u);
    tui_state_scroll_by(state, 100);
    assert(state->page_scroll == 0u);
    destroy_state(state);
}

static void test_settings_navigation_and_edits(void) {
    char path[] = "/tmp/nomad-state-settings-XXXXXX";
    int descriptor = mkstemp(path);
    assert(descriptor >= 0);
    assert(close(descriptor) == 0);
    assert(unlink(path) == 0);
    tui_state_t *state = make_state();
    (void)snprintf(state->settings_path, sizeof state->settings_path, "%s", path);

    tui_state_setting_move(state, -1);
    assert(state->setting_selected == TUI_SETTING_ANNOUNCE_NOW);
    tui_state_setting_move(state, 1);
    assert(state->setting_selected == TUI_SETTING_DISPLAY_NAME);
    tui_state_setting_activate(state);
    assert(state->field == TUI_FIELD_SETTING);
    tui_editor_clear(&state->setting);
    assert(tui_editor_insert(&state->setting, "Rei", 3u));
    assert(tui_state_setting_apply(state));
    assert(strcmp(state->settings.display_name, "Rei") == 0);
    assert(state->field == TUI_FIELD_NONE);

    state->setting_selected = TUI_SETTING_ANNOUNCE_INTERVAL;
    tui_state_setting_activate(state);
    tui_editor_clear(&state->setting);
    assert(tui_editor_insert(&state->setting, "29", 2u));
    assert(!tui_state_setting_apply(state));
    assert(state->field == TUI_FIELD_SETTING);
    assert(state->settings.announce_interval_minutes == 360u);
    tui_state_setting_cancel(state);
    assert(state->field == TUI_FIELD_NONE);

    state->setting_selected = TUI_SETTING_ANNOUNCE_NOW;
    tui_state_setting_activate(state);
    assert(state->has_announce_result);
    assert(state->last_announce_result == RNS_ERROR_INVALID_STATE);
    assert(strstr(state->status, "offline") != NULL);

    tui_settings_t persisted;
    bool found = false;
    assert(tui_settings_load(path, &persisted, &found) && found);
    assert(strcmp(persisted.display_name, "Rei") == 0);
    assert(unlink(path) == 0);
    destroy_state(state);
}

static void test_router_events_update_visible_state(void) {
    tui_state_t *state = make_state();
    add_contact(state, 0xa1u, TUI_TRUST_UNKNOWN);
    add_message(state, 0xa1u, true, "pending message");
    state->messages[0].value.message_id[0] = 0x42u;
    lxmf_router_event_t event = {
        .method = LXMF_DELIVERY_METHOD_OPPORTUNISTIC,
        .state = LXMF_DELIVERY_SENT,
        .queue_reason = LXMF_QUEUE_REASON_NONE,
        .result = LXMF_OK
    };
    event.message_id[0] = 0x42u;
    tui_state_apply_router_event(state, &event);
    assert(state->messages[0].value.status == LXMF_DELIVERY_SENT);
    assert(strstr(state->status, "awaiting delivery proof") != NULL);
    event.state = LXMF_DELIVERY_DELIVERED;
    tui_state_apply_router_event(state, &event);
    assert(state->messages[0].value.status == LXMF_DELIVERY_DELIVERED);
    assert(strstr(state->status, "Delivered") != NULL);

    state->messages[0].value.signature_state = LXMF_SIGNATURE_UNVERIFIED;
    tui_state_apply_signature(state, event.message_id, LXMF_SIGNATURE_VERIFIED);
    assert(state->messages[0].value.signature_state == LXMF_SIGNATURE_VERIFIED);
    tui_state_apply_signature(state, event.message_id, LXMF_SIGNATURE_FAILED);
    assert(state->message_count == 0u);
    assert(state->contacts[0].messages == 0u);
    assert(strstr(state->status, "invalid signature") != NULL);
    destroy_state(state);
}

static void test_rejected_message_keeps_owned_previews(void) {
    tui_state_t *state = make_state();
    add_contact(state, 0xa1u, TUI_TRUST_UNKNOWN);
    add_message(state, 0xa1u, false, "rejected");
    add_message(state, 0xa1u, false, "second");
    add_message(state, 0xa1u, false, "third message");
    for (size_t i = 0u; i < 3u; ++i)
        state->messages[i].value.message_id[0] = (uint8_t)(i + 1u);
    uint8_t rejected[LXMF_MESSAGE_ID_LENGTH] = {1u};
    tui_state_apply_signature(state, rejected, LXMF_SIGNATURE_FAILED);
    assert(state->message_count == 2u);
    assert(state->messages[0].value.content.data == state->messages[0].content);
    assert(state->messages[1].value.content.data == state->messages[1].content);
    assert(memcmp(state->messages[0].value.content.data, "second", 6u) == 0);
    assert(memcmp(state->messages[1].value.content.data, "third message", 13u) == 0);
    rejected[0] = 2u;
    tui_state_apply_signature(state, rejected, LXMF_SIGNATURE_FAILED);
    assert(state->message_count == 1u);
    assert(memcmp(state->messages[0].value.content.data, "third message", 13u) == 0);
    destroy_state(state);
}

int main(void) {
    test_rejected_message_keeps_owned_previews();
    test_verified_peer_stamp_cost_resolution();
    test_node_selection_survives_resort();
    test_node_move_clamps();
    test_only_nomad_nodes_serve_pages();
    test_empty_registry_has_no_selection();
    test_browser_links_and_scroll();
    test_settings_navigation_and_edits();
    test_router_events_update_visible_state();
    test_trust_tabs();
    test_thread_and_search();
    test_selection_cycles();
    test_scroll_is_clamped();
    test_open_conversation();
    test_contact_table_bound();
    test_contact_persistence_preserves_peer_metadata();
    return 0;
}
