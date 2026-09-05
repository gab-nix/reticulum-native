#include "tui_render.h"

#include "tui_text.h"
#include "tui_qr.h"

#include <curses.h>
#include <limits.h>
#include <stdarg.h>
#include <stdlib.h>
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
    int input_rows;
    int hint_row;
    int legend_row;
} tui_layout_t;

static const char *renderer_name(uint8_t renderer) {
    switch (renderer) {
        case LXMF_RENDERER_PLAIN: return "plain";
        case LXMF_RENDERER_MICRON: return "micron";
        case LXMF_RENDERER_MARKDOWN: return "markdown";
        case LXMF_RENDERER_BBCODE: return "bbcode";
    }
    return "unknown";
}

static void append_summary(char *output, size_t capacity, const char *format,
                           ...) {
    size_t used = strlen(output);
    if (used >= capacity) return;
    va_list arguments;
    va_start(arguments, format);
    (void)vsnprintf(output + used, capacity - used, format, arguments);
    va_end(arguments);
}

void tui_render_message_metadata(const tui_message_metadata_t *metadata,
                                 char *output, size_t capacity) {
    if (capacity == 0u) return;
    output[0] = '\0';
    if (metadata == NULL) return;
    if (metadata->state == TUI_METADATA_MISSING_PACKED) {
        append_summary(output, capacity, " [metadata: packed message unavailable]");
        return;
    }
    if (metadata->state == TUI_METADATA_MALFORMED) {
        append_summary(output, capacity, " [metadata: malformed]");
        return;
    }
    if (metadata->state == TUI_METADATA_UNAVAILABLE) {
        append_summary(output, capacity, " [metadata: storage read failed]");
        return;
    }
    uint32_t mask = metadata->present_mask;
    bool has_item = false;
    if (mask != 0u) append_summary(output, capacity, " [");
    if ((mask & LXMF_STANDARD_RENDERER) != 0u) {
        append_summary(output, capacity, "renderer:%s",
                       renderer_name(metadata->renderer));
        has_item = true;
    }
    if ((mask & LXMF_STANDARD_REPLY_TO) != 0u) {
        append_summary(output, capacity, "%sreply:%02x%02x%02x%02x",
                       has_item ? " " : "",
                       metadata->reply_to[0], metadata->reply_to[1],
                       metadata->reply_to[2], metadata->reply_to[3]);
        has_item = true;
    }
    if ((mask & LXMF_STANDARD_REPLY_QUOTE) != 0u)
        append_summary(output, capacity, "%squote:\"%s\"",
                       has_item ? " " : "", metadata->reply_quote);
    if ((mask & LXMF_STANDARD_REACTION) != 0u)
        append_summary(output, capacity, " reaction:%s@%02x%02x%02x%02x",
                       metadata->reaction, metadata->reaction_to[0],
                       metadata->reaction_to[1], metadata->reaction_to[2],
                       metadata->reaction_to[3]);
    if ((mask & LXMF_STANDARD_THREAD) != 0u)
        append_summary(output, capacity, " thread:%02x%02x%02x%02x",
                       metadata->thread[0], metadata->thread[1],
                       metadata->thread[2], metadata->thread[3]);
    if ((mask & LXMF_STANDARD_ATTACHMENTS) != 0u)
        append_summary(output, capacity, " files:%zu", metadata->attachment_count);
    if ((mask & LXMF_STANDARD_IMAGE) != 0u) {
        append_summary(output, capacity, " image:%zuB", metadata->image_size);
        if (metadata->image_format_kind == LXMF_MEDIA_FORMAT_TEXT)
            append_summary(output, capacity, "/%s",
                           metadata->image_text_format);
        else if (metadata->image_format_kind == LXMF_MEDIA_FORMAT_INTEGER)
            append_summary(output, capacity, "/format-%u",
                           (unsigned)metadata->image_integer_format);
    }
    if ((mask & LXMF_STANDARD_AUDIO) != 0u) {
        append_summary(output, capacity, " audio:%zuB", metadata->audio_size);
        if (metadata->audio_format_kind == LXMF_MEDIA_FORMAT_TEXT)
            append_summary(output, capacity, "/%s",
                           metadata->audio_text_format);
        else if (metadata->audio_format_kind == LXMF_MEDIA_FORMAT_INTEGER)
            append_summary(output, capacity, "/format-%u",
                           (unsigned)metadata->audio_integer_format);
    }
    if (mask != 0u) append_summary(output, capacity, "]");
}

void tui_render_compose_reference(const tui_compose_reference_t *reference,
                                  char *output, size_t capacity) {
    if (capacity == 0u) return;
    output[0] = '\0';
    if (reference == NULL ||
        reference->kind == TUI_COMPOSE_REFERENCE_NONE) return;
    (void)snprintf(output, capacity, "%s %02x%02x%02x%02x: %.96s",
                   reference->kind == TUI_COMPOSE_REFERENCE_REPLY
                       ? "Reply to" : "React to",
                   reference->message_id[0], reference->message_id[1],
                   reference->message_id[2], reference->message_id[3],
                   reference->preview);
}

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
    layout.input_rows = 1;
    layout.hint_row = rows - 2;
    layout.legend_row = rows - 1;
    return layout;
}

static void clipped(WINDOW *window, int y, int x, int width, const char *text) {
    if (width <= 0 || text == NULL) return;
    int rows, columns; getmaxyx(window, rows, columns);
    if (y < 0 || y >= rows || x < 0 || x >= columns) return;
    if (width > columns - x) width = columns - x;
    size_t length = strlen(text), cells = 0u;
    if (wmove(window, y, x) == ERR) return;
    for (size_t at = 0u; at < length;) {
        size_t n = tui_utf8_length((const uint8_t *)text + at, length - at);
        size_t w = tui_text_cell_width(text + at, length - at);
        if (cells + w > (size_t)width) break;
        if (n == 0u || (unsigned char)text[at] < 32u ||
            (unsigned char)text[at] == 127u)
            (void)waddch(window, ' ');
        else (void)waddnstr(window, text + at, (int)n);
        cells += w; at += n != 0u ? n : 1u;
    }
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

static bool draw_address_qr(const char *address, const char *title) {
    int rows, columns; getmaxyx(stdscr, rows, columns);
    tui_qr_t qr;
    if (!tui_qr_fits(rows, columns) || MB_CUR_MAX < 3 ||
        !tui_qr_address(address, 32u, &qr)) return false;
    const int height = 23, width = 40;
    int top = (rows - height) / 2, left = (columns - width) / 2;
    (void)attron(A_REVERSE);
    for (int y = 0; y < height; ++y) {
        (void)move(top + y, left);
        for (int x = 0; x < width; ++x) (void)addch(' ');
    }
    clipped(stdscr, top, left + 2, width - 4, title);
    clipped(stdscr, top + 1, left + 3, 32, address);
    for (size_t row = 0; row < TUI_QR_ROWS; ++row)
        (void)mvaddstr(top + 3 + (int)row, left + 3, qr.rows[row]);
    clipped(stdscr, top + 21, left + 2, width - 4, "Public address only. Esc/i closes.");
    (void)attroff(A_REVERSE);
    return true;
}

static void draw_local_qr(const tui_state_t *state) {
    char address[33]; tui_hex_format(state->local, 16u, address);
    if (!draw_address_qr(address, "My LXMF address")) {
        const char *lines[] = {address, "QR needs a UTF-8 terminal at least 40x23.", "Esc/U closes."};
        centered_box("My LXMF address", lines, 3u);
    }
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
    size_t unread = 0u, up = 0u;
    uint64_t rx = 0u, tx = 0u;
    for (size_t i = 0u; i < state->contact_count; ++i) unread += state->contacts[i].unread;
    for (size_t i = 0u; i < state->interfaces.count; ++i) {
        const rns_runtime_interface_info_t *info = &state->interfaces.items[i];
        if (info->state == RNS_RUNTIME_INTERFACE_UP) ++up;
        rx += info->bytes_received; tx += info->bytes_sent;
    }
    const char *mode = state->command_active ? "COMMAND"
        : state->field == TUI_FIELD_NONE ? "NORMAL" : "INSERT";
    if (layout->columns < 72)
        (void)snprintf(header, sizeof header, " %s | %s | N:%zu U:%zu", mode, link, state->nodes.count, unread);
    else
        (void)snprintf(header, sizeof header,
            " Nomad %s | %s %zu/%zu | Nodes:%zu Unread:%zu | RX:%lluK TX:%lluK | %.8s",
            mode, link, up, state->interfaces.count, state->nodes.count, unread,
            (unsigned long long)(rx / 1024u), (unsigned long long)(tx / 1024u), address + 24u);
    (void)attron(A_REVERSE | A_BOLD);
    clipped(stdscr, 0, 0, layout->columns, header);
    for (int x = (int)strlen(header); x < layout->columns; ++x) (void)addch(' ');
    (void)attroff(A_REVERSE | A_BOLD);
    clipped(stdscr, 1, 0, layout->columns,
            "[C]hats [N]etwork [B]rowser [S]ettings [I]faces [F]Config [G]uide [L]ogs [R]RC");
}

static const char *propagation_sync_phase_name(lxmf_pn_session_state_t state) {
    switch (state) {
        case LXMF_PN_IDLE: return "idle";
        case LXMF_PN_PATH: return "finding path";
        case LXMF_PN_LINK: return "authenticating link";
        case LXMF_PN_LIST: return "listing";
        case LXMF_PN_DOWNLOAD: return "downloading";
        case LXMF_PN_ACK: return "acknowledging";
        case LXMF_PN_UPLOAD: return "uploading";
        case LXMF_PN_COMPLETE: return "complete";
        case LXMF_PN_FAILED: return "failed";
        case LXMF_PN_CANCELLED: return "cancelled";
    }
    return "unknown";
}

static const char *rrc_state_name(rns_rrc_session_state_t state) {
    switch (state) {
        case RNS_RRC_SESSION_DISCONNECTED: return "disconnected";
        case RNS_RRC_SESSION_DISCOVERING: return "discovering path";
        case RNS_RRC_SESSION_LINKING: return "authenticating link";
        case RNS_RRC_SESSION_HELLO: return "waiting for WELCOME";
        case RNS_RRC_SESSION_CONNECTED: return "connected";
        case RNS_RRC_SESSION_RECONNECT_WAIT: return "reconnect wait";
        case RNS_RRC_SESSION_FAILED: return "failed";
    }
    return "invalid";
}

static const char *rrc_item_value(const tui_state_t *state,
                                  tui_rrc_item_t item) {
    if (state->field == TUI_FIELD_RRC && state->rrc.selected == item)
        return tui_editor_text(&state->setting);
    const char *value = tui_rrc_edit_value(&state->rrc, item);
    if (value != NULL) return value;
    if (item == TUI_RRC_ITEM_CONNECT)
        return state->rrc.session == NULL ? "Connect" : "Disconnect";
    if (item == TUI_RRC_ITEM_RECONNECT)
        return state->rrc.auto_reconnect ? "On" : "Off";
    if (item == TUI_RRC_ITEM_JOIN) return "Send JOIN";
    if (item == TUI_RRC_ITEM_PART) return "Send PART";
    if (item == TUI_RRC_ITEM_SEND) return "Send message";
    return "";
}

size_t tui_render_rrc_first_item(tui_rrc_item_t selected, size_t visible_rows) {
    if (visible_rows == 0u) visible_rows = 1u;
    if (visible_rows >= TUI_RRC_ITEM_COUNT ||
        (size_t)selected < visible_rows) return 0u;
    return (size_t)selected - visible_rows + 1u;
}

static void draw_rrc(const tui_state_t *state, const tui_layout_t *layout) {
    static const char *const labels[] = {
        "Hub address", "Hub public identity", "Nick", "Connection",
        "Auto reconnect", "Room", "Join", "Part", "Message", "Send"};
    char heading[256];
    (void)snprintf(heading, sizeof heading, "RRC  state: %s  attempts: %zu",
                   rrc_state_name(state->rrc.info.state),
                   state->rrc.info.hello_attempts);
    (void)attron(A_BOLD);
    clipped(stdscr, 3, 1, layout->columns - 2, heading);
    (void)attroff(A_BOLD);
    int row = 4;
    int item_rows = layout->hint_row - 3 - row;
    if (item_rows < 1) item_rows = 1;
    size_t first_item = tui_render_rrc_first_item(
        state->rrc.selected, (size_t)item_rows);
    for (size_t i = first_item; i < TUI_RRC_ITEM_COUNT &&
         (int)(i - first_item) < item_rows && row < layout->hint_row - 3;
         ++i, ++row) {
        char line[512];
        (void)snprintf(line, sizeof line, "%-20s %s", labels[i],
                       rrc_item_value(state, (tui_rrc_item_t)i));
        if (i == (size_t)state->rrc.selected) (void)attron(A_REVERSE);
        clipped(stdscr, row, 2, layout->columns - 4, line);
        if (i == (size_t)state->rrc.selected) (void)attroff(A_REVERSE);
    }
    if (state->rrc.info.state == RNS_RRC_SESSION_CONNECTED &&
        row < layout->hint_row - 2) {
        char hub_name[RNS_RRC_MAX_HUB_NAME_BYTES + 1u];
        char caps[256];
        (void)tui_text_sanitize(state->rrc.info.welcome.hub_name,
            state->rrc.info.welcome.hub_name_length, hub_name,
            sizeof hub_name);
        (void)snprintf(caps, sizeof caps,
            "Hub %.96s  msg:%zuB room:%zuB rooms:%zu rate:%zu/min resource:%s",
            hub_name[0] != '\0' ? hub_name : "(unnamed)",
            state->rrc.info.welcome.max_message_bytes,
            state->rrc.info.welcome.max_room_bytes,
            state->rrc.info.welcome.max_rooms,
            state->rrc.info.welcome.rate_per_minute,
            state->rrc.info.welcome.resource_envelopes ? "advertised" : "no");
        clipped(stdscr, row++, 2, layout->columns - 4, caps);
    }
    if (state->rrc.motd[0] != '\0' && row < layout->hint_row - 1) {
        char motd[448];
        (void)snprintf(motd, sizeof motd, "MOTD: %s", state->rrc.motd);
        clipped(stdscr, row++, 2, layout->columns - 4, motd);
    }
    for (size_t i = 0u; i < state->rrc.room_count &&
         row < layout->hint_row - 1; ++i) {
        const tui_rrc_room_t *room = &state->rrc.rooms[i];
        const char *room_state = room->part_pending ? "parting"
                                 : room->join_pending ? "joining"
                                 : room->joined ? "joined" : "saved";
        char line[448];
        (void)snprintf(line, sizeof line, "#%s  %s  %zu member%s",
                       room->name, room_state, room->member_count,
                       room->member_count == 1u ? "" : "s");
        clipped(stdscr, row++, 2, layout->columns - 4, line);
    }
    if (row < layout->hint_row - 1)
        clipped(stdscr, row++, 2, layout->columns - 4,
            "Bounded live room state; persistent history and Resource envelopes are unavailable.");
    size_t shown = state->rrc.message_count;
    size_t available = row < layout->hint_row ? (size_t)(layout->hint_row - row) : 0u;
    if (shown > available) shown = available;
    size_t first = state->rrc.message_count - shown;
    for (size_t i = first; i < state->rrc.message_count; ++i) {
        const tui_rrc_message_t *message = &state->rrc.messages[i];
        char line[512];
        (void)snprintf(line, sizeof line, "#%s <%s> %s",
                       message->room[0] != '\0' ? message->room : "*",
                       message->nick[0] != '\0' ? message->nick : "unknown",
                       message->body);
        clipped(stdscr, row++, 2, layout->columns - 4, line);
    }
    clipped(stdscr, layout->hint_row, 0, layout->columns,
            "j/k select  Enter edit/action  r status  C or Esc conversations  q quit");
    clipped(stdscr, layout->input_row, 0, layout->columns,
            state->rrc.status);
}

static void draw_interfaces(const tui_state_t *state,
                            const tui_layout_t *layout) {
    size_t count = tui_state_interface_count(state);
    size_t selected = state->interfaces.selected_index < count
                          ? state->interfaces.selected_index : 0u;
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
        size_t visible = rows > 0 ? (size_t)rows / 3u : 0u;
        size_t first = tui_interfaces_first(&state->interfaces, visible);
        for (size_t i = first; i < count && i - first < visible; ++i) {
            rns_runtime_interface_info_t info;
            if (!tui_state_interface_info(state, i, &info)) continue;
            char lines[3][TUI_INTERFACE_LINE_MAX];
            int width = layout->columns - 4;
            tui_interfaces_format(&info, width > 0 ? (size_t)width : 0u, lines);
            if (i == selected) (void)attron(A_REVERSE);
            for (size_t line = 0u; line < 3u; ++line)
                clipped(stdscr, 5 + (int)((i - first) * 3u + line), 2,
                        width, lines[line]);
            if (i == selected) (void)attroff(A_REVERSE);
        }
    }
    clipped(stdscr, layout->hint_row, 0, layout->columns,
            "j/k scroll  r refresh  C or Esc conversations  q quit");
}

static void config_interface_line(const rns_config_interface_t *interface,
                                  char *line, size_t capacity) {
    const char *state = interface->enabled ? "enabled" : "disabled";
    switch (interface->type) {
        case RNS_CONFIG_PROVIDER:
            (void)snprintf(line, capacity, "%s  Runtime provider  %s",
                           interface->name, state);
            break;
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
        (void)snprintf(line, sizeof line, "Invalid: %.150s",
                       state->config_path);
        clipped(stdscr, 5, 2, layout->columns - 4, line);
        if (state->config_diagnostic.message[0] != '\0') {
            (void)snprintf(line, sizeof line, "Line %zu: %.128s",
                           state->config_diagnostic.line,
                           state->config_diagnostic.message);
            clipped(stdscr, 6, 2, layout->columns - 4, line);
        }
    } else {
        char line[384];
        (void)snprintf(line, sizeof line, "File: %.376s", state->config_path);
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

static void message_text(const tui_state_t *state, const tui_message_t *message,
                          char *rendered, size_t capacity) {
    char text[LXMF_STORE_MAX_CONTENT + 1u], metadata[448];
    size_t length = message->value.content.len;
    if (length > sizeof text - 1u) length = sizeof text - 1u;
    for (size_t i = 0u; i < length; ++i) {
        unsigned char c = message->value.content.data[i];
        text[i] = (c < 32u && c != '\n') || c == 127u ? ' ' : (char)c;
    }
    text[length] = '\0';
    tui_render_message_metadata(&message->metadata, metadata, sizeof metadata);
    bool outgoing = tui_state_outgoing(state, message);
    bool referenced = state->compose_reference.kind != TUI_COMPOSE_REFERENCE_NONE &&
        memcmp(state->compose_reference.message_id, message->value.message_id,
               LXMF_MESSAGE_ID_LENGTH) == 0;
    (void)snprintf(rendered, capacity, "%c%s %s %s%s", referenced ? '*' : ' ',
        outgoing ? ">" : "<", outgoing ? delivery_marker(message->value.status) : "   ",
        text, metadata);
}

static void draw_thread(tui_state_t *state, const tui_layout_t *layout) {
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

    size_t messages = tui_state_thread_count(state), total = 0u;
    char rendered[LXMF_STORE_MAX_CONTENT + 512u];
    for (size_t index = 0u; index < messages; ++index) {
        const tui_message_t *message = tui_state_thread_message(state, index);
        if (message == NULL) continue;
        message_text(state, message, rendered, sizeof rendered);
        size_t offset = 0u, start, bytes, length = strlen(rendered);
        while (tui_text_wrap_next(rendered, length, (size_t)layout->pane_width,
                                  &offset, &start, &bytes)) ++total;
    }
    size_t capacity = layout->content_rows > 0 ? (size_t)layout->content_rows : 0u;
    state->thread_scroll_limit = total > capacity ? total - capacity : 0u;
    state->thread_layout_valid = true;
    if (state->scroll > state->thread_scroll_limit) state->scroll = state->thread_scroll_limit;
    size_t end = total > state->scroll ? total - state->scroll : 0u;
    size_t first = end > capacity ? end - capacity : 0u;
    size_t row = 0u;
    for (size_t index = 0u; index < messages && row < end; ++index) {
        const tui_message_t *message = tui_state_thread_message(state, index);
        if (message == NULL) break;
        message_text(state, message, rendered, sizeof rendered);
        size_t length = strlen(rendered), offset = 0u, start, bytes;
        while (row < end && tui_text_wrap_next(rendered, length,
                (size_t)layout->pane_width, &offset, &start, &bytes)) {
            if (row >= first) {
                state->thread_visible_last = index;
                char saved = rendered[start + bytes]; rendered[start + bytes] = '\0';
                clipped(stdscr, layout->content_top + (int)(row - first),
                        layout->pane_x, layout->pane_width, rendered + start);
                rendered[start + bytes] = saved;
            }
            ++row;
        }
    }
}

static void draw_input(const tui_state_t *state, const tui_layout_t *layout) {
    int cursor_y = layout->input_row, cursor_x = 0;
    const tui_editor_t *editor = NULL;
    const char *prompt = "Search: ";
    switch (state->field) {
        case TUI_FIELD_COMPOSE:
            editor = &state->composer;
            if (state->compose_reference.kind == TUI_COMPOSE_REFERENCE_REPLY)
                prompt = "Reply: ";
            else
                prompt = state->compose_delivery_method ==
                             LXMF_DELIVERY_METHOD_PROPAGATED ? "Relay: " : "Direct: ";
            break;
        case TUI_FIELD_REACTION:
            editor = &state->reaction;
            prompt = "React: ";
            break;
        case TUI_FIELD_SEARCH: editor = &state->search; prompt = "Search: "; break;
        case TUI_FIELD_NODE_SEARCH:
            editor = &state->node_search; prompt = "Find node: "; break;
        case TUI_FIELD_ADDRESS: editor = &state->address; prompt = "Address: "; break;
        case TUI_FIELD_SETTING:
        case TUI_FIELD_HOST:
        case TUI_FIELD_RRC:
        case TUI_FIELD_BROWSER_FORM:
            break;
        case TUI_FIELD_NONE: break;
    }
    (void)mvhline(layout->divider_row, 0, ACS_HLINE, layout->columns);
    bool composing = state->field == TUI_FIELD_COMPOSE;
    int text_x = composing ? 2 : TUI_PROMPT_WIDTH;
    if (composing) {
        (void)mvaddch(layout->divider_row, 0, ACS_ULCORNER);
        (void)mvaddch(layout->divider_row, layout->columns - 1, ACS_URCORNER);
        char label[80];
        (void)snprintf(label, sizeof label, " INSERT | %s %zu/%zu bytes ",
            prompt, editor->length, editor->capacity);
        (void)attron(A_BOLD);
        clipped(stdscr, layout->divider_row, 2, layout->columns - 4, label);
        (void)attroff(A_BOLD);
        for (int row = layout->input_row; row < layout->hint_row; ++row) {
            (void)mvaddch(row, 0, ACS_VLINE);
            (void)mvaddch(row, layout->columns - 1, ACS_VLINE);
        }
        (void)mvhline(layout->hint_row, 0, ACS_HLINE, layout->columns);
        (void)mvaddch(layout->hint_row, 0, ACS_LLCORNER);
        (void)mvaddch(layout->hint_row, layout->columns - 1, ACS_LRCORNER);
    }
    clipped(stdscr, layout->input_row, 0, layout->columns,
            composing ? "" : editor != NULL ? prompt : state->status);
    if (editor != NULL) {
        size_t width = composing ? (size_t)(layout->columns - 4)
                                 : (size_t)(layout->columns - TUI_PROMPT_WIDTH - 1);
        if (state->field == TUI_FIELD_COMPOSE) {
            size_t row, column, offset = 0u, start, bytes, line = 0u;
            tui_editor_position(editor, width, &row, &column);
            size_t first = row >= (size_t)layout->input_rows ? row - (size_t)layout->input_rows + 1u : 0u;
            char text[TUI_EDITOR_MAX + 1u];
            while (tui_text_wrap_next(editor->text, editor->length, width, &offset, &start, &bytes)) {
                if (line >= first && line < first + (size_t)layout->input_rows) {
                    memcpy(text, editor->text + start, bytes); text[bytes] = '\0';
                    clipped(stdscr, layout->input_row + (int)(line - first),
                            text_x, (int)width, text);
                }
                ++line;
            }
            cursor_y = layout->input_row + (int)(row - first);
            cursor_x = text_x + (int)column;
        } else {
            size_t offset = 0u, column = 0u;
            (void)tui_editor_view(editor, width, &offset, &column);
            clipped(stdscr, layout->input_row, TUI_PROMPT_WIDTH, (int)width,
                    tui_editor_text(editor) + offset);
            cursor_x = TUI_PROMPT_WIDTH + (int)column;
        }
    }
    char reference_hint[TUI_FIELD_PREVIEW_MAX + 80u];
    const char *hint = state->field == TUI_FIELD_COMPOSE
                           ? "Enter send  Ctrl-N newline  Up/Down move  Esc leave"
                           : editor != NULL ? "Enter accept  Esc leave  Home/End  Ctrl-U/K/W"
                           : "e reply  z react  d route  v save  / search  i info  ? help";
    if (editor != NULL && state->compose_reference.kind !=
                              TUI_COMPOSE_REFERENCE_NONE) {
        tui_render_compose_reference(&state->compose_reference,
                                     reference_hint,
                                     sizeof reference_hint);
        hint = reference_hint;
    }
    clipped(stdscr, layout->hint_row, composing ? 2 : 0,
            composing ? layout->columns - 4 : layout->columns, hint);
    clipped(stdscr, layout->legend_row, 0, layout->columns,
            editor != NULL ? state->status
                : " NORMAL | : commands  Enter compose  PgUp/PgDn history");
    if (editor != NULL) (void)move(cursor_y, cursor_x);
}

static void draw_conversation_overlay(const tui_state_t *state) {
    if (state->overlay == TUI_OVERLAY_HELP) {
        static const char *const help[] = {
            "j/k or arrows: conversation    PgUp/PgDn: history",
            "1/2/3: trusted/unknown/untrusted    /: search",
            "Enter: compose    i: peer info    p: pin    x: block",
            "PgUp/PgDn then e: reply to newest visible message",
            "PgUp/PgDn then z: react without changing the saved draft",
            "t/u: trust/untrust    n: local note    y: copy fallback",
            "i then q: peer address QR    U: my public address QR",
            "v: explicitly save newest attachment (RETICULUM_ATTACHMENT_DIR)",
            "d: choose direct or propagated delivery for queued messages",
            "Composer: arrows Home End Del Backspace Ctrl-A/E/U/K/W",
            "Propagated means accepted by the relay, not delivered to the recipient.",
            "Press ? or Esc to close."
        };
        centered_box("Help", help, sizeof help / sizeof help[0]);
        return;
    }
    const tui_contact_t *contact = tui_state_selected_contact(state);
    if (state->overlay == TUI_OVERLAY_LOCAL_QR) { draw_local_qr(state); return; }
    if ((state->overlay != TUI_OVERLAY_PEER && state->overlay != TUI_OVERLAY_PEER_QR) || contact == NULL) return;
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
    if (state->overlay == TUI_OVERLAY_PEER_QR && draw_address_qr(peer, "Peer LXMF address")) return;
    const char *lines[] = {peer, state->overlay == TUI_OVERLAY_PEER_QR ?
                           "QR needs a UTF-8 terminal at least 40x23." : "q: show scannable address QR",
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
    if (node->propagation)
        return node->lxmf_pn_app_data_valid && node->lxmf_pn_enabled
            ? "LXMF propagation relay"
            : "LXMF propagation endpoint (inactive/static-only)";
    switch (node->kind) {
        case RNS_NODE_KIND_NOMAD: return "Nomad node";
        case RNS_NODE_KIND_LXMF: return "LXMF inbox";
        case RNS_NODE_KIND_OTHER: break;
    }
    return "transport";
}

void tui_render_node_roles(const rns_node_record *node, char output[4]) {
    if (output == NULL) return;
    output[0] = node != NULL && node->has_message_destination ? 'P' : '-';
    output[1] = node != NULL && node->propagation
        ? (node->lxmf_pn_app_data_valid && node->lxmf_pn_enabled ? 'R' : 'r')
        : '-';
    output[2] = node != NULL && node->kind == RNS_NODE_KIND_NOMAD ? 'S' : '-';
    output[3] = '\0';
}

const char *tui_render_node_propagation_reason(const rns_node_record *node) {
    if (node == NULL || !node->propagation || !node->lxmf_pn_app_data_valid)
        return "not a propagation announce";
    if (!node->reachable) return "stale or unreachable";
    if (!node->lxmf_pn_enabled) return "propagation is disabled";
    if (node->lxmf_pn_stamp_cost == 0u ||
        node->lxmf_pn_stamp_cost == UINT8_MAX)
        return "invalid propagation cost";
    return NULL;
}

/* Renders the node details and the actions that apply to this node. */
static void draw_node_popup(const tui_state_t *state) {
    rns_node_record node;
    char address[TUI_ADDRESS_DIGITS + 1u];
    char inbox[TUI_ADDRESS_DIGITS + 1u];
    char roles[4];
    char lines[16][96];
    const char *pointers[16];
    size_t count = 0u;
    if (!tui_state_selected_node(state, &node)) return;
    tui_hex_format(node.destination, LXMF_DESTINATION_LENGTH, address);
    bool pages = tui_state_node_serves_pages(&node);
    bool messageable = node.has_message_destination;
    tui_render_node_roles(&node, roles);

    (void)snprintf(lines[count++], sizeof lines[0], "Address:  %s", address);
    (void)snprintf(lines[count++], sizeof lines[0], "Name:     %s",
                   node.name[0] != '\0' ? node.name : "(none announced)");
    (void)snprintf(lines[count++], sizeof lines[0],
                   "Roles:    %s  (P peer, R relay, S site; r inactive relay)",
                   roles);
    (void)snprintf(lines[count++], sizeof lines[0], "Type:     %s%s",
                   node_kind_name(&node),
                   pages ? " - serves pages" : " - serves no pages");
    (void)snprintf(lines[count++], sizeof lines[0],
                   "Route:    %u hops on interface %llu, %s%s",
                   (unsigned)node.hops, (unsigned long long)node.interface_id,
                   node.reachable ? "reachable" : "stale",
                   node.propagation
                       ? node.lxmf_pn_enabled
                           ? ", active propagation relay"
                           : ", inactive propagation endpoint"
                       : "");
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
    const char *propagation_reason =
        tui_render_node_propagation_reason(&node);
    bool propagation_ready = propagation_reason == NULL;
    (void)snprintf(lines[count++], sizeof lines[0],
                   "p  Use as propagation node");
    (void)snprintf(lines[count++], sizeof lines[0], "%s",
                   propagation_ready ? "   available" : "   unavailable");
    if (!propagation_ready)
        (void)snprintf(lines[count++], sizeof lines[0], "   %s",
                       propagation_reason);
    bool selected_propagation = state->settings.has_propagation_node &&
        memcmp(state->settings.propagation_node, node.destination,
               LXMF_DESTINATION_LENGTH) == 0;
    (void)snprintf(lines[count++], sizeof lines[0], "%s",
        state->propagation_sync.active
            ? "s  Cancel active propagation sync"
            : selected_propagation && propagation_ready
                ? "s  Sync messages now"
                : "s  Sync   (select this fresh propagation node first)");
    (void)snprintf(lines[count++], sizeof lines[0], "r  Refresh path");
    (void)snprintf(lines[count++], sizeof lines[0], "Esc  Close");
    for (size_t i = 0u; i < count; ++i) pointers[i] = lines[i];
    centered_box("Node details", pointers, count);
}

static void draw_network(const tui_state_t *state, const tui_layout_t *layout) {
    char heading[96];
    size_t capacity = tui_state_node_count(state);
    rns_node_record *sorted = capacity == 0U ? NULL
        : malloc(capacity * sizeof *sorted);
    size_t count = capacity != 0U && sorted == NULL ? 0U
        : tui_state_node_list(state, sorted, capacity);
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
                capacity != 0U
                    ? "Node list could not be allocated; known nodes remain stored."
                    : "No live announces received yet. Check interface status and wait for announces.");
    for (size_t i = first; i < count && i - first < rows; ++i) {
        char address[TUI_ADDRESS_DIGITS + 1u];
        char roles[4];
        char line[160];
        bool selected = state->has_node_selection && i == position;
        tui_hex_format(sorted[i].destination, LXMF_DESTINATION_LENGTH, address);
        tui_render_node_roles(&sorted[i], roles);
        (void)snprintf(line, sizeof line, "%s %s%s%s  %u hops  if:%llu  %s",
                       roles,
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
            "P peer  R relay  S site  r inactive relay | j/k select  Enter actions");
    free(sorted);
}

static void draw_setting_row(const tui_state_t *state, const tui_layout_t *layout,
                             int row, tui_setting_item_t item, const char *text) {
    bool selected = state->setting_selected == item;
    if (selected) (void)attron(A_REVERSE);
    clipped(stdscr, row, 2, layout->columns - 4, text);
    if (selected) (void)attroff(A_REVERSE);
}

static void draw_host(const tui_state_t *state, const tui_layout_t *layout) {
    char items[TUI_HOST_COUNT][1100];
    (void)snprintf(items[TUI_HOST_ROOT], sizeof items[0], "Page root: %s", state->settings.host_pages_root);
    (void)snprintf(items[TUI_HOST_PAGES], sizeof items[0], "Pages (semicolon-separated): %s", state->settings.host_pages);
    (void)snprintf(items[TUI_HOST_ACCESS], sizeof items[0], "Access: %s", state->settings.host_identified_only ? "identified links" : "anonymous links allowed");
    (void)snprintf(items[TUI_HOST_TOGGLE], sizeof items[0], "%s hosting", state->host.node != NULL ? "Stop" : "Start");
    (void)snprintf(items[TUI_HOST_ANNOUNCE], sizeof items[0], "Announce now (%zu sent; %s)", state->host.announces, rns_status_string(state->host.error));
    char host_address[33];
    if (state->host.node != NULL)
        tui_hex_format(rns_hosted_node_destination(state->host.node), 16u, host_address);
    clipped(stdscr, 2, 1, layout->columns - 2, state->host.node != NULL ? host_address : "Node: stopped (no service advertised)");
    int capacity = layout->divider_row - 4;
    int first = (int)state->host_selected >= capacity ? (int)state->host_selected - capacity + 1 : 0;
    for (int i = first, row = 4; i < (int)TUI_HOST_COUNT && row < layout->divider_row; ++i, ++row) {
        if (i == (int)state->host_selected) (void)attron(A_REVERSE);
        clipped(stdscr, row, 1, layout->columns - 2, items[i]);
        if (i == (int)state->host_selected) (void)attroff(A_REVERSE);
    }
    clipped(stdscr, 3, 1, layout->columns - 2, "Executables, files and propagation hosting disabled");
    (void)mvhline(layout->divider_row, 0, ACS_HLINE, layout->columns);
    size_t offset = 0u, cursor = 0u;
    if (state->field == TUI_FIELD_HOST) {
        (void)tui_editor_view(&state->host_editor, (size_t)layout->columns, &offset, &cursor);
        clipped(stdscr, layout->input_row, 0, layout->columns, tui_editor_text(&state->host_editor) + offset);
    } else clipped(stdscr, layout->input_row, 0, layout->columns, state->status);
    clipped(stdscr, layout->hint_row, 0, layout->columns, state->field == TUI_FIELD_HOST ? "Enter save  Esc cancel" : "j/k select  Enter edit/action  C conversations");
    if (state->field == TUI_FIELD_HOST) (void)move(layout->input_row, (int)cursor);
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
        rns_node_record selected_node;
        tui_propagation_state_t route =
            tui_state_propagation_state(state, &selected_node, &cost);
        const char *state_text = route == TUI_PROPAGATION_READY
            ? (selected_node.reachable ? "upload ready" : "cached route; connection unconfirmed")
            : route == TUI_PROPAGATION_STALE ? "stale; waiting for fresh announce"
            : route == TUI_PROPAGATION_DISABLED ? "announce says disabled"
            : route == TUI_PROPAGATION_INVALID_COST ? "invalid advertised cost"
            : "waiting for verified announce";
        if (route == TUI_PROPAGATION_READY)
            (void)snprintf(items[TUI_SETTING_PROPAGATION_NODE], sizeof items[0],
                           "Propagation node: %s (%s, cost %u; sync ready)",
                           propagation, state_text, (unsigned)cost);
        else
            (void)snprintf(items[TUI_SETTING_PROPAGATION_NODE], sizeof items[0],
                           "Propagation node: %s (%s)",
                           propagation, state_text);
    } else {
        (void)snprintf(items[TUI_SETTING_PROPAGATION_NODE], sizeof items[0], "%s",
                       "Propagation node: none (select one in Network)");
    }
    if (state->propagation_sync.active) {
        (void)snprintf(items[TUI_SETTING_PROPAGATION_SYNC], sizeof items[0],
            "Cancel Sync: %s, %zu/%zu messages",
            propagation_sync_phase_name(state->propagation_sync.state),
            state->propagation_sync.received,
            state->propagation_sync.available);
    } else if (state->propagation_sync.state == LXMF_PN_COMPLETE) {
        (void)snprintf(items[TUI_SETTING_PROPAGATION_SYNC], sizeof items[0],
            "Sync Now (last: %zu accepted, %zu duplicate, %zu rejected)",
            state->propagation_sync.accepted, state->propagation_sync.duplicates,
            state->propagation_sync.rejected);
    } else if (state->propagation_sync.state == LXMF_PN_FAILED ||
               state->propagation_sync.state == LXMF_PN_CANCELLED) {
        (void)snprintf(items[TUI_SETTING_PROPAGATION_SYNC], sizeof items[0],
            "Sync Now (last: %s)",
            propagation_sync_phase_name(state->propagation_sync.state));
    } else {
        (void)snprintf(items[TUI_SETTING_PROPAGATION_SYNC], sizeof items[0], "%s",
                       "Sync Now");
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
                : "j/k select  Enter edit/toggle/announce/sync  C conversations  q quit");
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
static const char *span_display(const tui_state_t *state,
                                const rns_micron_span *span,
                                size_t span_index, char *buffer,
                                size_t capacity) {
    const rns_micron_page *page = &state->page;
    const char *text = rns_micron_span_text(page, span);
    const rns_micron_form_control *control =
        rns_micron_form_control_for_span(&state->form, span_index);
    switch (span->kind) {
        case RNS_MICRON_SPAN_FIELD: {
            unsigned width = span->width != 0u ? span->width : 1u;
            if (width > 64u) width = 64u;
            const char *value = control != NULL ? control->value : text;
            if (span->masked) {
                size_t hidden = control != NULL ? control->value_length : strlen(value);
                if (hidden > width) hidden = width;
                (void)snprintf(buffer, capacity, "[%.*s%*s]", (int)hidden,
                               "****************************************************************",
                               (int)(width - hidden), "");
            } else
                (void)snprintf(buffer, capacity, "[%-*.*s]", (int)width,
                               (int)width, value);
            return buffer;
        }
        case RNS_MICRON_SPAN_CHECKBOX:
            (void)snprintf(buffer, capacity, "[%c] %s",
                           control != NULL && control->checked ? 'x' : ' ',
                           text);
            return buffer;
        case RNS_MICRON_SPAN_RADIO:
            (void)snprintf(buffer, capacity, "(%c) %s",
                           control != NULL && control->checked ? '*' : ' ',
                           text);
            return buffer;
        case RNS_MICRON_SPAN_TEXT:
        case RNS_MICRON_SPAN_LINK:
        default:
            return text;
    }
}

static int line_columns(const tui_state_t *state,
                        const rns_micron_line *line) {
    const rns_micron_page *page = &state->page;
    char buffer[RNS_MICRON_TEXT_MAX + 8u];
    size_t columns = 0u;
    for (uint16_t i = 0u; i < line->span_count; ++i) {
        const rns_micron_span *span = &page->spans[line->first_span + i];
        size_t span_index = (size_t)line->first_span + i;
        const char *text = span_display(state, span, span_index, buffer,
                                        sizeof buffer);
        columns += tui_utf8_columns(text, strlen(text));
    }
    return columns > INT_MAX ? INT_MAX : (int)columns;
}

static bool interactive_span(const rns_micron_span *span) {
    return span->kind == RNS_MICRON_SPAN_LINK ||
           span->kind == RNS_MICRON_SPAN_FIELD ||
           span->kind == RNS_MICRON_SPAN_CHECKBOX ||
           span->kind == RNS_MICRON_SPAN_RADIO;
}

/* Index of the page line carrying the nth interactive item. */
static size_t interactive_line(const rns_micron_page *page, size_t nth) {
    size_t seen = 0u;
    for (size_t i = 0u; i < page->line_count; ++i) {
        const rns_micron_line *line = &page->lines[i];
        for (uint16_t j = 0u; j < line->span_count; ++j) {
            if (!interactive_span(&page->spans[line->first_span + j])) continue;
            if (seen++ == nth) return i;
        }
    }
    return page->line_count;
}

static void draw_micron_line(const tui_state_t *state, const rns_micron_line *line,
                             int y, int left, int width,
                             size_t *interactive_ordinal) {
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
    int used = line_columns(state, line);
    int x = left + indent;
    if (line->align == RNS_MICRON_ALIGN_CENTER && used < available)
        x += (available - used) / 2;
    else if (line->align == RNS_MICRON_ALIGN_RIGHT && used < available)
        x += available - used;
    int remaining = left + width - x;
    for (uint16_t i = 0u; i < line->span_count && remaining > 0; ++i) {
        const rns_micron_span *span = &page->spans[line->first_span + i];
        size_t span_index = (size_t)line->first_span + i;
        const char *text = span_display(state, span, span_index, buffer,
                                        sizeof buffer);
        chtype attrs = micron_attrs(&span->style);
        if (line->heading != 0u) attrs |= A_BOLD | A_REVERSE;
        if (interactive_span(span)) {
            bool selected = *interactive_ordinal == state->link_selected;
            attrs |= selected ? (A_REVERSE | A_BOLD) :
                     span->kind == RNS_MICRON_SPAN_LINK ? A_UNDERLINE : A_NORMAL;
            ++*interactive_ordinal;
        }
        int drawn = put_text(y, x, remaining, text, attrs);
        x += drawn;
        remaining -= drawn;
    }
}

static void draw_browser(tui_state_t *state, const tui_layout_t *layout) {
    char title[RNS_MICRON_TEXT_MAX + 32u];
    (void)snprintf(title, sizeof title, "Browser [%s] %s",
                   state->browser_identified ? "identified" : "anonymous", state->url);
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
    bool retained = strcmp(state->url, state->page_url) != 0;
    if (retained) {
        clipped(stdscr, 5, 2, layout->columns - 4,
                "Showing the previously loaded page:");
        clipped(stdscr, 6, 2, layout->columns - 4, state->page_url);
    }
    int body_top = (failed || retained) ? 7 : 5;
    int body_rows = layout->hint_row - body_top - 1;
    if (body_rows < 1) return;

    const rns_micron_page *page = &state->page;
    /* Keep the selected link on screen as the selection walks the document. */
    size_t focus = interactive_line(page, state->link_selected);
    if (focus < page->line_count) {
        if (focus < state->page_scroll) state->page_scroll = focus;
        else if (focus >= state->page_scroll + (size_t)body_rows)
            state->page_scroll = focus - (size_t)body_rows + 1u;
    }
    if (state->page_scroll >= page->line_count)
        state->page_scroll = page->line_count != 0u ? page->line_count - 1u : 0u;

    size_t interactive_ordinal = 0u;
    for (size_t i = 0u; i < page->line_count; ++i) {
        if (i < state->page_scroll) {
            const rns_micron_line *skipped = &page->lines[i];
            for (uint16_t j = 0u; j < skipped->span_count; ++j)
                if (interactive_span(&page->spans[skipped->first_span + j]))
                    ++interactive_ordinal;
            continue;
        }
        int y = body_top + (int)(i - state->page_scroll);
        if (y >= layout->hint_row - 1) break;
        draw_micron_line(state, &page->lines[i], y, 2, layout->columns - 4,
                         &interactive_ordinal);
    }
    if (page->truncated || page->unsupported)
        clipped(stdscr, layout->hint_row - 1, 2, layout->columns - 4,
                page->truncated ? "Page was truncated to fit the parser bounds"
                                : "Tables and partials are shown unformatted");
    if (state->field == TUI_FIELD_BROWSER_FORM) {
        char edit[RNS_MICRON_FORM_VALUE_MAX + 40u];
        const rns_micron_form_control *control =
            rns_micron_form_control_at(&state->form,
                                       state->browser_edit_control);
        const rns_micron_span *span = control != NULL &&
                                      control->span_index < page->span_count
                                          ? &page->spans[control->span_index] : NULL;
        const char *name = span != NULL
                               ? rns_micron_span_target(page, span) : "field";
        (void)snprintf(edit, sizeof edit, "Edit %s: %s", name,
                       span != NULL && span->masked ? "(hidden)"
                                                    : tui_editor_text(&state->browser_editor));
        clipped(stdscr, layout->hint_row - 1, 2, layout->columns - 4, edit);
        clipped(stdscr, layout->hint_row, 0, layout->columns,
                "Enter apply  Esc cancel  arrows edit");
    } else
        clipped(stdscr, layout->hint_row, 0, layout->columns,
                "j/k control  Enter open  Backspace back  r reload  i identity  N network");
}

static void draw_guide(const tui_layout_t *layout) {
    static const char *const lines[] = {
        "Nomad Chat Guide",
        "C Conversations   N Network   B Browser   R RRC   S Settings   I Interfaces",
        "F validated Reticulum configuration",
        "j/k or arrows select; Enter activates; Esc returns to Conversations",
        "",
        "Conversations: 1/2/3 trust tabs, / search, a address, Enter compose",
        "Contact actions: i details, p pin, x block, t trust, u untrust",
        "Delivery: d selects direct or propagation-node upload for queued messages",
        "Network: / search, Enter details; b browse, m message, p relay, s sync/cancel",
        "Browser: j/k select controls, Enter edit/toggle/follow, Backspace back, r reload",
        "Settings: j/k select, Enter edit, announce, start or cancel propagation sync",
        "",
        "Delivery is proof-backed. A queued message is not shown as delivered.",
        "Feature availability is listed in docs/FEATURE_STATUS.md."
    };
    for (size_t i = 0u; i < sizeof lines / sizeof lines[0]; ++i) {
        int row = 3 + (int)i;
        if (row >= layout->hint_row) break;
        clipped(stdscr, row, 2, layout->columns - 4, lines[i]);
    }
    clipped(stdscr, layout->hint_row, 0, layout->columns,
            "C or Esc conversations  N network  B browser  S settings  q quit");
}

static void draw_logs(const tui_state_t *state, const tui_layout_t *layout) {
    (void)attron(A_BOLD);
    clipped(stdscr, 3, 1, layout->columns - 2, "Event Log");
    (void)attroff(A_BOLD);
    size_t count = tui_state_log_count(state);
    if (count == 0u) {
        clipped(stdscr, 5, 2, layout->columns - 4, "No events recorded.");
    } else {
        size_t selected = tui_state_log_position(state);
        size_t rows = layout->hint_row > 5 ? (size_t)(layout->hint_row - 5) : 1u;
        size_t first = selected >= rows ? selected - rows + 1u : 0u;
        for (size_t i = first; i < count && i - first < rows; ++i) {
            const tui_log_entry_t *entry = tui_state_log_entry(state, i);
            if (entry == NULL) continue;
            bool active = i == selected;
            if (active) (void)attron(A_REVERSE);
            char line[TUI_LOG_TEXT_MAX + 32u];
            (void)snprintf(line, sizeof line, "%6llu  %s",
                           (unsigned long long)entry->sequence, entry->text);
            clipped(stdscr, 5 + (int)(i - first), 2,
                    layout->columns - 4, line);
            if (active) (void)attroff(A_REVERSE);
        }
    }
    clipped(stdscr, layout->hint_row, 0, layout->columns,
            "j/k select  x clear  C or Esc conversations  q quit");
}

static void render_base(tui_state_t *state) {
    int rows, columns;
    if (state == NULL) return;
    getmaxyx(stdscr, rows, columns);
    (void)erase();
    tui_layout_t layout = layout_of(rows, columns);
    if (rows < TUI_MIN_ROWS || columns < TUI_MIN_COLUMNS) {
        draw_too_small(&layout);
        return;
    }
    if (state->screen == TUI_SCREEN_CONVERSATIONS && state->field == TUI_FIELD_COMPOSE) {
        size_t row, column;
        state->composer_columns = (size_t)(columns - 4);
        tui_editor_position(&state->composer, state->composer_columns, &row, &column);
        size_t offset = 0u, start, bytes, count = 0u;
        while (tui_text_wrap_next(state->composer.text, state->composer.length,
                state->composer_columns, &offset, &start, &bytes)) ++count;
        if (count <= row) count = row + 1u;
        size_t maximum = rows >= 16 ? 4u : 2u;
        layout.input_rows = (int)(count < maximum ? count : maximum);
        layout.input_row -= layout.input_rows - 1;
        layout.divider_row -= layout.input_rows - 1;
        layout.content_rows -= layout.input_rows - 1;
    }
    draw_chrome(state, &layout);
    if (state->screen == TUI_SCREEN_NODE) {
        draw_host(state, &layout);
        return;
    }
    if (state->screen == TUI_SCREEN_NETWORK) {
        draw_network(state, &layout);
        if (state->overlay == TUI_OVERLAY_NODE_ACTIONS) draw_node_popup(state);
        return;
    }
    if (state->screen == TUI_SCREEN_SETTINGS) {
        draw_settings(state, &layout);
        if (state->overlay == TUI_OVERLAY_LOCAL_QR) draw_local_qr(state);
        return;
    }
    if (state->screen == TUI_SCREEN_BROWSER) {
        draw_browser(state, &layout);
        if (state->overlay == TUI_OVERLAY_BROWSER_IDENTITY) {
            const char *lines[] = {
                state->browser_identified ? "Stop identifying and create a fresh anonymous link?" :
                    "Reveal your public identity to this node and reload?",
                state->url,
                "Applies only to this node, for this session.",
                "Other nodes remain anonymous. No private keys are sent.",
                "Enter confirm   Esc cancel"};
            centered_box("Browser identity", lines, sizeof lines / sizeof lines[0]);
        }
        return;
    }
    if (state->screen == TUI_SCREEN_GUIDE) {
        draw_guide(&layout);
        return;
    }
    if (state->screen == TUI_SCREEN_INTERFACES) {
        draw_interfaces(state, &layout);
        return;
    }
    if (state->screen == TUI_SCREEN_CONFIG) {
        draw_config(state, &layout);
        return;
    }
    if (state->screen == TUI_SCREEN_RRC) {
        draw_rrc(state, &layout);
        return;
    }
    if (state->screen == TUI_SCREEN_LOGS) {
        draw_logs(state, &layout);
        return;
    }
    if (!layout.narrow) draw_sidebar(state, &layout);
    draw_thread(state, &layout);
    draw_input(state, &layout);
    draw_conversation_overlay(state);
}

void tui_render_draw(tui_state_t *state) {
    if (state == NULL) return;
    render_base(state);
    if (state->command_active) {
        int rows, columns; getmaxyx(stdscr, rows, columns);
        if (rows >= TUI_MIN_ROWS && columns >= TUI_MIN_COLUMNS) {
            int top = rows - 7;
            (void)attron(A_REVERSE);
            for (int y = top; y < rows; ++y)
                (void)mvhline(y, 0, ' ', columns);
            (void)attron(A_BOLD);
            clipped(stdscr, top, 1, columns - 2, " COMMAND | Enter execute  Esc close");
            (void)attroff(A_BOLD);
            clipped(stdscr, top + 1, 1, columns - 2, ":");
            size_t offset, column;
            (void)tui_editor_view(&state->command, (size_t)(columns - 4), &offset, &column);
            clipped(stdscr, top + 1, 2, columns - 4, state->command.text + offset);
            size_t at = 0u, start, bytes;
            for (int row = 2; row <= 3 && tui_text_wrap_next(state->status,
                    strlen(state->status), (size_t)(columns - 2), &at, &start, &bytes); ++row) {
                char line[TUI_STATUS_MAX];
                memcpy(line, state->status + start, bytes); line[bytes] = '\0';
                clipped(stdscr, top + row, 1, columns - 2, line);
            }
            clipped(stdscr, top + 4, 1, columns - 2, "chats network browser settings");
            clipped(stdscr, top + 5, 1, columns - 2, "interfaces config logs guide node");
            clipped(stdscr, top + 6, 1, columns - 2, "rrc announce sync help q(uit)");
            (void)attroff(A_REVERSE);
            (void)move(top + 1, 2 + (int)column);
        }
    }
    (void)refresh();
}

int tui_render_dump(const tui_state_t *state, FILE *output) {
    char address[TUI_ADDRESS_DIGITS + 1u];
    if (state == NULL || output == NULL) return -1;
    tui_hex_format(state->local, LXMF_DESTINATION_LENGTH, address);
    (void)fprintf(output, "Nomad Chat\nIdentity: %s\n", address);
    if (state->screen == TUI_SCREEN_NODE) {
        if (state->host.node != NULL) {
            char host_address[33];
            tui_hex_format(rns_hosted_node_destination(state->host.node), 16u, host_address);
            (void)fprintf(output, "Hosted address: %s\n", host_address);
        }
        (void)fprintf(output, "Screen: Node\nHosting: %s\nStartup: %s\nPage root: %s\nPages: %s\nAccess: %s\nAnnounces: %zu\nExecutables, files and propagation hosting disabled\nStatus: %s\n",
            state->host.node != NULL ? "active" : "stopped", state->settings.host_enabled ? "enabled" : "disabled",
            state->settings.host_pages_root, state->settings.host_pages,
            state->settings.host_identified_only ? "identified" : "anonymous", state->host.announces, state->status);
        return ferror(output) ? -1 : 0;
    }
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
            rns_node_record selected_node;
            tui_propagation_state_t route =
                tui_state_propagation_state(state, &selected_node, &cost);
            const char *state_text = route == TUI_PROPAGATION_STALE
                ? "stale; waiting for fresh announce"
                : route == TUI_PROPAGATION_DISABLED
                    ? "announce says disabled"
                : route == TUI_PROPAGATION_INVALID_COST
                    ? "invalid advertised cost"
                    : "waiting for verified announce";
            if (route == TUI_PROPAGATION_READY)
                (void)fprintf(output,
                    "Propagation node: %s (%s, cost %u)\n",
                    propagation, selected_node.reachable
                        ? "upload and sync ready" : "cached route; connection unconfirmed",
                    (unsigned)cost);
            else
                (void)fprintf(output,
                    "Propagation node: %s (%s)\n",
                    propagation, state_text);
        } else {
            (void)fprintf(output, "Propagation node: none\n");
        }
        (void)fprintf(output,
            "Propagation sync: %s active=%s available=%zu received=%zu "
            "accepted=%zu duplicates=%zu rejected=%zu acknowledged=%zu "
            "result=%s transport=%s\n",
            propagation_sync_phase_name(state->propagation_sync.state),
            state->propagation_sync.active ? "yes" : "no",
            state->propagation_sync.available,
            state->propagation_sync.received,
            state->propagation_sync.accepted,
            state->propagation_sync.duplicates,
            state->propagation_sync.rejected,
            state->propagation_sync.acknowledged,
            lxmf_status_string(state->propagation_sync.result),
            rns_status_string(state->propagation_sync.transport_error));
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
            "Network: N, / search, Enter actions, b browse, m message, p relay, s sync\n"
            "Browser: B, j/k controls, Enter edit/toggle/follow, Backspace back, r reload\n"
            "Settings: S, j/k select, Enter edit/announce/sync/cancel\n"
            "RRC: R, j/k select, Enter edit/connect/join/part/send\n"
            "Escape: close active layer or return to Conversations\n"
            "Status: %s\n", state->status);
        return ferror(output) ? -1 : 0;
    }
    if (state->screen == TUI_SCREEN_BROWSER) {
        (void)fprintf(output, "Browser identity: %s\n",
                      state->browser_identified ? "identified" : "anonymous");
        if (state->overlay == TUI_OVERLAY_BROWSER_IDENTITY)
            (void)fprintf(output, "Confirm browser identity change: Enter confirm, Esc cancel\n");
        (void)fprintf(output, "Screen: Browser\nURL: %s\nPage URL: %s\nControls: %zu\n",
                      state->url, state->page_url, tui_state_browser_control_count(state));
        size_t ordinal = 0u;
        for (size_t i = 0u; i < state->page.span_count; ++i) {
            const rns_micron_span *span = &state->page.spans[i];
            if (!interactive_span(span)) continue;
            const char *kind = span->kind == RNS_MICRON_SPAN_LINK ? "link" :
                               span->kind == RNS_MICRON_SPAN_FIELD ? "field" :
                               span->kind == RNS_MICRON_SPAN_CHECKBOX
                                   ? "checkbox" : "radio";
            char safe[RNS_MICRON_TEXT_MAX * 2u];
            const char *label = span->kind == RNS_MICRON_SPAN_LINK
                                    ? rns_micron_span_text(&state->page, span)
                                    : rns_micron_span_target(&state->page, span);
            (void)tui_text_sanitize((const uint8_t *)label, strlen(label), safe,
                                    sizeof safe);
            (void)fprintf(output, "%c %s %s", ordinal == state->link_selected
                                                 ? '>' : ' ', kind, safe);
            const rns_micron_form_control *control =
                rns_micron_form_control_for_span(&state->form, i);
            if (control != NULL && span->kind != RNS_MICRON_SPAN_FIELD)
                (void)fprintf(output, " checked=%s",
                              control->checked ? "yes" : "no");
            (void)fputc('\n', output);
            ++ordinal;
        }
        (void)fprintf(output, "Status: %s\n", state->status);
        return ferror(output) ? -1 : 0;
    }
    if (state->screen == TUI_SCREEN_NETWORK) {
        size_t capacity = tui_state_node_count(state);
        rns_node_record *nodes = capacity == 0U ? NULL
            : malloc(capacity * sizeof *nodes);
        size_t count = capacity != 0U && nodes == NULL ? 0U
            : tui_state_node_list(state, nodes, capacity);
        (void)fprintf(output, "Screen: Network\nNodes: %zu\n", count);
        rns_node_record selected;
        if (state->overlay == TUI_OVERLAY_NODE_ACTIONS &&
            tui_state_selected_node(state, &selected)) {
            char roles[4];
            tui_render_node_roles(&selected, roles);
            (void)fprintf(output, "Roles: %s\n", roles);
            const char *reason =
                tui_render_node_propagation_reason(&selected);
            (void)fprintf(output, "Propagation action: %s",
                          reason == NULL ? "available" : "unavailable: ");
            if (reason != NULL) (void)fputs(reason, output);
            (void)fputc('\n', output);
        }
        (void)fprintf(output, "Status: %s\n", state->status);
        free(nodes);
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
                          i == state->interfaces.selected_index ? '>' : ' ',
                          info.name,
                          rns_config_interface_type_name(info.type),
                          tui_interfaces_state_name(info.state),
                          (unsigned long long)info.packets_received,
                          (unsigned long long)info.bytes_received,
                          (unsigned long long)info.packets_sent,
                          (unsigned long long)info.bytes_sent,
                          (unsigned long long)info.packets_dropped,
                          rns_status_string(info.last_error));
            (void)fprintf(output,
                          "  connections attempts=%llu established=%llu lost=%llu peers=%llu id=%llu\n",
                          (unsigned long long)info.connection_attempts,
                          (unsigned long long)info.connections_established,
                          (unsigned long long)info.connections_lost,
                          (unsigned long long)info.peers,
                          (unsigned long long)info.id);
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
    if (state->screen == TUI_SCREEN_RRC) {
        (void)fprintf(output,
            "Screen: RRC\nState: %s\nHub address: %s\nHub public identity: %s\nNick: %s\nRoom: %s\nAuto reconnect: %s\nDraft: %s\nMessages: %zu\n",
            rrc_state_name(state->rrc.info.state), state->rrc.hub_address,
            state->rrc.hub_identity, state->rrc.nick, state->rrc.room,
            state->rrc.auto_reconnect ? "on" : "off",
            state->rrc.outgoing[0] != '\0' ? "retained" : "empty",
            state->rrc.message_count);
        if (state->rrc.info.state == RNS_RRC_SESSION_CONNECTED)
            (void)fprintf(output,
                "Limits: message=%zu room=%zu rooms=%zu rate=%zu resource=%s\n",
                state->rrc.info.welcome.max_message_bytes,
                state->rrc.info.welcome.max_room_bytes,
                state->rrc.info.welcome.max_rooms,
                state->rrc.info.welcome.rate_per_minute,
                state->rrc.info.welcome.resource_envelopes ? "advertised" : "no");
        if (state->rrc.motd[0] != '\0')
            (void)fprintf(output, "MOTD: %s\n", state->rrc.motd);
        (void)fprintf(output, "Rooms: %zu\n", state->rrc.room_count);
        for (size_t i = 0u; i < state->rrc.room_count; ++i) {
            const tui_rrc_room_t *room = &state->rrc.rooms[i];
            const char *room_state = room->part_pending ? "parting"
                                     : room->join_pending ? "joining"
                                     : room->joined ? "joined" : "saved";
            (void)fprintf(output, "#%s %s members=%zu\n", room->name,
                          room_state, room->member_count);
        }
        for (size_t i = 0u; i < state->rrc.message_count; ++i)
            (void)fprintf(output, "#%s <%s> %s\n",
                state->rrc.messages[i].room[0] != '\0'
                    ? state->rrc.messages[i].room : "*",
                state->rrc.messages[i].nick[0] != '\0'
                    ? state->rrc.messages[i].nick : "unknown",
                state->rrc.messages[i].body);
        (void)fprintf(output,
            "Missing: persistent message history and Resource envelopes\nStatus: %s\n",
            state->rrc.status);
        return ferror(output) ? -1 : 0;
    }
    if (state->screen == TUI_SCREEN_LOGS) {
        (void)fprintf(output, "Screen: Logs\nEvents: %zu\n",
                      tui_state_log_count(state));
        size_t selected = tui_state_log_position(state);
        for (size_t i = 0u; i < tui_state_log_count(state); ++i) {
            const tui_log_entry_t *entry = tui_state_log_entry(state, i);
            if (entry != NULL)
                (void)fprintf(output, "%c %llu %s\n", i == selected ? '>' : ' ',
                              (unsigned long long)entry->sequence, entry->text);
        }
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
    if (state->compose_reference.kind != TUI_COMPOSE_REFERENCE_NONE) {
        char reference[TUI_FIELD_PREVIEW_MAX + 80u];
        tui_render_compose_reference(&state->compose_reference, reference,
                                     sizeof reference);
        (void)fprintf(output, "Compose reference: %s\n", reference);
    }
    (void)fprintf(output, "Messages: %zu\n", count);
    for (size_t i = 0u; i < count; ++i) {
        const tui_message_t *message = tui_state_thread_message(state, i);
        char metadata[448];
        bool outgoing = tui_state_outgoing(state, message);
        (void)fprintf(output, "%s %s ", outgoing ? ">" : "<",
                      outgoing ? delivery_marker(message->value.status) : "   ");
        tui_text_escape(output, message->value.content.data,
                        message->value.content.len);
        tui_render_message_metadata(&message->metadata, metadata, sizeof metadata);
        (void)fputs(metadata, output);
        (void)fputc('\n', output);
        for (size_t j = 0u; j < message->metadata.attachment_count; ++j)
            (void)fprintf(output, "  attachment %zu: %s (%zu bytes; save as %s)\n",
                          j + 1u,
                          message->metadata.attachments[j].display_name,
                          message->metadata.attachments[j].size,
                          message->metadata.attachments[j].safe_name);
    }
    (void)fprintf(output, "Status: %s\n", state->status);
    return ferror(output) ? -1 : 0;
}
