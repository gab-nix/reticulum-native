#include "tui.h"
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
        if (screen != TUI_SCREEN_CONVERSATIONS && screen != TUI_SCREEN_SETTINGS) {
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

static void test_hidden_editors(void) {
    for (int screen = 0; screen < TUI_SCREEN_COUNT; ++screen) {
        for (int field = TUI_FIELD_COMPOSE; field <= TUI_FIELD_SETTING; ++field) {
            bool visible = field == TUI_FIELD_SETTING
                             ? screen == TUI_SCREEN_SETTINGS
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
    assert(state->interface_selected == 0u);
    assert(tui_dispatch_key(state, '?'));
    assert(strstr(state->status, "Interfaces") != NULL);
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

int main(void) {
    assert(!tui_dispatch_key(NULL, '\n'));
    test_screen_scoping();
    test_empty_network_and_filtered_contact();
    test_delivery_shortcut_is_screen_scoped();
    test_hidden_editors();
    test_modal_isolation();
    test_shortcuts_drafts_and_node_action();
    test_browser_terminal_escape();
    return 0;
}
