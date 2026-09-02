#include "tui_render.h"

#include "tui_text.h"

#include <curses.h>
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

static void centered_box(const char *title, const char *const *lines, size_t count) {
    int rows, columns;
    getmaxyx(stdscr, rows, columns);
    int width = columns > 70 ? 68 : columns - 4;
    int height = (int)count + 4;
    if (width < 20 || rows < height + 2) return;
    WINDOW *popup = newwin(height, width, (rows - height) / 2, (columns - width) / 2);
    if (popup == NULL) return;
    (void)wbkgd(popup, A_REVERSE);
    (void)box(popup, 0, 0);
    (void)wattron(popup, A_BOLD);
    clipped(popup, 1, 2, width - 4, title);
    (void)wattroff(popup, A_BOLD);
    for (size_t i = 0u; i < count; ++i)
        clipped(popup, 2 + (int)i, 2, width - 4, lines[i]);
    (void)wrefresh(popup);
    (void)delwin(popup);
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
            "[C]hats [N]etwork [B]rowser N[o]de [S]ettings [G]uide [L]ogs [R]RC");
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
        case TUI_FIELD_COMPOSE: editor = &state->composer; prompt = "Message: "; break;
        case TUI_FIELD_SEARCH: editor = &state->search; prompt = "Search: "; break;
        case TUI_FIELD_ADDRESS: editor = &state->address; prompt = "Address: "; break;
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
                           : "1/2/3 trust tabs  / search  i info  p pin  t/u trust  ? help");
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
            "Composer: arrows Home End Del Backspace Ctrl-A/E/U/K/W",
            "Configured networks send and receive opportunistic LXMF. Press ? or Esc to close."
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

static void draw_network(const tui_state_t *state, const tui_layout_t *layout) {
    rns_node_record sorted[RNS_NODE_REGISTRY_MAX];
    (void)attron(A_BOLD);
    clipped(stdscr, 3, 1, layout->columns - 2, "Active and known nodes");
    (void)attroff(A_BOLD);
    if (state->nodes.count == 0u)
        clipped(stdscr, 5, 2, layout->columns - 4,
                "No live announces received yet. Check interface status and wait for announces.");
    size_t count = rns_node_registry_sorted(&state->nodes, sorted, RNS_NODE_REGISTRY_MAX);
    for (size_t i = 0u; i < count && (int)i < layout->rows - 8; ++i) {
        char address[TUI_ADDRESS_DIGITS + 1u];
        char line[160];
        bool selected = i == state->node_selected;
        tui_hex_format(sorted[i].destination, LXMF_DESTINATION_LENGTH, address);
        (void)snprintf(line, sizeof line, "%s%s%s  %u hops  if:%llu  %s",
                       sorted[i].name[0] != '\0' ? sorted[i].name : "",
                       sorted[i].name[0] != '\0' ? "  " : "", address,
                       (unsigned)sorted[i].hops,
                       (unsigned long long)sorted[i].interface_id,
                       sorted[i].reachable ? "reachable" : "stale");
        if (selected) (void)attron(A_REVERSE);
        clipped(stdscr, 5 + (int)i, 2, layout->columns - 4, line);
        if (selected) (void)attroff(A_REVERSE);
    }
    clipped(stdscr, layout->hint_row, 0, layout->columns,
            "j/k select  Enter actions  R refresh path  B browser  C conversations  q quit");
    if (state->overlay == TUI_OVERLAY_NODE_ACTIONS && count > 0u) {
        static const char *const actions[] = {
            "B: browse Nomad page", "M: message associated LXMF inbox", "Esc: cancel"
        };
        centered_box("Known node actions", actions, sizeof actions / sizeof actions[0]);
    }
}

static void draw_browser(const tui_state_t *state, const tui_layout_t *layout) {
    char title[RNS_MICRON_TEXT_MAX + 32u];
    (void)snprintf(title, sizeof title, "Browser  %s", state->url);
    (void)attron(A_BOLD);
    clipped(stdscr, 3, 1, layout->columns - 2, title);
    (void)attroff(A_BOLD);
    if (state->browser != NULL) {
        rns_browser_state_t browser_state = rns_browser_state(state->browser);
        char notice[TUI_STATUS_MAX];
        if (browser_state == RNS_BROWSER_PATH_DISCOVERY ||
            browser_state == RNS_BROWSER_LINK_ESTABLISHMENT ||
            browser_state == RNS_BROWSER_REQUEST_TRANSMISSION) {
            (void)snprintf(notice, sizeof notice, "Loading remote page... %.0f%%",
                           rns_browser_progress(state->browser) * 100.0);
            clipped(stdscr, 4, 2, layout->columns - 4, notice);
        } else if (browser_state == RNS_BROWSER_FAILED) {
            (void)snprintf(notice, sizeof notice, "Page load failed: %s",
                           rns_status_string(rns_browser_error(state->browser)));
            clipped(stdscr, 4, 2, layout->columns - 4, notice);
        }
    }
    size_t link = 0u;
    for (size_t i = 0u; i < state->page.count && (int)i < layout->rows - 8; ++i) {
        const rns_micron_item *item = &state->page.items[i];
        char line[2u * RNS_MICRON_TEXT_MAX + 16u];
        if (item->kind == RNS_MICRON_LINK) {
            (void)snprintf(line, sizeof line, "[%c] %s -> %s",
                           link == state->link_selected ? '>' : ' ', item->text,
                           item->target);
            ++link;
        } else if (item->kind == RNS_MICRON_MEDIA) {
            (void)snprintf(line, sizeof line, "[media] %s", item->target);
        } else {
            (void)snprintf(line, sizeof line, "%s", item->text);
        }
        bool heading = item->kind == RNS_MICRON_HEADING;
        if (heading) (void)attron(A_BOLD);
        clipped(stdscr, 5 + (int)i, 2, layout->columns - 4, line);
        if (heading) (void)attroff(A_BOLD);
    }
    clipped(stdscr, layout->hint_row, 0, layout->columns,
            "j/k select  Enter open  Backspace back  R reload  Esc cancel  N network");
}

void tui_render_draw(const tui_state_t *state) {
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
        (void)refresh();
        return;
    }
    if (state->screen == TUI_SCREEN_BROWSER) {
        draw_browser(state, &layout);
        (void)refresh();
        return;
    }
    if (!layout.narrow) draw_sidebar(state, &layout);
    draw_thread(state, &layout);
    draw_input(state, &layout);
    (void)refresh();
    draw_conversation_overlay(state);
}

int tui_render_dump(const tui_state_t *state, FILE *output) {
    char address[TUI_ADDRESS_DIGITS + 1u];
    if (state == NULL || output == NULL) return -1;
    tui_hex_format(state->local, LXMF_DESTINATION_LENGTH, address);
    (void)fprintf(output, "Nomad Chat\nIdentity: %s\n", address);
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
