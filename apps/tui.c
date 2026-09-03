#include "tui.h"

#include "tui_render.h"
#include "tui_state.h"
#include "tui_text.h"

#include <curses.h>
#include <locale.h>
#include <string.h>

#define TUI_POLL_INTERVAL_MS 50
#define TUI_SCROLL_STEP 5

/* A selected index can survive switching trust tabs or filtering to an empty
 * list. It must not enable actions for a conversation that is not displayed. */
static bool conversation_selected(tui_state_t *state) {
    if (state->screen != TUI_SCREEN_CONVERSATIONS ||
        tui_state_selected_contact(state) == NULL) return false;
    tui_state_refresh(state);
    for (size_t i = 0u; i < state->visible_count; ++i)
        if (state->visible[i] == state->selected) return true;
    return false;
}

static bool field_visible(tui_state_t *state) {
    switch (state->field) {
        case TUI_FIELD_COMPOSE: return conversation_selected(state);
        case TUI_FIELD_SEARCH: case TUI_FIELD_ADDRESS:
            return state->screen == TUI_SCREEN_CONVERSATIONS;
        case TUI_FIELD_NODE_SEARCH: return state->screen == TUI_SCREEN_NETWORK;
        case TUI_FIELD_SETTING: return state->screen == TUI_SCREEN_SETTINGS;
        case TUI_FIELD_NONE: return true;
    }
    return false;
}

static bool overlay_visible(tui_state_t *state) {
    switch (state->overlay) {
        case TUI_OVERLAY_HELP:
            return state->screen == TUI_SCREEN_CONVERSATIONS;
        case TUI_OVERLAY_PEER: return conversation_selected(state);
        case TUI_OVERLAY_NODE_ACTIONS: {
            rns_node_record node;
            return state->screen == TUI_SCREEN_NETWORK &&
                   tui_state_selected_node(state, &node);
        }
        case TUI_OVERLAY_NONE: return true;
    }
    return false;
}

static tui_editor_t *active_editor(tui_state_t *state) {
    switch (state->field) {
        case TUI_FIELD_COMPOSE: return &state->composer;
        case TUI_FIELD_SEARCH: return &state->search;
        case TUI_FIELD_NODE_SEARCH: return &state->node_search;
        case TUI_FIELD_ADDRESS: return &state->address;
        case TUI_FIELD_SETTING: return &state->setting;
        case TUI_FIELD_NONE: break;
    }
    return NULL;
}

static bool editor_command(int key, tui_edit_command_t *command) {
    switch (key) {
        case KEY_BACKSPACE: case 127: case 8: *command = TUI_EDIT_BACKSPACE; return true;
        case KEY_DC: *command = TUI_EDIT_DELETE; return true;
        case KEY_LEFT: case 2: *command = TUI_EDIT_LEFT; return true;
        case KEY_RIGHT: case 6: *command = TUI_EDIT_RIGHT; return true;
        case KEY_HOME: case 1: *command = TUI_EDIT_HOME; return true;
        case KEY_END: case 5: *command = TUI_EDIT_END; return true;
        case 21: *command = TUI_EDIT_KILL_TO_START; return true;
        case 11: *command = TUI_EDIT_KILL_TO_END; return true;
        case 23: *command = TUI_EDIT_KILL_WORD; return true;
        default: return false;
    }
}

static void unavailable_screen(tui_state_t *state, const char *name) {
    tui_state_set_status(state, "%s screen is not implemented; remaining in Conversations",
                         name);
    state->screen = TUI_SCREEN_CONVERSATIONS;
}

static void submit_address(tui_state_t *state) {
    uint8_t destination[LXMF_DESTINATION_LENGTH];
    if (!tui_hex_parse(tui_editor_text(&state->address), destination,
                       sizeof destination)) {
        tui_state_set_status(state, "Address must be exactly 32 hexadecimal characters");
        return;
    }
    tui_editor_clear(&state->address);
    (void)tui_state_open_conversation(state, destination);
}

static void submit_message(tui_state_t *state) {
    lxmf_status_t status = tui_state_queue_message(state);
    if (status != LXMF_OK) {
        tui_state_set_status(state, "Could not queue message (%d)", status);
        return;
    }
    /* A caller-polled send is normally pending here. Preserve the router's
     * queue reason or delivery event instead of treating it as a failure. */
    if (!state->router_ready)
        tui_state_set_status(state, "Queued locally; network delivery is pending");
    tui_editor_clear(&state->composer);
    tui_state_save_draft(state);
    state->field = TUI_FIELD_NONE;
}

/* Returns false when the field was submitted or cancelled. */
static void handle_field_key(tui_state_t *state, int key) {
    tui_editor_t *editor = active_editor(state);
    tui_edit_command_t command;
    if (editor == NULL) return;
    if (key == 27) {
        if (state->field == TUI_FIELD_SETTING) tui_state_setting_cancel(state);
        else state->field = TUI_FIELD_NONE;
        return;
    }
    if (key == '\n' || key == KEY_ENTER) {
        if (state->field == TUI_FIELD_SETTING) (void)tui_state_setting_apply(state);
        else if (state->field == TUI_FIELD_ADDRESS) submit_address(state);
        else if (state->field == TUI_FIELD_SEARCH) {
            state->field = TUI_FIELD_NONE;
            tui_state_refresh(state);
            if (state->visible_count > 0u) state->selected = state->visible[0];
            state->filter_dirty = true;
        } else if (state->field == TUI_FIELD_NODE_SEARCH) {
            state->field = TUI_FIELD_NONE;
            tui_state_node_move(state, 0);
            tui_state_set_status(state, tui_state_node_count(state) == 0u
                ? "No nodes match the search" : "Network search applied");
        } else submit_message(state);
        return;
    }
    if (editor_command(key, &command)) (void)tui_editor_apply(editor, command);
    else if (key >= 0 && key <= 0xff)
        (void)tui_editor_insert_byte(editor, (unsigned char)key);
    if (state->field == TUI_FIELD_SEARCH) state->filter_dirty = true;
    else if (state->field == TUI_FIELD_COMPOSE) tui_state_save_draft(state);
}

static void handle_node_actions_key(tui_state_t *state, int key) {
    rns_node_record node;
    if (key == 27) {
        state->overlay = TUI_OVERLAY_NONE;
        return;
    }
    if (!tui_state_selected_node(state, &node)) return;
    if (key == 'b' || key == 'B') {
        tui_state_browse_node(state, &node);
    } else if (key == 'm' || key == 'M') {
        if (node.has_message_destination)
            (void)tui_state_open_conversation(state, node.message_destination);
        else tui_state_set_status(state, "This announce has no associated LXMF inbox");
    } else if (key == 'p' || key == 'P') {
        if (tui_state_use_propagation_node(state, &node))
            state->overlay = TUI_OVERLAY_NONE;
        else
            tui_state_set_status(state,
                "Node cannot be selected without a fresh enabled propagation announce and cost");
    } else if (key == 'r' || key == 'R') {
        tui_state_request_path(state);
        state->overlay = TUI_OVERLAY_NONE;
    }
}

static void select_previous(tui_state_t *state) {
    if (state->screen == TUI_SCREEN_BROWSER) {
        if (state->link_selected > 0u) --state->link_selected;
    } else if (state->screen == TUI_SCREEN_SETTINGS) {
        tui_state_setting_move(state, -1);
    } else if (state->screen == TUI_SCREEN_NETWORK) {
        tui_state_node_move(state, -1);
    } else if (state->screen == TUI_SCREEN_INTERFACES) {
        tui_state_interface_move(state, -1);
    } else if (state->screen == TUI_SCREEN_CONVERSATIONS)
        tui_state_select_offset(state, -1);
}

static void select_next(tui_state_t *state) {
    if (state->screen == TUI_SCREEN_BROWSER) {
        size_t links = tui_state_link_count(state);
        if (links > 0u) state->link_selected = (state->link_selected + 1u) % links;
    } else if (state->screen == TUI_SCREEN_SETTINGS) {
        tui_state_setting_move(state, 1);
    } else if (state->screen == TUI_SCREEN_NETWORK) {
        tui_state_node_move(state, 1);
    } else if (state->screen == TUI_SCREEN_INTERFACES) {
        tui_state_interface_move(state, 1);
    } else if (state->screen == TUI_SCREEN_CONVERSATIONS)
        tui_state_select_offset(state, 1);
}

static void activate(tui_state_t *state) {
    if (state->screen == TUI_SCREEN_BROWSER) {
        tui_state_browse_selected(state);
    } else if (state->screen == TUI_SCREEN_SETTINGS) {
        tui_state_setting_activate(state);
    } else if (state->screen == TUI_SCREEN_NETWORK) {
        if (tui_state_node_count(state) > 0u) {
            rns_node_record selected;
            if (!tui_state_selected_node(state, &selected)) tui_state_node_move(state, 0);
            state->overlay = TUI_OVERLAY_NODE_ACTIONS;
        } else tui_state_set_status(state, "No known nodes: wait for a verified announce");
    } else if (state->screen == TUI_SCREEN_CONVERSATIONS) {
        if (conversation_selected(state)) state->field = TUI_FIELD_COMPOSE;
        else tui_state_set_status(state, "No visible conversation: provide a destination address");
    }
}

static void reload(tui_state_t *state) {
    if (state->screen == TUI_SCREEN_NETWORK) tui_state_request_path(state);
    else if (state->screen == TUI_SCREEN_BROWSER)
        (void)tui_state_browse(state, state->url, false);
    else if (state->screen == TUI_SCREEN_SETTINGS)
        tui_state_set_status(state, "Select Announce Now to send an LXMF announce");
    else unavailable_screen(state, "RRC");
}

/* Returns false when the client should exit. */
static bool handle_command_key(tui_state_t *state, int key) {
    switch (key) {
        case 'q': case 'Q':
            return false;
        case 27:
            if (state->screen == TUI_SCREEN_BROWSER && state->browser != NULL) {
                rns_browser_state_t current = rns_browser_state(state->browser);
                if ((current == RNS_BROWSER_PATH_DISCOVERY ||
                     current == RNS_BROWSER_LINK_ESTABLISHMENT ||
                     current == RNS_BROWSER_REQUEST_TRANSMISSION) &&
                    tui_state_browse_cancel(state)) return true;
            }
            if (state->screen == TUI_SCREEN_CONVERSATIONS) return false;
            state->screen = TUI_SCREEN_CONVERSATIONS;
            break;
        case '?':
            if (state->screen == TUI_SCREEN_CONVERSATIONS)
                state->overlay = TUI_OVERLAY_HELP;
            else if (state->screen == TUI_SCREEN_NETWORK)
                tui_state_set_status(state, "Network: j/k select, Enter node actions, R request path, Esc Chats");
            else if (state->screen == TUI_SCREEN_BROWSER)
                tui_state_set_status(state, "Browser: j/k links, Enter open, PgUp/PgDn scroll, R reload, Esc cancel/back");
            else if (state->screen == TUI_SCREEN_SETTINGS)
                tui_state_set_status(state, "Settings: j/k select, Enter edit/apply, Esc cancel/back");
            else if (state->screen == TUI_SCREEN_INTERFACES)
                tui_state_set_status(state, "Interfaces: j/k inspect live counters, Esc Chats");
            else if (state->screen == TUI_SCREEN_CONFIG)
                tui_state_set_status(state, "Config: validated read-only runtime configuration, Esc Chats");
            else tui_state_set_status(state, "Screen unavailable: C or Esc returns to Conversations");
            break;
        case '1':
            if (state->screen == TUI_SCREEN_CONVERSATIONS)
                tui_state_set_tab(state, TUI_TRUST_TRUSTED);
            break;
        case '2':
            if (state->screen == TUI_SCREEN_CONVERSATIONS)
                tui_state_set_tab(state, TUI_TRUST_UNKNOWN);
            break;
        case '3':
            if (state->screen == TUI_SCREEN_CONVERSATIONS)
                tui_state_set_tab(state, TUI_TRUST_UNTRUSTED);
            break;
        case KEY_UP: case 'k': select_previous(state); break;
        case KEY_DOWN: case 'j': case '\t': select_next(state); break;
        case KEY_PPAGE:
            if (state->screen == TUI_SCREEN_BROWSER || conversation_selected(state))
                tui_state_scroll_by(state, TUI_SCROLL_STEP);
            break;
        case KEY_NPAGE:
            if (state->screen == TUI_SCREEN_BROWSER || conversation_selected(state))
                tui_state_scroll_by(state, -TUI_SCROLL_STEP);
            break;
        case '/':
            if (state->screen == TUI_SCREEN_CONVERSATIONS) {
                state->field = TUI_FIELD_SEARCH;
                (void)tui_editor_apply(&state->search, TUI_EDIT_END);
            } else if (state->screen == TUI_SCREEN_NETWORK) {
                state->field = TUI_FIELD_NODE_SEARCH;
                (void)tui_editor_apply(&state->node_search, TUI_EDIT_END);
            }
            break;
        case 'a': case 'A':
            if (state->screen == TUI_SCREEN_CONVERSATIONS) {
                state->field = TUI_FIELD_ADDRESS;
                tui_editor_clear(&state->address);
            }
            break;
        case 'd': case 'D':
            if (state->screen == TUI_SCREEN_CONVERSATIONS)
                tui_state_toggle_delivery_method(state);
            break;
        case 'i':
            if (conversation_selected(state))
                state->overlay = TUI_OVERLAY_PEER;
            break;
        case 'p':
            if (conversation_selected(state)) tui_state_toggle_pin(state);
            break;
        case 'x':
            if (conversation_selected(state)) tui_state_toggle_block(state);
            break;
        case 't':
            if (conversation_selected(state))
                tui_state_set_trust(state, TUI_TRUST_TRUSTED);
            break;
        case 'u':
            if (conversation_selected(state))
                tui_state_set_trust(state, TUI_TRUST_UNTRUSTED);
            break;
        case 'n':
            if (conversation_selected(state)) tui_state_toggle_note(state);
            break;
        case 'y':
            tui_state_set_status(state,
                                 "Clipboard unavailable; use history or --dump-ui to copy text");
            break;
        case 'c': case 'C': state->screen = TUI_SCREEN_CONVERSATIONS; break;
        case 'N': state->screen = TUI_SCREEN_NETWORK; break;
        case 'B': state->screen = TUI_SCREEN_BROWSER; break;
        case 'o': case 'O': unavailable_screen(state, "Node"); break;
        case 's': case 'S': state->screen = TUI_SCREEN_SETTINGS; break;
        case 'g': case 'G': state->screen = TUI_SCREEN_GUIDE; break;
        case 'I': state->screen = TUI_SCREEN_INTERFACES; break;
        case 'F': state->screen = TUI_SCREEN_CONFIG; break;
        case 'l': case 'L': unavailable_screen(state, "Logs"); break;
        case 'r': case 'R': reload(state); break;
        case '\n': case KEY_ENTER: activate(state); break;
        case KEY_BACKSPACE: case 127: case 8:
            if (state->screen == TUI_SCREEN_BROWSER) tui_state_browse_back(state);
            break;
        default: break;
    }
    return true;
}

bool tui_dispatch_key(tui_state_t *state, int key) {
    if (state == NULL) return false;
    if (key == ERR || key == KEY_RESIZE) return true;
    /* Asynchronous screen/state changes can leave stale editors or modals.
     * Discard their focus, never their text, and consume this key so a hidden
     * Enter cannot become a send or activate an unrelated screen action. */
    if (!overlay_visible(state)) {
        state->overlay = TUI_OVERLAY_NONE;
        if (!field_visible(state)) state->field = TUI_FIELD_NONE;
        return true;
    }
    if (state->overlay == TUI_OVERLAY_NODE_ACTIONS) {
        handle_node_actions_key(state, key);
        return true;
    }
    if (state->overlay != TUI_OVERLAY_NONE) {
        if (key == 27 || key == '?' || key == 'i') state->overlay = TUI_OVERLAY_NONE;
        return true;
    }
    if (!field_visible(state)) {
        state->field = TUI_FIELD_NONE;
        return true;
    }
    if (state->field != TUI_FIELD_NONE) {
        handle_field_key(state, key);
        return true;
    }
    return handle_command_key(state, key);
}

static int run_loop(tui_state_t *state) {
    int result = 0;
    bool running = true;
    if (setlocale(LC_ALL, "") == NULL) return -1;
    if (initscr() == NULL) return -1;
    (void)cbreak();
    (void)noecho();
    /* Micron pages carry colour; a terminal without it still renders text. */
    if (has_colors()) {
        (void)start_color();
        (void)use_default_colors();
    }
    (void)keypad(stdscr, TRUE);
    (void)timeout(TUI_POLL_INTERVAL_MS);
    while (running) {
        tui_state_poll(state);
        tui_state_refresh(state);
        tui_render_draw(state);
        (void)curs_set(state->field != TUI_FIELD_NONE ? 1 : 0);
        int key = getch();
        if (key == ERR || key == KEY_RESIZE) continue;
        running = tui_dispatch_key(state, key);
    }
    if (endwin() == ERR) result = -1;
    return result;
}

static int run(const char *identity_path, const char *store_path,
               const char *destination_hex, const char *config_path) {
    tui_state_t state;
    if (tui_state_open(&state, identity_path, store_path, destination_hex,
                       config_path) != 0) return -1;
    int result = run_loop(&state);
    tui_state_close(&state);
    return result;
}

int nomad_tui_run(const char *identity_path, const char *store_path) {
    return run(identity_path, store_path, NULL, NULL);
}

int nomad_tui_run_destination(const char *identity_path, const char *store_path,
                              const char *destination_hex) {
    return run(identity_path, store_path, destination_hex, NULL);
}

int nomad_tui_run_config(const char *config_path, const char *identity_path,
                         const char *store_path, const char *destination_hex) {
    if (config_path == NULL) return -1;
    return run(identity_path, store_path, destination_hex, config_path);
}

int nomad_tui_dump(const char *identity_path, const char *store_path,
                   const char *destination_hex, FILE *output) {
    tui_state_t state;
    if (output == NULL) return -1;
    if (tui_state_open(&state, identity_path, store_path, destination_hex, NULL) != 0)
        return -1;
    int result = tui_render_dump(&state, output);
    tui_state_close(&state);
    return result;
}
