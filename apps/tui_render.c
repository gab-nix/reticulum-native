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
    char lines[10][96];
    const char *pointers[10];
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

    (void)snprintf(heading, sizeof heading, "Active and known nodes  (%zu of %zu)",
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
            "j/k select  Enter details  R refresh path  B browser  C conversations  q quit");
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
    if (state->screen == TUI_SCREEN_BROWSER) {
        draw_browser(state, &layout);
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
