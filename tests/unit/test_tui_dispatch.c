#include "tui.h"
#include "tui_render.h"
#include "tui_state.h"

#include <assert.h>
#include <curses.h>
#include <stdlib.h>
#include <string.h>

static tui_state_t *state_create(void) {
    tui_state_t *state = calloc(1u, sizeof *state);
    assert(state != NULL);
    tui_editor_init(&state->composer, TUI_COMPOSER_CAPACITY);
    tui_editor_init(&state->search, TUI_SEARCH_CAPACITY);
    tui_editor_init(&state->node_search, TUI_SEARCH_CAPACITY);
    tui_editor_init(&state->address, TUI_ADDRESS_DIGITS);
    tui_editor_init(&state->setting, LXMF_DISPLAY_NAME_MAX);
    tui_settings_defaults(&state->settings);
    tui_rrc_init(&state->rrc);
    rns_node_registry_init(&state->nodes, 3600.0);
    state->contact_count = 2u;
    state->contacts[0].peer[0] = 0x11u;
    state->contacts[1].peer[0] = 0x22u;
    state->contacts[0].trust = TUI_TRUST_UNKNOWN;
    state->contacts[1].trust = TUI_TRUST_UNKNOWN;
    state->tab = TUI_TRUST_UNKNOWN;
    state->filter_dirty = true;
    tui_state_refresh(state);
    assert(state->visible_count == 2u);
    return state;
}

static void seed_node(tui_state_t *state) {
    rns_node_record node = {0};
    node.destination[0] = 0x33u;
    node.message_destination[0] = 0x44u;
    node.has_message_destination = true;
    node.kind = RNS_NODE_KIND_NOMAD;
    node.reachable = true;
    node.seen_at = 10.0;
    node.expires_at = 3610.0;
    assert(rns_node_registry_upsert(&state->nodes, &node));
    tui_state_node_move(state, 0);
}

static void test_screen_scoping(void) {
    for (int screen = 0; screen < TUI_SCREEN_COUNT; ++screen) {
        tui_state_t *state = state_create();
        state->screen = (tui_screen_t)screen;
        state->scroll = 7u;
        state->page.line_count = 30u;
        state->page_scroll = 10u;
        assert(tui_dispatch_key(state, ERR));
        assert(tui_dispatch_key(state, KEY_RESIZE));
        assert(state->screen == (tui_screen_t)screen);
        if (screen != TUI_SCREEN_CONVERSATIONS) {
            assert(tui_dispatch_key(state, 'i'));
            assert(state->overlay == TUI_OVERLAY_NONE);
            assert(tui_dispatch_key(state, 'p'));
            assert(tui_dispatch_key(state, 'x'));
            assert(tui_dispatch_key(state, 't'));
            assert(tui_dispatch_key(state, 'u'));
            assert(tui_dispatch_key(state, 'n'));
            assert(!state->contacts[0].pinned && !state->contacts[0].blocked);
            assert(state->contacts[0].trust == TUI_TRUST_UNKNOWN);
            assert(state->contacts[0].note[0] == '\0');
            assert(tui_dispatch_key(state, KEY_PPAGE));
            assert(tui_dispatch_key(state, KEY_NPAGE));
            assert(state->scroll == 7u);
            assert(state->page_scroll == 10u);
        }
        size_t selected = state->selected;
        assert(tui_dispatch_key(state, 'j'));
        if (screen == TUI_SCREEN_CONVERSATIONS) assert(state->selected != selected);
        else assert(state->selected == selected);
        assert(tui_dispatch_key(state, 'k'));
        assert(state->selected == selected);
        if (screen != TUI_SCREEN_CONVERSATIONS && screen != TUI_SCREEN_SETTINGS &&
            screen != TUI_SCREEN_RRC) {
            assert(tui_dispatch_key(state, '\n'));
            assert(state->field == TUI_FIELD_NONE && state->overlay == TUI_OVERLAY_NONE);
            assert(state->screen == (tui_screen_t)screen);
        }
        assert(tui_dispatch_key(state, '?'));
        if (screen == TUI_SCREEN_CONVERSATIONS) {
            assert(state->overlay == TUI_OVERLAY_HELP);
            assert(tui_dispatch_key(state, 27));
        } else {
            assert(state->overlay == TUI_OVERLAY_NONE);
            assert(state->status[0] != '\0');
        }
        if (screen != TUI_SCREEN_CONVERSATIONS) {
            assert(tui_dispatch_key(state, 27));
            assert(state->screen == TUI_SCREEN_CONVERSATIONS);
        }
        assert(!tui_dispatch_key(state, 27));
        free(state);
    }
}

static void test_empty_network_and_filtered_contact(void) {
    tui_state_t *state = state_create();
    state->screen = TUI_SCREEN_NETWORK;
    assert(tui_dispatch_key(state, '\n'));
    assert(state->screen == TUI_SCREEN_NETWORK);
    assert(state->field == TUI_FIELD_NONE && state->overlay == TUI_OVERLAY_NONE);
    assert(strstr(state->status, "No known nodes") != NULL);
    state->screen = TUI_SCREEN_CONVERSATIONS;
    tui_state_set_tab(state, TUI_TRUST_TRUSTED);
    assert(state->visible_count == 0u && state->contact_count == 2u);
    assert(tui_dispatch_key(state, '\n'));
    assert(tui_dispatch_key(state, 'i'));
    assert(tui_dispatch_key(state, 'x'));
    assert(state->field == TUI_FIELD_NONE && state->overlay == TUI_OVERLAY_NONE);
    assert(!state->contacts[state->selected].blocked);
    free(state);
}

static void test_delivery_shortcut_is_screen_scoped(void) {
    tui_state_t *state = state_create();
    state->compose_delivery_method = LXMF_DELIVERY_METHOD_DIRECT;
    assert(tui_dispatch_key(state, 'd'));
    assert(state->compose_delivery_method == LXMF_DELIVERY_METHOD_PROPAGATED);
    assert(strstr(state->status, "choose a verified node") != NULL);
    state->screen = TUI_SCREEN_NETWORK;
    assert(tui_dispatch_key(state, 'd'));
    assert(state->compose_delivery_method == LXMF_DELIVERY_METHOD_PROPAGATED);
    state->screen = TUI_SCREEN_CONVERSATIONS;
    assert(tui_dispatch_key(state, 'D'));
    assert(state->compose_delivery_method == LXMF_DELIVERY_METHOD_DIRECT);
    free(state);
}

static void test_propagation_sync_actions_are_screen_scoped(void) {
    tui_state_t *state = state_create();
    state->screen = TUI_SCREEN_SETTINGS;
    state->setting_selected = TUI_SETTING_PROPAGATION_SYNC;
    assert(tui_dispatch_key(state, '\n'));
    assert(state->field == TUI_FIELD_NONE);
    assert(strstr(state->status, "runtime is offline") != NULL);

    state->screen = TUI_SCREEN_NETWORK;
    seed_node(state);
    assert(tui_dispatch_key(state, '\n'));
    assert(state->overlay == TUI_OVERLAY_NODE_ACTIONS);
    assert(tui_dispatch_key(state, 's'));
    assert(state->overlay == TUI_OVERLAY_NONE);
    assert(strstr(state->status, "Use p to select") != NULL);

    state->propagation_sync.active = true;
    assert(tui_dispatch_key(state, '\n'));
    assert(state->overlay == TUI_OVERLAY_NODE_ACTIONS);
    assert(tui_dispatch_key(state, 's'));
    assert(state->overlay == TUI_OVERLAY_NONE);
    assert(strstr(state->status, "Cannot cancel sync") != NULL);
    free(state);
}

static void test_event_log_screen(void) {
    tui_state_t *state = state_create();
    tui_state_set_status(state, "one");
    tui_state_set_status(state, "two");
    assert(tui_dispatch_key(state, 'L'));
    assert(state->screen == TUI_SCREEN_LOGS);
    assert(tui_state_log_position(state) == 1u);
    assert(tui_dispatch_key(state, 'k'));
    assert(tui_state_log_position(state) == 0u);
    assert(tui_dispatch_key(state, 'j'));
    assert(tui_state_log_position(state) == 1u);
    assert(tui_dispatch_key(state, 'x'));
    assert(tui_state_log_count(state) == 0u);
    assert(strcmp(state->status, "Event log cleared") == 0);
    assert(!state->contacts[0].blocked);
    assert(tui_dispatch_key(state, 27));
    assert(state->screen == TUI_SCREEN_CONVERSATIONS);
    free(state);
}

static void test_hidden_editors(void) {
    for (int screen = 0; screen < TUI_SCREEN_COUNT; ++screen) {
        for (int field = TUI_FIELD_COMPOSE; field <= TUI_FIELD_RRC; ++field) {
            bool visible = field == TUI_FIELD_SETTING
                             ? screen == TUI_SCREEN_SETTINGS
                             : field == TUI_FIELD_RRC
                                   ? screen == TUI_SCREEN_RRC
                             : field == TUI_FIELD_NODE_SEARCH
                                   ? screen == TUI_SCREEN_NETWORK
                                   : screen == TUI_SCREEN_CONVERSATIONS;
            if (visible) continue;
            tui_state_t *state = state_create();
            state->screen = (tui_screen_t)screen;
            state->field = (tui_field_t)field;
            assert(tui_editor_insert_byte(&state->composer, 'd'));
            assert(tui_editor_insert_byte(&state->setting, 'v'));
            assert(tui_editor_insert_byte(&state->address, 'a'));
            assert(tui_editor_insert_byte(&state->search, 's'));
            assert(tui_editor_insert_byte(&state->node_search, 'n'));
            tui_settings_t saved = state->settings;
            assert(tui_dispatch_key(state, '\n'));
            assert(state->screen == (tui_screen_t)screen);
            assert(state->field == TUI_FIELD_NONE && state->overlay == TUI_OVERLAY_NONE);
            assert(strcmp(tui_editor_text(&state->composer), "d") == 0);
            assert(strcmp(tui_editor_text(&state->setting), "v") == 0);
            assert(strcmp(tui_editor_text(&state->address), "a") == 0);
            assert(strcmp(tui_editor_text(&state->search), "s") == 0);
            assert(strcmp(tui_editor_text(&state->node_search), "n") == 0);
            assert(memcmp(&saved, &state->settings, sizeof saved) == 0);
            assert(state->message_count == 0u);
            free(state);
        }
    }
}

static void test_modal_isolation(void) {
    for (int screen = 0; screen < TUI_SCREEN_COUNT; ++screen) {
        for (int overlay = TUI_OVERLAY_HELP; overlay <= TUI_OVERLAY_NODE_ACTIONS; ++overlay) {
            tui_state_t *state = state_create();
            state->screen = (tui_screen_t)screen;
            seed_node(state);
            state->overlay = (tui_overlay_t)overlay;
            bool visible = overlay == TUI_OVERLAY_NODE_ACTIONS
                             ? screen == TUI_SCREEN_NETWORK
                             : screen == TUI_SCREEN_CONVERSATIONS;
            size_t selected = state->selected;
            assert(tui_dispatch_key(state, 'j'));
            assert(state->selected == selected);
            assert(state->overlay == (visible ? (tui_overlay_t)overlay : TUI_OVERLAY_NONE));
            if (visible) {
                assert(tui_dispatch_key(state, 'N'));
                assert(state->screen == (tui_screen_t)screen);
                assert(tui_dispatch_key(state, 27));
                assert(state->overlay == TUI_OVERLAY_NONE);
            }
            assert(state->field == TUI_FIELD_NONE);
            free(state);
        }
    }
    tui_state_t *state = state_create();
    state->overlay = TUI_OVERLAY_HELP;
    state->field = TUI_FIELD_COMPOSE;
    assert(tui_editor_insert_byte(&state->composer, 'd'));
    assert(tui_dispatch_key(state, 27));
    assert(state->overlay == TUI_OVERLAY_NONE && state->field == TUI_FIELD_COMPOSE);
    assert(tui_dispatch_key(state, 27));
    assert(state->field == TUI_FIELD_NONE);
    assert(strcmp(tui_editor_text(&state->composer), "d") == 0);
    assert(!tui_dispatch_key(state, 27));
    free(state);
}

static void test_shortcuts_drafts_and_node_action(void) {
    tui_state_t *state = state_create();
    assert(tui_dispatch_key(state, '\n'));
    assert(state->field == TUI_FIELD_COMPOSE);
    assert(tui_dispatch_key(state, 'N')); /* Text while composing, not navigation. */
    assert(state->screen == TUI_SCREEN_CONVERSATIONS);
    assert(tui_dispatch_key(state, 27));
    assert(tui_dispatch_key(state, 'N'));
    assert(state->screen == TUI_SCREEN_NETWORK);
    seed_node(state);
    assert(tui_dispatch_key(state, '/'));
    assert(state->field == TUI_FIELD_NODE_SEARCH);
    assert(tui_dispatch_key(state, '3'));
    assert(tui_dispatch_key(state, '3'));
    assert(tui_dispatch_key(state, '\n'));
    assert(state->field == TUI_FIELD_NONE && tui_state_node_count(state) == 1u);
    /* A disappeared selection must not produce an invisible node popup. */
    memset(state->node_selection, 0xff, sizeof state->node_selection);
    assert(tui_dispatch_key(state, '\n'));
    assert(state->overlay == TUI_OVERLAY_NODE_ACTIONS);
    assert(state->node_selection[0] == 0x33u);
    assert(tui_dispatch_key(state, 'm'));
    assert(state->screen == TUI_SCREEN_CONVERSATIONS);
    assert(state->overlay == TUI_OVERLAY_NONE && state->field == TUI_FIELD_COMPOSE);
    assert(state->contacts[state->selected].peer[0] == 0x44u);
    assert(tui_editor_empty(&state->composer));
    state->field = TUI_FIELD_NONE;
    assert(tui_dispatch_key(state, 'k'));
    assert(tui_editor_empty(&state->composer));
    assert(tui_dispatch_key(state, 'k'));
    assert(strcmp(tui_editor_text(&state->composer), "N") == 0);
    state->field = TUI_FIELD_COMPOSE;
    assert(tui_dispatch_key(state, 27));
    assert(tui_dispatch_key(state, 'S'));
    assert(state->screen == TUI_SCREEN_SETTINGS);
    assert(tui_dispatch_key(state, '\n'));
    assert(state->field == TUI_FIELD_SETTING);
    assert(tui_dispatch_key(state, 27));
    assert(state->screen == TUI_SCREEN_SETTINGS && state->field == TUI_FIELD_NONE);
    assert(tui_dispatch_key(state, 27));
    assert(state->screen == TUI_SCREEN_CONVERSATIONS);
    assert(tui_dispatch_key(state, 'G'));
    assert(state->screen == TUI_SCREEN_GUIDE);
    assert(tui_dispatch_key(state, '\n'));
    assert(state->screen == TUI_SCREEN_GUIDE && state->field == TUI_FIELD_NONE);
    assert(tui_dispatch_key(state, 27));
    assert(state->screen == TUI_SCREEN_CONVERSATIONS);
    assert(tui_dispatch_key(state, 'I'));
    assert(state->screen == TUI_SCREEN_INTERFACES);
    assert(tui_dispatch_key(state, 'j'));
    assert(state->interfaces.selected_index == 0u);
    assert(tui_dispatch_key(state, 'r'));
    assert(strstr(state->status, "counters refreshed") != NULL);
    assert(tui_dispatch_key(state, '?'));
    assert(strstr(state->status, "Interfaces") != NULL);
    assert(tui_dispatch_key(state, 27));
    assert(state->screen == TUI_SCREEN_CONVERSATIONS);
    assert(tui_dispatch_key(state, 'F'));
    assert(state->screen == TUI_SCREEN_CONFIG);
    assert(tui_dispatch_key(state, '?'));
    assert(strstr(state->status, "validated read-only") != NULL);
    assert(tui_dispatch_key(state, 27));
    assert(state->screen == TUI_SCREEN_CONVERSATIONS);
    assert(tui_dispatch_key(state, 'R'));
    assert(state->screen == TUI_SCREEN_RRC);
    assert(tui_dispatch_key(state, '\n'));
    assert(state->field == TUI_FIELD_RRC);
    for (size_t i = 0u; i < TUI_RRC_HUB_ADDRESS_HEX; ++i)
        assert(tui_dispatch_key(state, 'a'));
    assert(tui_dispatch_key(state, '\n'));
    assert(state->field == TUI_FIELD_NONE);
    assert(strlen(state->rrc.hub_address) == TUI_RRC_HUB_ADDRESS_HEX);
    assert(tui_dispatch_key(state, 'j'));
    assert(state->rrc.selected == TUI_RRC_ITEM_HUB_IDENTITY);
    assert(tui_dispatch_key(state, 27));
    assert(state->screen == TUI_SCREEN_CONVERSATIONS);
    assert(!tui_dispatch_key(state, 'Q'));
    free(state);
}

static void test_browser_terminal_escape(void) {
    tui_state_t *state = state_create();
    rns_config_t config;
    rns_config_init(&config);
    assert(rns_runtime_create(&state->runtime, &config, NULL) == RNS_OK);
    assert(rns_browser_create(&state->browser, state->runtime, NULL) == RNS_OK);
    state->screen = TUI_SCREEN_BROWSER;
    assert(tui_dispatch_key(state, 27));
    assert(state->screen == TUI_SCREEN_CONVERSATIONS);
    rns_browser_cancel(state->browser);
    state->screen = TUI_SCREEN_BROWSER;
    assert(tui_dispatch_key(state, 27));
    assert(state->screen == TUI_SCREEN_CONVERSATIONS);
    assert(rns_browser_state(state->browser) == RNS_BROWSER_CANCELLED);
    rns_browser_destroy(state->browser);
    rns_runtime_destroy(state->runtime);
    free(state);
}

static void test_network_popup_reasons(void) {
    tui_state_t *state = state_create();
    state->screen = TUI_SCREEN_NETWORK;
    seed_node(state);
    rns_node_record *node = &state->nodes.records[0];
    const char *reason = tui_render_node_propagation_reason(node);
    assert(reason != NULL && strcmp(reason, "not a propagation announce") == 0);
    assert(strlen(reason) + 3u <= 30u);

    node->propagation = true;
    node->lxmf_pn_app_data_valid = true;
    node->reachable = false;
    assert(strcmp(tui_render_node_propagation_reason(node),
                  "stale or unreachable") == 0);
    node->reachable = true;
    node->lxmf_pn_enabled = false;
    assert(strcmp(tui_render_node_propagation_reason(node),
                  "propagation is disabled") == 0);
    node->lxmf_pn_enabled = true;
    node->lxmf_pn_stamp_cost = 0u;
    assert(strcmp(tui_render_node_propagation_reason(node),
                  "invalid propagation cost") == 0);
    node->lxmf_pn_stamp_cost = 8u;
    assert(tui_render_node_propagation_reason(node) == NULL);

    node->lxmf_pn_enabled = false;
    state->overlay = TUI_OVERLAY_NODE_ACTIONS;
    FILE *dump = tmpfile();
    assert(dump != NULL && tui_render_dump(state, dump) == 0);
    assert(fseek(dump, 0L, SEEK_SET) == 0);
    char output[512];
    size_t length = fread(output, 1u, sizeof output - 1u, dump);
    output[length] = '\0';
    assert(strstr(output,
                  "Propagation action: unavailable: propagation is disabled") != NULL);
    assert(strstr(output, "&#") == NULL);
    assert(fclose(dump) == 0);
    free(state);
}

static void test_rrc_headless_dump(void) {
    tui_state_t *state = state_create();
    state->screen = TUI_SCREEN_RRC;
    assert(tui_rrc_edit_apply(&state->rrc, TUI_RRC_ITEM_HUB_ADDRESS,
        "00112233445566778899aabbccddeeff", 32u));
    assert(tui_rrc_edit_apply(&state->rrc, TUI_RRC_ITEM_NICK, "Rei", 3u));
    assert(tui_rrc_edit_apply(&state->rrc, TUI_RRC_ITEM_ROOM, "lobby", 5u));
    static const uint8_t body[] = {0x65u, 'h', 'e', 'l', 'l', 'o'};
    rns_rrc_envelope_t envelope = {
        .version = RNS_RRC_VERSION,
        .type = RNS_RRC_MESSAGE,
        .timestamp_ms = 10u,
        .room = {(const uint8_t *)"lobby", 5u},
        .body_cbor = {body, sizeof body},
        .nick = {(const uint8_t *)"Rei", 3u}};
    tui_rrc_apply_envelope(&state->rrc, &envelope);
    FILE *dump = tmpfile();
    assert(dump != NULL);
    assert(tui_render_dump(state, dump) == 0);
    assert(fseek(dump, 0L, SEEK_SET) == 0);
    char output[2048];
    size_t length = fread(output, 1u, sizeof output - 1u, dump);
    output[length] = '\0';
    assert(strstr(output, "Screen: RRC") != NULL);
    assert(strstr(output, "State: disconnected") != NULL);
    assert(strstr(output, "Auto reconnect: on") != NULL);
    assert(strstr(output, "#lobby <Rei> hello") != NULL);
    assert(strstr(output, "Resource envelopes") != NULL);
    assert(fclose(dump) == 0);
    assert(tui_render_rrc_first_item(TUI_RRC_ITEM_HUB_ADDRESS, 3u) == 0u);
    assert(tui_render_rrc_first_item(TUI_RRC_ITEM_SEND, 3u) ==
           (size_t)TUI_RRC_ITEM_SEND - 2u);
    assert(tui_render_rrc_first_item(TUI_RRC_ITEM_SEND, 0u) ==
           (size_t)TUI_RRC_ITEM_SEND);
    free(state);
}

int main(void) {
    assert(!tui_dispatch_key(NULL, '\n'));
    test_screen_scoping();
    test_empty_network_and_filtered_contact();
    test_delivery_shortcut_is_screen_scoped();
    test_propagation_sync_actions_are_screen_scoped();
    test_event_log_screen();
    test_hidden_editors();
    test_modal_isolation();
    test_shortcuts_drafts_and_node_action();
    test_browser_terminal_escape();
    test_network_popup_reasons();
    test_rrc_headless_dump();
    return 0;
}
