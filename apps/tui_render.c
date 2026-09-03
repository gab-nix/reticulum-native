#include "tui_render.h"

#include "tui_text.h"

#include <curses.h>
#include <limits.h>
#include <string.h>

#define TUI_MIN_ROWS 10
#define TUI_MIN_COLUMNS 38
#define TUI_NARROW_COLUMNS 72
#define TUI_SIDEBAR_WIDTH 24
#define TUI_PROMPT_WIDTH 9

/*
 * Row assignment for the conversation screen. Every screen shares the header,
 * navigation and footer rows so the chrome never shifts between screens.
 */
typedef struct {
    int rows;
    int columns;
    bool narrow;
    int sidebar;
    int pane_x;
    int pane_width;
    int content_top;
    int content_rows;
    int divider_row;
    int input_row;
    int hint_row;
    int legend_row;
} tui_layout_t;

static tui_layout_t layout_of(int rows, int columns) {
    tui_layout_t layout;
    layout.rows = rows;
    layout.columns = columns;
    layout.narrow = columns < TUI_NARROW_COLUMNS;
    layout.sidebar = layout.narrow ? 0 : TUI_SIDEBAR_WIDTH;
    layout.pane_x = layout.sidebar + (layout.narrow ? 0 : 1);
    layout.pane_width = columns - layout.pane_x;
    layout.content_top = 3;
    layout.content_rows = rows - 7;
    layout.divider_row = rows - 4;
    layout.input_row = rows - 3;
    layout.hint_row = rows - 2;
    layout.legend_row = rows - 1;
    return layout;
}

static void clipped(WINDOW *window, int y, int x, int width, const char *text) {
    if (width > 0) (void)mvwaddnstr(window, y, x, text, width);
}

/*
 * Overlays are drawn straight into stdscr and painted by the single refresh at
 * the end of the frame. A separate window would be erased and repainted by
 * every stdscr refresh, which flickers at the redraw interval.
 */
static void centered_box(const char *title, const char *const *lines, size_t count) {
    int rows, columns;
    getmaxyx(stdscr, rows, columns);
    int width = columns > 70 ? 68 : columns - 4;
    int height = (int)count + 4;
    if (width < 20 || rows < height + 2) return;
    int top = (rows - height) / 2;
    int left = (columns - width) / 2;
    (void)attron(A_REVERSE);
    for (int y = 0; y < height; ++y) {
        (void)move(top + y, left);
        for (int x = 0; x < width; ++x) (void)addch(' ');
    }
    (void)mvhline(top, left, ACS_HLINE, width);
    (void)mvhline(top + height - 1, left, ACS_HLINE, width);
    (void)mvvline(top, left, ACS_VLINE, height);
    (void)mvvline(top, left + width - 1, ACS_VLINE, height);
    (void)mvaddch(top, left, ACS_ULCORNER);
    (void)mvaddch(top, left + width - 1, ACS_URCORNER);
    (void)mvaddch(top + height - 1, left, ACS_LLCORNER);
    (void)mvaddch(top + height - 1, left + width - 1, ACS_LRCORNER);
    (void)attron(A_BOLD);
    clipped(stdscr, top + 1, left + 2, width - 4, title);
    (void)attroff(A_BOLD);
    for (size_t i = 0u; i < count; ++i)
        clipped(stdscr, top + 2 + (int)i, left + 2, width - 4, lines[i]);
    (void)attroff(A_REVERSE);
}

static const char *delivery_marker(lxmf_delivery_status_t status) {
    switch (status) {
        case LXMF_DELIVERY_QUEUED: return "[.]";
        case LXMF_DELIVERY_SENDING: return "[>]";
        case LXMF_DELIVERY_SENT: return "[+]";
        case LXMF_DELIVERY_DELIVERED: return "[x]";
        case LXMF_DELIVERY_FAILED: return "[!]";
    }
    return "[?]";
}

static const char *trust_name(tui_trust_t trust) {
    switch (trust) {
        case TUI_TRUST_TRUSTED: return "TRUSTED";
        case TUI_TRUST_UNKNOWN: return "UNKNOWN";
        case TUI_TRUST_UNTRUSTED: return "UNTRUSTED";
    }
    return "UNKNOWN";
}

static void draw_too_small(const tui_layout_t *layout) {
    (void)attron(A_BOLD);
    clipped(stdscr, 0, 0, layout->columns, "Nomad Chat");
    (void)attroff(A_BOLD);
    clipped(stdscr, 2, 0, layout->columns, "Terminal too small (need 38x10)");
    clipped(stdscr, layout->legend_row, 0, layout->columns, "q quit");
}

static void draw_chrome(const tui_state_t *state, const tui_layout_t *layout) {
    char address[TUI_ADDRESS_DIGITS + 1u];
    char header[TUI_STATUS_MAX];
    tui_hex_format(state->local, LXMF_DESTINATION_LENGTH, address);
    /*
     * A configured runtime whose interfaces are all down is not online, and
     * saying so is the difference between a visible fault and a silent one.
     */
    const char *link = state->runtime == NULL ? "OFFLINE"
                       : tui_state_link_ready(state) ? "ONLINE" : "NO LINK";
    (void)snprintf(header, sizeof header, " Nomad Chat  %.32s  %s ", address, link);
    (void)attron(A_REVERSE | A_BOLD);
    clipped(stdscr, 0, 0, layout->columns, header);
    for (int x = (int)strlen(header); x < layout->columns; ++x) (void)addch(' ');
    (void)attroff(A_REVERSE | A_BOLD);
    clipped(stdscr, 1, 0, layout->columns,
            "[C]hats [N]etwork [B]rowser [S]ettings [I]faces [F]Config [G]uide [L]ogs [R]RC");
}

static const char *interface_state_name(rns_runtime_interface_state_t state) {
    switch (state) {
        case RNS_RUNTIME_INTERFACE_DISABLED: return "disabled";
        case RNS_RUNTIME_INTERFACE_STARTING: return "starting";
        case RNS_RUNTIME_INTERFACE_UP: return "up";
        case RNS_RUNTIME_INTERFACE_DOWN: return "down";
        case RNS_RUNTIME_INTERFACE_UNSUPPORTED: return "unsupported";
    }
    return "invalid";
}

static void draw_interfaces(const tui_state_t *state,
                            const tui_layout_t *layout) {
    size_t count = tui_state_interface_count(state);
    size_t selected = state->interface_selected < count
                          ? state->interface_selected : 0u;
    (void)attron(A_BOLD);
    clipped(stdscr, 3, 1, layout->columns - 2, "Interfaces");
    (void)attroff(A_BOLD);
    if (count == 0u) {
        clipped(stdscr, 5, 2, layout->columns - 4,
                state->runtime == NULL
                    ? "Offline: start with --config to load Reticulum interfaces."
                    : "The loaded configuration defines no interfaces.");
    } else {
        int rows = layout->hint_row - 5;
        size_t first = selected >= (size_t)rows && rows > 0
                           ? selected - (size_t)rows + 1u : 0u;
        for (size_t i = first; i < count && (int)(i - first) < rows; ++i) {
            rns_runtime_interface_info_t info;
            if (!tui_state_interface_info(state, i, &info)) continue;
            char line[256];
            (void)snprintf(line, sizeof line,
                           "%s  %s  %s  rx:%llu/%lluB tx:%llu/%lluB drop:%llu",
                           info.name, rns_config_interface_type_name(info.type),
                           interface_state_name(info.state),
                           (unsigned long long)info.packets_received,
                           (unsigned long long)info.bytes_received,
                           (unsigned long long)info.packets_sent,
                           (unsigned long long)info.bytes_sent,
                           (unsigned long long)info.packets_dropped);
            if (i == selected) (void)attron(A_REVERSE);
            clipped(stdscr, 5 + (int)(i - first), 2,
                    layout->columns - 4, line);
            if (i == selected) (void)attroff(A_REVERSE);
        }
        rns_runtime_interface_info_t info;
        if (tui_state_interface_info(state, selected, &info) &&
            info.last_error != RNS_OK) {
            char error[TUI_STATUS_MAX];
            (void)snprintf(error, sizeof error, "Selected interface error: %s",
                           rns_status_string(info.last_error));
            clipped(stdscr, layout->hint_row - 1, 2,
                    layout->columns - 4, error);
        }
    }
    clipped(stdscr, layout->hint_row, 0, layout->columns,
            "j/k inspect  C or Esc conversations  S settings  q quit");
}

static void config_interface_line(const rns_config_interface_t *interface,
                                  char *line, size_t capacity) {
    const char *state = interface->enabled ? "enabled" : "disabled";
    switch (interface->type) {
        case RNS_CONFIG_TCP_CLIENT:
            (void)snprintf(line, capacity, "%s  TCP client  %s  %s:%u",
                           interface->name, state, interface->target_host,
                           (unsigned)interface->target_port);
            break;
        case RNS_CONFIG_TCP_SERVER:
            (void)snprintf(line, capacity, "%s  TCP server  %s  %s:%u",
                           interface->name, state, interface->listen_ip,
                           (unsigned)interface->listen_port);
            break;
        case RNS_CONFIG_UDP:
            (void)snprintf(line, capacity,
                           "%s  UDP  %s  listen %s:%u  forward %s:%u",
                           interface->name, state, interface->listen_ip,
                           (unsigned)interface->listen_port,
                           interface->forward_ip,
                           (unsigned)interface->forward_port);
            break;
        case RNS_CONFIG_AUTO:
        case RNS_CONFIG_KISS:
        case RNS_CONFIG_RNODE:
            (void)snprintf(line, capacity, "%s  %s  %s",
                           interface->name,
                           rns_config_interface_type_name(interface->type),
                           state);
            break;
    }
}

static void draw_config(const tui_state_t *state,
                        const tui_layout_t *layout) {
    (void)attron(A_BOLD);
    clipped(stdscr, 3, 1, layout->columns - 2, "Reticulum configuration");
    (void)attroff(A_BOLD);
    if (!state->config_attempted) {
        clipped(stdscr, 5, 2, layout->columns - 4,
                "No configuration loaded. Start nomad-chat with --config PATH.");
    } else if (!state->config_valid) {
        char line[TUI_STATUS_MAX];
        (void)snprintf(line, sizeof line, "Invalid: %s", state->config_path);
        clipped(stdscr, 5, 2, layout->columns - 4, line);
        if (state->config_diagnostic.message[0] != '\0') {
            (void)snprintf(line, sizeof line, "Line %zu: %s",
                           state->config_diagnostic.line,
                           state->config_diagnostic.message);
            clipped(stdscr, 6, 2, layout->columns - 4, line);
        }
    } else {
        char line[384];
        (void)snprintf(line, sizeof line, "File: %s", state->config_path);
        clipped(stdscr, 5, 2, layout->columns - 4, line);
        (void)snprintf(line, sizeof line,
                       "Transport: %s   Shared instance: %s   Panic on error: %s",
                       state->parsed_config.enable_transport ? "yes" : "no",
                       state->parsed_config.share_instance ? "yes" : "no",
                       state->parsed_config.panic_on_interface_error ? "yes" : "no");
        clipped(stdscr, 6, 2, layout->columns - 4, line);
        int available = layout->hint_row - 8;
        for (size_t i = 0u; i < state->parsed_config.interface_count &&
                            (int)i < available; ++i) {
            config_interface_line(&state->parsed_config.interfaces[i], line,
                                  sizeof line);
            clipped(stdscr, 8 + (int)i, 2, layout->columns - 4, line);
        }
    }
    clipped(stdscr, layout->hint_row, 0, layout->columns,
            "Read-only validated view  C or Esc conversations  S settings  q quit");
}

static void draw_sidebar(const tui_state_t *state, const tui_layout_t *layout) {
    char tabline[48];
    (void)attron(A_BOLD);
    (void)snprintf(tabline, sizeof tabline, "%s (%zu)", trust_name(state->tab),
                   state->visible_count);
    clipped(stdscr, 2, 1, layout->sidebar - 2, tabline);
    (void)attroff(A_BOLD);
    for (size_t position = 0u;
         position < state->visible_count && (int)position < layout->content_rows;
         ++position) {
        size_t index = state->visible[position];
        const tui_contact_t *contact = &state->contacts[index];
        char peer[TUI_ADDRESS_DIGITS + 1u];
        char line[64];
        bool selected = index == state->selected;
        tui_hex_format(contact->peer, LXMF_DESTINATION_LENGTH, peer);
        (void)snprintf(line, sizeof line, "%c%-11.11s %c%2zu ",
                       contact->pinned ? '*' : ' ', peer,
                       contact->unread ? '!' : ' ',
                       contact->unread ? contact->unread : contact->messages);
        if (selected) (void)attron(A_REVERSE);
        clipped(stdscr, layout->content_top + (int)position, 0, layout->sidebar - 1, line);
        if (selected) (void)attroff(A_REVERSE);
    }
    (void)mvvline(1, layout->sidebar - 1, ACS_VLINE, layout->rows - 4);
}

static void draw_thread(const tui_state_t *state, const tui_layout_t *layout) {
    char peer_line[80] = "No conversation selected";
    const tui_contact_t *contact = tui_state_selected_contact(state);
    if (contact != NULL) {
        char peer[TUI_ADDRESS_DIGITS + 1u];
        tui_hex_format(contact->peer, LXMF_DESTINATION_LENGTH, peer);
        (void)snprintf(peer_line, sizeof peer_line, "%s%s",
                       layout->narrow ? "Chat: " : "Conversation: ", peer);
    }
    (void)attron(A_BOLD);
    clipped(stdscr, 2, layout->pane_x, layout->pane_width, peer_line);
    (void)attroff(A_BOLD);

    size_t total = tui_state_thread_count(state);
    size_t capacity = layout->content_rows > 0 ? (size_t)layout->content_rows : 0u;
    size_t end = total > state->scroll ? total - state->scroll : 0u;
    size_t first = end > capacity ? end - capacity : 0u;
    for (size_t index = first; index < end; ++index) {
        const tui_message_t *message = tui_state_thread_message(state, index);
        char text[LXMF_STORE_MAX_CONTENT + 1u];
        char rendered[LXMF_STORE_MAX_CONTENT + 32u];
        if (message == NULL) break;
        bool outgoing = tui_state_outgoing(state, message);
        (void)tui_text_sanitize(message->value.content.data, message->value.content.len,
                                text, sizeof text);
        (void)snprintf(rendered, sizeof rendered, "%s %s %s", outgoing ? ">" : "<",
                       outgoing ? delivery_marker(message->value.status) : "   ", text);
        clipped(stdscr, layout->content_top + (int)(index - first), layout->pane_x,
                layout->pane_width, rendered);
    }
}

static void draw_input(const tui_state_t *state, const tui_layout_t *layout) {
    const tui_editor_t *editor = NULL;
    const char *prompt = "Search: ";
    switch (state->field) {
        case TUI_FIELD_COMPOSE:
            editor = &state->composer;
            prompt = state->compose_delivery_method ==
                         LXMF_DELIVERY_METHOD_PROPAGATED ? "Relay: " : "Direct: ";
            break;
        case TUI_FIELD_SEARCH: editor = &state->search; prompt = "Search: "; break;
        case TUI_FIELD_NODE_SEARCH:
            editor = &state->node_search; prompt = "Find node: "; break;
        case TUI_FIELD_ADDRESS: editor = &state->address; prompt = "Address: "; break;
        case TUI_FIELD_SETTING: break;
        case TUI_FIELD_NONE: break;
    }
    (void)mvhline(layout->divider_row, 0, ACS_HLINE, layout->columns);
    clipped(stdscr, layout->input_row, 0, layout->columns,
            editor != NULL ? prompt : state->status);
    if (editor != NULL) {
        clipped(stdscr, layout->input_row, TUI_PROMPT_WIDTH,
                layout->columns - TUI_PROMPT_WIDTH, tui_editor_text(editor));
        int cursor_x = TUI_PROMPT_WIDTH + (int)tui_editor_column(editor);
        (void)move(layout->input_row,
                   cursor_x < layout->columns ? cursor_x : layout->columns - 1);
    }
    clipped(stdscr, layout->hint_row, 0, layout->columns,
            editor != NULL ? "Enter accept  Esc cancel  Home/End  Ctrl-A/E/U/K/W"
                           : "d direct/propagated  1/2/3 trust  / search  i info  ? help");
    clipped(stdscr, layout->legend_row, 0, layout->columns,
            "[.] queued  [>] sending  [+] sent  [x] delivered  [!] failed");
}

static void draw_conversation_overlay(const tui_state_t *state) {
    if (state->overlay == TUI_OVERLAY_HELP) {
        static const char *const help[] = {
            "j/k or arrows: conversation    PgUp/PgDn: history",
            "1/2/3: trusted/unknown/untrusted    /: search",
            "Enter: compose    i: peer info    p: pin    x: block",
            "t/u: trust/untrust    n: local note    y: copy fallback",
            "d: choose direct or propagated delivery for queued messages",
            "Composer: arrows Home End Del Backspace Ctrl-A/E/U/K/W",
            "Propagated means accepted by the relay, not delivered to the recipient.",
            "Press ? or Esc to close."
        };
        centered_box("Help", help, sizeof help / sizeof help[0]);
        return;
    }
    const tui_contact_t *contact = tui_state_selected_contact(state);
    if (state->overlay != TUI_OVERLAY_PEER || contact == NULL) return;
    char peer[TUI_ADDRESS_DIGITS + 1u];
    char trust[64];
    char note[80];
    char flags[80];
    tui_hex_format(contact->peer, LXMF_DESTINATION_LENGTH, peer);
    (void)snprintf(trust, sizeof trust, "Trust: %s", trust_name(contact->trust));
    (void)snprintf(note, sizeof note, "Note: %s",
                   contact->note[0] != '\0' ? contact->note : "none");
    (void)snprintf(flags, sizeof flags, "Pinned: %s  Blocked: %s  Unread: %zu",
                   contact->pinned ? "yes" : "no", contact->blocked ? "yes" : "no",
                   contact->unread);
    const char *lines[] = {peer, "[ QR display unavailable in text-only build ]",
                           trust, note, flags,
                           "Contact preferences are persisted. Esc/i closes."};
    centered_box("Peer information", lines, sizeof lines / sizeof lines[0]);
}

static const char *tui_browser_error_text(const rns_browser_t *browser) {
    rns_status_t error = rns_browser_error(browser);
    if (error == RNS_ERROR_UNSUPPORTED)
        return "the remote response uses an unsupported protocol feature";
    if (error == RNS_ERROR_TIMEOUT)
        return "no response (the node may not serve this page)";
    return rns_status_string(error);
}

static const char *node_kind_name(const rns_node_record *node) {
    switch (node->kind) {
        case RNS_NODE_KIND_NOMAD: return "Nomad node";
        case RNS_NODE_KIND_LXMF: return "LXMF inbox";
        case RNS_NODE_KIND_OTHER: break;
    }
    return "transport";
}

/* Renders the node details and the actions that apply to this node. */
static void draw_node_popup(const tui_state_t *state) {
    rns_node_record node;
    char address[TUI_ADDRESS_DIGITS + 1u];
    char inbox[TUI_ADDRESS_DIGITS + 1u];
    char lines[11][96];
    const char *pointers[11];
    size_t count = 0u;
    if (!tui_state_selected_node(state, &node)) return;
    tui_hex_format(node.destination, LXMF_DESTINATION_LENGTH, address);
    bool pages = tui_state_node_serves_pages(&node);
    bool messageable = node.has_message_destination;

    (void)snprintf(lines[count++], sizeof lines[0], "Address:  %s", address);
    (void)snprintf(lines[count++], sizeof lines[0], "Name:     %s",
                   node.name[0] != '\0' ? node.name : "(none announced)");
    (void)snprintf(lines[count++], sizeof lines[0], "Type:     %s%s",
                   node_kind_name(&node),
                   pages ? " - serves pages" : " - serves no pages");
    (void)snprintf(lines[count++], sizeof lines[0],
                   "Route:    %u hops on interface %llu, %s%s",
                   (unsigned)node.hops, (unsigned long long)node.interface_id,
                   node.reachable ? "reachable" : "stale",
                   node.propagation ? ", propagation node" : "");
    if (messageable) {
        tui_hex_format(node.message_destination, LXMF_DESTINATION_LENGTH, inbox);
        (void)snprintf(lines[count++], sizeof lines[0], "Inbox:    %s", inbox);
    } else {
        (void)snprintf(lines[count++], sizeof lines[0],
                       "Inbox:    none announced");
    }
    (void)snprintf(lines[count++], sizeof lines[0], "%s", "");
    (void)snprintf(lines[count++], sizeof lines[0], "b  Browse pages%s",
                   pages ? "" : "   (unavailable: not a Nomad node)");
    (void)snprintf(lines[count++], sizeof lines[0], "m  Send a message%s",
                   messageable ? "" : "   (unavailable: no LXMF inbox announced)");
    bool propagation_ready = node.reachable && node.propagation &&
        node.lxmf_pn_app_data_valid && node.lxmf_pn_enabled &&
        node.lxmf_pn_stamp_cost > 0u && node.lxmf_pn_stamp_cost < UINT8_MAX;
    (void)snprintf(lines[count++], sizeof lines[0],
                   "p  Use as propagation node%s",
                   propagation_ready ? "" : "   (unavailable: no enabled announce/cost)");
    (void)snprintf(lines[count++], sizeof lines[0], "r  Refresh path");
    (void)snprintf(lines[count++], sizeof lines[0], "Esc  Close");
    for (size_t i = 0u; i < count; ++i) pointers[i] = lines[i];
    centered_box("Node details", pointers, count);
}

static void draw_network(const tui_state_t *state, const tui_layout_t *layout) {
    rns_node_record sorted[RNS_NODE_REGISTRY_MAX];
    char heading[96];
    size_t count = tui_state_node_list(state, sorted, RNS_NODE_REGISTRY_MAX);
    size_t position = tui_state_node_position(state);
    size_t rows = layout->rows > 8 ? (size_t)(layout->rows - 8) : 1u;
    /*
     * Scroll the window around the selection rather than the selection around
     * the window, so the cursor stays on the chosen node.
     */
    size_t first = position >= rows ? position - rows + 1u : 0u;
    if (first + rows > count) first = count > rows ? count - rows : 0u;

    const char *filter = tui_editor_text(&state->node_search);
    if (*filter != '\0')
        (void)snprintf(heading, sizeof heading,
                       "Active and known nodes  (%zu of %zu)  filter: %.32s",
                       count == 0u ? 0u : position + 1u, count, filter);
    else
        (void)snprintf(heading, sizeof heading,
                       "Active and known nodes  (%zu of %zu)",
                       count == 0u ? 0u : position + 1u, count);
    (void)attron(A_BOLD);
    clipped(stdscr, 3, 1, layout->columns - 2, heading);
    (void)attroff(A_BOLD);
    if (count == 0u)
        clipped(stdscr, 5, 2, layout->columns - 4,
                "No live announces received yet. Check interface status and wait for announces.");
    for (size_t i = first; i < count && i - first < rows; ++i) {
        char address[TUI_ADDRESS_DIGITS + 1u];
        char line[160];
        bool selected = state->has_node_selection && i == position;
        tui_hex_format(sorted[i].destination, LXMF_DESTINATION_LENGTH, address);
        (void)snprintf(line, sizeof line, "%-4s %s%s%s  %u hops  if:%llu  %s",
                       tui_state_node_serves_pages(&sorted[i]) ? "PAGE" : "",
                       sorted[i].name[0] != '\0' ? sorted[i].name : "",
                       sorted[i].name[0] != '\0' ? "  " : "", address,
                       (unsigned)sorted[i].hops,
                       (unsigned long long)sorted[i].interface_id,
                       sorted[i].reachable ? "reachable" : "stale");
        if (selected) (void)attron(A_REVERSE);
        clipped(stdscr, 5 + (int)(i - first), 2, layout->columns - 4, line);
        if (selected) (void)attroff(A_REVERSE);
    }
    clipped(stdscr, layout->hint_row, 0, layout->columns,
            "j/k select  / search  Enter details  R refresh path  B browser  C conversations");
}

static void draw_setting_row(const tui_state_t *state, const tui_layout_t *layout,
                             int row, tui_setting_item_t item, const char *text) {
    bool selected = state->setting_selected == item;
    if (selected) (void)attron(A_REVERSE);
    clipped(stdscr, row, 2, layout->columns - 4, text);
    if (selected) (void)attroff(A_REVERSE);
}

static void draw_settings(const tui_state_t *state, const tui_layout_t *layout) {
    char address[TUI_ADDRESS_DIGITS + 1u];
    char propagation[TUI_ADDRESS_DIGITS + 1u];
    char items[TUI_SETTING_COUNT][192];
    char line[192];
    size_t total = state->runtime != NULL
                       ? rns_runtime_interface_count(state->runtime) : 0u;
    size_t up = 0u;
    for (size_t i = 0u; i < total; ++i) {
        rns_runtime_interface_info_t info;
        if (rns_runtime_interface_info(state->runtime, i, &info) == RNS_OK &&
            info.state == RNS_RUNTIME_INTERFACE_UP && info.last_error == RNS_OK)
            ++up;
    }
    tui_hex_format(state->local, LXMF_DESTINATION_LENGTH, address);
    (void)attron(A_BOLD);
    clipped(stdscr, 2, 1, layout->columns - 2,
            "Settings (local identity loaded)");
    (void)attroff(A_BOLD);
    (void)snprintf(line, sizeof line, "LXMF: %s", address);
    clipped(stdscr, 3, 0, layout->columns, line);
    (void)snprintf(items[TUI_SETTING_DISPLAY_NAME], sizeof items[0],
                   "Display name: %s",
                   state->settings.display_name[0] != '\0'
                       ? state->settings.display_name : "(empty)");
    if (state->settings.has_stamp_cost)
        (void)snprintf(items[TUI_SETTING_STAMP_COST], sizeof items[0],
                       "Inbound stamp cost: %u (enforced and advertised)",
                       (unsigned)state->settings.stamp_cost);
    else
        (void)snprintf(items[TUI_SETTING_STAMP_COST], sizeof items[0], "%s",
                       "Inbound stamp cost: off");
    (void)snprintf(items[TUI_SETTING_ANNOUNCE_AT_START], sizeof items[0],
                   "Announce at start: %s",
                   state->settings.announce_at_start ? "yes" : "no");
    (void)snprintf(items[TUI_SETTING_ANNOUNCE_INTERVAL], sizeof items[0],
                   "Announce interval: %u minutes",
                   state->settings.announce_interval_minutes);
    if (state->settings.has_propagation_node) {
        tui_hex_format(state->settings.propagation_node, LXMF_DESTINATION_LENGTH,
                       propagation);
        uint8_t cost = 0u;
        tui_propagation_state_t route =
            tui_state_propagation_state(state, NULL, &cost);
        const char *state_text = route == TUI_PROPAGATION_READY ? "upload ready"
            : route == TUI_PROPAGATION_STALE ? "stale; waiting for fresh announce"
            : route == TUI_PROPAGATION_DISABLED ? "announce says disabled"
            : route == TUI_PROPAGATION_INVALID_COST ? "invalid advertised cost"
            : "waiting for verified announce";
        if (route == TUI_PROPAGATION_READY)
            (void)snprintf(items[TUI_SETTING_PROPAGATION_NODE], sizeof items[0],
                           "Propagation node: %s (%s, cost %u; sync unavailable)",
                           propagation, state_text, (unsigned)cost);
        else
            (void)snprintf(items[TUI_SETTING_PROPAGATION_NODE], sizeof items[0],
                           "Propagation node: %s (%s; sync unavailable)",
                           propagation, state_text);
    } else {
        (void)snprintf(items[TUI_SETTING_PROPAGATION_NODE], sizeof items[0], "%s",
                       "Propagation node: none (sync unavailable)");
    }
    (void)snprintf(items[TUI_SETTING_ANNOUNCE_NOW], sizeof items[0], "%s",
                   "Announce Now");

    const int setting_top = 4;
    size_t capacity = layout->divider_row > setting_top ?
                          (size_t)(layout->divider_row - setting_top) : 0u;
    size_t first = 0u;
    if (capacity < TUI_SETTING_COUNT &&
        (size_t)state->setting_selected >= capacity)
        first = (size_t)state->setting_selected - capacity + 1u;
    for (size_t i = first, shown = 0u;
         i < TUI_SETTING_COUNT && shown < capacity; ++i, ++shown)
        draw_setting_row(state, layout, setting_top + (int)shown,
                         (tui_setting_item_t)i, items[i]);

    if (capacity > TUI_SETTING_COUNT) {
        int row = setting_top + (int)TUI_SETTING_COUNT;
        (void)snprintf(line, sizeof line, "Interfaces: %zu/%zu up", up, total);
        clipped(stdscr, row, 2, layout->columns - 4, line);
        if (capacity > TUI_SETTING_COUNT + 1u) {
            if (!state->has_announce_result)
                (void)snprintf(line, sizeof line, "%s", "Last announce: never");
            else if (state->last_announce_result == RNS_OK)
                (void)snprintf(line, sizeof line,
                               "Last announce: success at monotonic %llu ms",
                               (unsigned long long)state->last_announce_ms);
            else
                (void)snprintf(line, sizeof line, "Last announce: %s",
                               rns_status_string(state->last_announce_result));
            clipped(stdscr, row + 1, 2, layout->columns - 4, line);
        }
    }

    (void)mvhline(layout->divider_row, 0, ACS_HLINE, layout->columns);
    if (state->field == TUI_FIELD_SETTING) {
        (void)snprintf(line, sizeof line, "Edit: %s",
                       tui_editor_text(&state->setting));
        clipped(stdscr, layout->input_row, 0, layout->columns, line);
        int cursor = 6 + (int)tui_editor_column(&state->setting);
        (void)move(layout->input_row,
                   cursor < layout->columns ? cursor : layout->columns - 1);
    } else
        clipped(stdscr, layout->input_row, 0, layout->columns, state->status);
    clipped(stdscr, layout->hint_row, 0, layout->columns,
            state->field == TUI_FIELD_SETTING
                ? "Enter save  Esc cancel  edit value"
                : "j/k select  Enter edit/toggle/announce  C conversations  q quit");
}

/* ------------------------------------------------------------------- micron */

/*
 * Micron carries 24 bit colour. Terminals do not, so each colour is reduced
 * to the closest entry the terminal actually has, and the resulting
 * foreground/background combinations are interned as curses pairs on demand.
 */
#define TUI_MAX_PAIRS 60

static struct { short foreground; short background; } tui_pairs[TUI_MAX_PAIRS];
static size_t tui_pair_count;

static short micron_color(uint32_t rgb) {
    if (rgb == RNS_MICRON_COLOR_DEFAULT) return -1;
    unsigned red = (rgb >> 16) & 0xffu;
    unsigned green = (rgb >> 8) & 0xffu;
    unsigned blue = rgb & 0xffu;
    if (COLORS >= 256) {
        if (red == green && green == blue) {
            if (red < 8u) return 16;
            if (red > 248u) return 231;
            return (short)(232u + (red - 8u) * 24u / 247u);
        }
        return (short)(16u + 36u * (red * 5u / 255u) + 6u * (green * 5u / 255u) +
                       blue * 5u / 255u);
    }
    return (short)((red > 127u ? 1 : 0) | (green > 127u ? 2 : 0) |
                   (blue > 127u ? 4 : 0));
}

static int micron_pair(uint32_t foreground, uint32_t background) {
    if (!has_colors() ||
        (foreground == RNS_MICRON_COLOR_DEFAULT &&
         background == RNS_MICRON_COLOR_DEFAULT))
        return 0;
    short want_fg = micron_color(foreground);
    short want_bg = micron_color(background);
    for (size_t i = 0u; i < tui_pair_count; ++i)
        if (tui_pairs[i].foreground == want_fg && tui_pairs[i].background == want_bg)
            return (int)i + 1;
    if (tui_pair_count == TUI_MAX_PAIRS || (int)tui_pair_count + 1 >= COLOR_PAIRS)
        return 0;
    if (init_pair((short)(tui_pair_count + 1u), want_fg, want_bg) == ERR) return 0;
    tui_pairs[tui_pair_count].foreground = want_fg;
    tui_pairs[tui_pair_count].background = want_bg;
    return (int)++tui_pair_count;
}

static chtype micron_attrs(const rns_micron_style *style) {
    chtype attrs = (chtype)COLOR_PAIR(micron_pair(style->foreground, style->background));
    if (style->bold) attrs |= A_BOLD;
    if (style->underline) attrs |= A_UNDERLINE;
#ifdef A_ITALIC
    if (style->italic) attrs |= A_ITALIC;
#else
    if (style->italic) attrs |= A_DIM;
#endif
    return attrs;
}

/* Writes at most limit columns of sanitised text and returns what it used. */
static int put_text(int y, int x, int limit, const char *text, chtype attrs) {
    char safe[2u * RNS_MICRON_TEXT_MAX];
    size_t bytes = 0u;
    int columns = 0;
    if (limit <= 0) return 0;
    size_t length = tui_text_sanitize((const uint8_t *)text, strlen(text), safe,
                                      sizeof safe);
    while (bytes < length && columns < limit) {
        size_t step = tui_utf8_length((const uint8_t *)safe + bytes, length - bytes);
        bytes += step != 0u ? step : 1u;
        ++columns;
    }
    if (columns == 0) return 0;
    (void)attron(attrs);
    (void)mvaddnstr(y, x, safe, (int)bytes);
    (void)attroff(attrs);
    return columns;
}

/* Renders one span into buffer and returns the text to draw. */
static const char *span_display(const rns_micron_page *page,
                                const rns_micron_span *span, char *buffer,
                                size_t capacity) {
    const char *text = rns_micron_span_text(page, span);
    switch (span->kind) {
        case RNS_MICRON_SPAN_FIELD: {
            unsigned width = span->width != 0u ? span->width : 1u;
            if (width > 64u) width = 64u;
            (void)snprintf(buffer, capacity, "[%-*.*s]", (int)width, (int)width,
                           span->masked ? "" : text);
            return buffer;
        }
        case RNS_MICRON_SPAN_CHECKBOX:
            (void)snprintf(buffer, capacity, "[%c] %s", span->prechecked ? 'x' : ' ',
                           text);
            return buffer;
        case RNS_MICRON_SPAN_RADIO:
            (void)snprintf(buffer, capacity, "(%c) %s", span->prechecked ? '*' : ' ',
                           text);
            return buffer;
        case RNS_MICRON_SPAN_TEXT:
        case RNS_MICRON_SPAN_LINK:
        default:
            return text;
    }
}

static int line_columns(const rns_micron_page *page, const rns_micron_line *line) {
    char buffer[RNS_MICRON_TEXT_MAX + 8u];
    size_t columns = 0u;
    for (uint16_t i = 0u; i < line->span_count; ++i) {
        const rns_micron_span *span = &page->spans[line->first_span + i];
        const char *text = span_display(page, span, buffer, sizeof buffer);
        columns += tui_utf8_columns(text, strlen(text));
    }
    return columns > INT_MAX ? INT_MAX : (int)columns;
}

/* Index of the page line carrying the nth link, or line_count when absent. */
static size_t link_line(const rns_micron_page *page, size_t nth) {
    size_t seen = 0u;
    for (size_t i = 0u; i < page->line_count; ++i) {
        const rns_micron_line *line = &page->lines[i];
        for (uint16_t j = 0u; j < line->span_count; ++j) {
            if (page->spans[line->first_span + j].kind != RNS_MICRON_SPAN_LINK)
                continue;
            if (seen++ == nth) return i;
        }
    }
    return page->line_count;
}

static void draw_micron_line(const tui_state_t *state, const rns_micron_line *line,
                             int y, int left, int width, size_t *link_ordinal) {
    const rns_micron_page *page = &state->page;
    char buffer[RNS_MICRON_TEXT_MAX + 8u];
    int indent = line->depth > 1u
                     ? (int)((line->depth - 1u) * RNS_MICRON_SECTION_INDENT)
                     : 0;
    if (indent > width / 2) indent = width / 2;
    int available = width - indent;
    if (available <= 0) return;
    if (line->divider) {
        chtype attrs = micron_attrs(&(rns_micron_style){RNS_MICRON_COLOR_DEFAULT,
                                                        RNS_MICRON_COLOR_DEFAULT,
                                                        false, false, false});
        for (int column = 0; column < available; ++column)
            (void)put_text(y, left + indent + column, 1, line->divider_char, attrs);
        return;
    }
    int used = line_columns(page, line);
    int x = left + indent;
    if (line->align == RNS_MICRON_ALIGN_CENTER && used < available)
        x += (available - used) / 2;
    else if (line->align == RNS_MICRON_ALIGN_RIGHT && used < available)
        x += available - used;
    int remaining = left + width - x;
    for (uint16_t i = 0u; i < line->span_count && remaining > 0; ++i) {
        const rns_micron_span *span = &page->spans[line->first_span + i];
        const char *text = span_display(page, span, buffer, sizeof buffer);
        chtype attrs = micron_attrs(&span->style);
        if (line->heading != 0u) attrs |= A_BOLD | A_REVERSE;
        if (span->kind == RNS_MICRON_SPAN_LINK) {
            bool selected = *link_ordinal == state->link_selected;
            attrs |= selected ? (A_REVERSE | A_BOLD) : A_UNDERLINE;
            ++*link_ordinal;
        }
        int drawn = put_text(y, x, remaining, text, attrs);
        x += drawn;
        remaining -= drawn;
    }
}

static void draw_browser(tui_state_t *state, const tui_layout_t *layout) {
    char title[RNS_MICRON_TEXT_MAX + 32u];
    (void)snprintf(title, sizeof title, "Browser  %s", state->url);
    (void)attron(A_BOLD);
    clipped(stdscr, 3, 1, layout->columns - 2, title);
    (void)attroff(A_BOLD);
    bool loading = false, failed = false;
    if (state->browser != NULL) {
        rns_browser_state_t browser_state = rns_browser_state(state->browser);
        char notice[TUI_STATUS_MAX];
        loading = browser_state == RNS_BROWSER_PATH_DISCOVERY ||
                  browser_state == RNS_BROWSER_LINK_ESTABLISHMENT ||
                  browser_state == RNS_BROWSER_REQUEST_TRANSMISSION;
        failed = browser_state == RNS_BROWSER_FAILED;
        if (loading) {
            (void)snprintf(notice, sizeof notice, "Loading remote page... %.0f%%",
                           rns_browser_progress(state->browser) * 100.0);
            clipped(stdscr, 4, 2, layout->columns - 4, notice);
        } else if (failed) {
            (void)snprintf(notice, sizeof notice, "Page load failed: %s",
                           tui_browser_error_text(state->browser));
            clipped(stdscr, 4, 2, layout->columns - 4, notice);
            clipped(stdscr, 5, 2, layout->columns - 4,
                    "Showing the previously loaded page:");
        }
    }
    /*
     * The retained page belongs to the previous URL. Showing it under a
     * progress line reads as though the new page had already arrived.
     */
    if (loading) {
        clipped(stdscr, layout->hint_row, 0, layout->columns,
                "Esc cancel  Backspace back  N network  q quit");
        return;
    }
    int body_top = failed ? 7 : 5;
    int body_rows = layout->hint_row - body_top - 1;
    if (body_rows < 1) return;

    const rns_micron_page *page = &state->page;
    /* Keep the selected link on screen as the selection walks the document. */
    size_t focus = link_line(page, state->link_selected);
    if (focus < page->line_count) {
        if (focus < state->page_scroll) state->page_scroll = focus;
        else if (focus >= state->page_scroll + (size_t)body_rows)
            state->page_scroll = focus - (size_t)body_rows + 1u;
    }
    if (state->page_scroll >= page->line_count)
        state->page_scroll = page->line_count != 0u ? page->line_count - 1u : 0u;

    size_t link_ordinal = 0u;
    for (size_t i = 0u; i < page->line_count; ++i) {
        if (i < state->page_scroll) {
            const rns_micron_line *skipped = &page->lines[i];
            for (uint16_t j = 0u; j < skipped->span_count; ++j)
                if (page->spans[skipped->first_span + j].kind == RNS_MICRON_SPAN_LINK)
                    ++link_ordinal;
            continue;
        }
        int y = body_top + (int)(i - state->page_scroll);
        if (y >= layout->hint_row - 1) break;
        draw_micron_line(state, &page->lines[i], y, 2, layout->columns - 4,
                         &link_ordinal);
    }
    if (page->truncated || page->unsupported)
        clipped(stdscr, layout->hint_row - 1, 2, layout->columns - 4,
                page->truncated ? "Page was truncated to fit the parser bounds"
                                : "Tables and partials are shown unformatted");
    clipped(stdscr, layout->hint_row, 0, layout->columns,
            "j/k link  Enter open  PgUp/PgDn scroll  Backspace back  R reload  N network");
}

static void draw_guide(const tui_layout_t *layout) {
    static const char *const lines[] = {
        "Nomad Chat Guide",
        "C Conversations   N Network   B Browser   S Settings   I Interfaces",
        "F validated Reticulum configuration",
        "j/k or arrows select; Enter activates; Esc returns to Conversations",
        "",
        "Conversations: 1/2/3 trust tabs, / search, a address, Enter compose",
        "Contact actions: i details, p pin, x block, t trust, u untrust",
        "Delivery: d selects direct or propagation-node upload for queued messages",
        "Network: / search nodes, Enter details, then b browse, m message, p relay",
        "Browser: j/k select links, Enter follow, Backspace back, R reload",
        "Settings: j/k select, Enter edit or activate Announce Now",
        "",
        "Delivery is proof-backed. A queued message is not shown as delivered.",
        "See docs/TUI.md for setup, persistence, bounds and current limitations."
    };
    for (size_t i = 0u; i < sizeof lines / sizeof lines[0]; ++i) {
        int row = 3 + (int)i;
        if (row >= layout->hint_row) break;
        clipped(stdscr, row, 2, layout->columns - 4, lines[i]);
    }
    clipped(stdscr, layout->hint_row, 0, layout->columns,
            "C or Esc conversations  N network  B browser  S settings  q quit");
}

void tui_render_draw(tui_state_t *state) {
    int rows, columns;
    if (state == NULL) return;
    getmaxyx(stdscr, rows, columns);
    (void)erase();
    tui_layout_t layout = layout_of(rows, columns);
    if (rows < TUI_MIN_ROWS || columns < TUI_MIN_COLUMNS) {
        draw_too_small(&layout);
        (void)refresh();
        return;
    }
    draw_chrome(state, &layout);
    if (state->screen == TUI_SCREEN_NETWORK) {
        draw_network(state, &layout);
        if (state->overlay == TUI_OVERLAY_NODE_ACTIONS) draw_node_popup(state);
        (void)refresh();
        return;
    }
    if (state->screen == TUI_SCREEN_SETTINGS) {
        draw_settings(state, &layout);
        (void)refresh();
        return;
    }
    if (state->screen == TUI_SCREEN_BROWSER) {
        draw_browser(state, &layout);
        (void)refresh();
        return;
    }
    if (state->screen == TUI_SCREEN_GUIDE) {
        draw_guide(&layout);
        (void)refresh();
        return;
    }
    if (state->screen == TUI_SCREEN_INTERFACES) {
        draw_interfaces(state, &layout);
        (void)refresh();
        return;
    }
    if (state->screen == TUI_SCREEN_CONFIG) {
        draw_config(state, &layout);
        (void)refresh();
        return;
    }
    if (!layout.narrow) draw_sidebar(state, &layout);
    draw_thread(state, &layout);
    draw_input(state, &layout);
    draw_conversation_overlay(state);
    (void)refresh();
}

int tui_render_dump(const tui_state_t *state, FILE *output) {
    char address[TUI_ADDRESS_DIGITS + 1u];
    if (state == NULL || output == NULL) return -1;
    tui_hex_format(state->local, LXMF_DESTINATION_LENGTH, address);
    (void)fprintf(output, "Nomad Chat\nIdentity: %s\n", address);
    if (state->screen == TUI_SCREEN_SETTINGS) {
        char propagation[TUI_ADDRESS_DIGITS + 1u];
        (void)fprintf(output, "Screen: Settings\nDisplay name: %s\n",
                      state->settings.display_name);
        if (state->settings.has_stamp_cost)
            (void)fprintf(output, "Stamp cost: %u (enforced and advertised)\n",
                          (unsigned)state->settings.stamp_cost);
        else
            (void)fprintf(output, "Stamp cost: off\n");
        (void)fprintf(output, "Announce at start: %s\nAnnounce interval: %u minutes\n",
                      state->settings.announce_at_start ? "yes" : "no",
                      state->settings.announce_interval_minutes);
        if (state->settings.has_propagation_node) {
            tui_hex_format(state->settings.propagation_node,
                           LXMF_DESTINATION_LENGTH, propagation);
            uint8_t cost = 0u;
            tui_propagation_state_t route =
                tui_state_propagation_state(state, NULL, &cost);
            const char *state_text = route == TUI_PROPAGATION_STALE
                ? "stale; waiting for fresh announce"
                : route == TUI_PROPAGATION_DISABLED
                    ? "announce says disabled"
                : route == TUI_PROPAGATION_INVALID_COST
                    ? "invalid advertised cost"
                    : "waiting for verified announce";
            if (route == TUI_PROPAGATION_READY)
                (void)fprintf(output,
                    "Propagation node: %s (upload ready, cost %u; sync unavailable)\n",
                    propagation, (unsigned)cost);
            else
                (void)fprintf(output,
                    "Propagation node: %s (%s; sync unavailable)\n",
                    propagation, state_text);
        } else {
            (void)fprintf(output, "Propagation node: none\n");
        }
        (void)fprintf(output, "Last announce: %s\nStatus: %s\n",
                      !state->has_announce_result ? "never"
                      : rns_status_string(state->last_announce_result),
                      state->status);
        return ferror(output) ? -1 : 0;
    }
    if (state->screen == TUI_SCREEN_GUIDE) {
        (void)fprintf(output,
            "Screen: Guide\n"
            "Conversations: C, trust tabs 1/2/3, / search, a address, Enter compose\n"
            "Network: N, / search, Enter node actions, b browse, m message, r path\n"
            "Browser: B, j/k links, Enter follow, Backspace back, R reload\n"
            "Settings: S, j/k select, Enter edit/activate\n"
            "Escape: close active layer or return to Conversations\n"
            "Status: %s\n", state->status);
        return ferror(output) ? -1 : 0;
    }
    if (state->screen == TUI_SCREEN_INTERFACES) {
        size_t count = tui_state_interface_count(state);
        (void)fprintf(output, "Screen: Interfaces\nInterfaces: %zu\n", count);
        for (size_t i = 0u; i < count; ++i) {
            rns_runtime_interface_info_t info;
            if (!tui_state_interface_info(state, i, &info)) continue;
            (void)fprintf(output,
                          "%c %s type=%s state=%s rx=%llu/%llu tx=%llu/%llu dropped=%llu error=%s\n",
                          i == state->interface_selected ? '>' : ' ', info.name,
                          rns_config_interface_type_name(info.type),
                          interface_state_name(info.state),
                          (unsigned long long)info.packets_received,
                          (unsigned long long)info.bytes_received,
                          (unsigned long long)info.packets_sent,
                          (unsigned long long)info.bytes_sent,
                          (unsigned long long)info.packets_dropped,
                          rns_status_string(info.last_error));
        }
        (void)fprintf(output, "Status: %s\n", state->status);
        return ferror(output) ? -1 : 0;
    }
    if (state->screen == TUI_SCREEN_CONFIG) {
        (void)fprintf(output, "Screen: Config\nPath: %s\n",
                      state->config_attempted ? state->config_path : "none");
        if (!state->config_attempted)
            (void)fprintf(output, "Validation: not loaded\n");
        else if (!state->config_valid) {
            (void)fprintf(output, "Validation: invalid\n");
            if (state->config_diagnostic.message[0] != '\0')
                (void)fprintf(output, "Diagnostic: line %zu: %s\n",
                              state->config_diagnostic.line,
                              state->config_diagnostic.message);
        } else {
            (void)fprintf(output,
                          "Validation: valid\nTransport: %s\nShared instance: %s\nPanic on error: %s\nInterfaces: %zu\n",
                          state->parsed_config.enable_transport ? "yes" : "no",
                          state->parsed_config.share_instance ? "yes" : "no",
                          state->parsed_config.panic_on_interface_error ? "yes" : "no",
                          state->parsed_config.interface_count);
            for (size_t i = 0u; i < state->parsed_config.interface_count; ++i) {
                char line[384];
                config_interface_line(&state->parsed_config.interfaces[i],
                                      line, sizeof line);
                (void)fprintf(output, "%s\n", line);
            }
        }
        (void)fprintf(output, "Status: %s\n", state->status);
        return ferror(output) ? -1 : 0;
    }
    const tui_contact_t *contact = tui_state_selected_contact(state);
    if (contact != NULL) {
        char peer[TUI_ADDRESS_DIGITS + 1u];
        tui_hex_format(contact->peer, LXMF_DESTINATION_LENGTH, peer);
        (void)fprintf(output, "Conversation: %s\n", peer);
    } else {
        (void)fprintf(output, "Conversation: none\n");
    }
    (void)fprintf(output, "Network: %s\n",
                  state->runtime == NULL ? "OFFLINE (local outbox only)"
                  : tui_state_link_ready(state) ? "ONLINE" : "NO LINK");
    (void)fprintf(output, "Compose delivery: %s\n",
                  lxmf_delivery_method_string(state->compose_delivery_method));
    (void)fprintf(output, "Conversations: %zu\n", state->contact_count);
    for (size_t i = 0u; i < state->contact_count; ++i) {
        char peer[TUI_ADDRESS_DIGITS + 1u];
        tui_hex_format(state->contacts[i].peer, LXMF_DESTINATION_LENGTH, peer);
        (void)fprintf(output, "%c %s (%zu messages)\n",
                      i == state->selected ? '>' : ' ', peer,
                      state->contacts[i].messages);
    }
    size_t count = tui_state_thread_count(state);
    (void)fprintf(output, "Messages: %zu\n", count);
    for (size_t i = 0u; i < count; ++i) {
        const tui_message_t *message = tui_state_thread_message(state, i);
        bool outgoing = tui_state_outgoing(state, message);
        (void)fprintf(output, "%s %s ", outgoing ? ">" : "<",
                      outgoing ? delivery_marker(message->value.status) : "   ");
        tui_text_escape(output, message->value.content.data,
                        message->value.content.len);
        (void)fputc('\n', output);
    }
    (void)fprintf(output, "Status: %s\n", state->status);
    return ferror(output) ? -1 : 0;
}
