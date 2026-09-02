#include "tui.h"

#include "reticulum/destination.h"
#include "reticulum/identity.h"
#include "reticulum/lxmf.h"
#include "reticulum/lxmf_store.h"
#include "reticulum/micron.h"
#include "reticulum/node_registry.h"

#include <curses.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <locale.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define TUI_MAX_CONTACTS 64u
#define TUI_MAX_MESSAGES 256u
#define TUI_COMPOSER_MAX 1024u
#define TUI_SEARCH_MAX 80u

typedef enum { TRUST_TRUSTED, TRUST_UNKNOWN, TRUST_UNTRUSTED } trust_group;
typedef enum { SCREEN_CONVERSATIONS, SCREEN_NETWORK, SCREEN_BROWSER,
               SCREEN_NODE, SCREEN_CONFIG, SCREEN_GUIDE, SCREEN_LOGS,
               SCREEN_RRC, SCREEN_COUNT } tui_screen;
typedef enum { INPUT_NONE, INPUT_COMPOSE, INPUT_SEARCH } input_mode;

typedef struct {
    uint8_t peer[16];
    size_t messages;
    double latest;
    size_t unread;
    trust_group trust;
    bool pinned;
    bool blocked;
    char note[48];
} tui_contact;

typedef struct {
    lxmf_store_message_t value;
    uint8_t content[LXMF_STORE_MAX_CONTENT];
} tui_message;

typedef struct {
    rns_identity identity;
    uint8_t local[16];
    lxmf_store_t store;
    tui_contact contacts[TUI_MAX_CONTACTS];
    size_t contact_count;
    tui_message *messages;
    size_t message_count;
    size_t selected;
    size_t scroll;
    char composer[TUI_COMPOSER_MAX + 1u];
    size_t composer_length;
    size_t composer_cursor;
    input_mode input;
    char search[TUI_SEARCH_MAX + 1u];
    size_t search_length;
    size_t search_cursor;
    trust_group tab;
    tui_screen screen;
    size_t selected_message;
    bool help;
    bool peer_info;
    rns_node_registry nodes;
    rns_micron_page page;
    rns_micron_history browser_history;
    size_t browser_selected;
    char browser_url[RNS_MICRON_TEXT_MAX];
    char status[160];
} tui_state;

static void hex_format(const uint8_t *bytes, size_t length, char *out) {
    static const char digits[] = "0123456789abcdef";
    for (size_t i = 0; i < length; ++i) {
        out[i * 2u] = digits[bytes[i] >> 4u];
        out[i * 2u + 1u] = digits[bytes[i] & 15u];
    }
    out[length * 2u] = '\0';
}

static bool hex_parse_16(const char *text, uint8_t out[16]) {
    if (!text || strlen(text) != 32u) return false;
    for (size_t i = 0; i < 16u; ++i) {
        unsigned value;
        if (sscanf(text + i * 2u, "%2x", &value) != 1) return false;
        out[i] = (uint8_t)value;
    }
    return true;
}

static bool load_identity(const char *path, rns_identity *identity) {
    uint8_t bytes[64];
    FILE *file = fopen(path, "rb");
    if (!file) return false;
    size_t count = fread(bytes, 1u, sizeof bytes, file);
    int extra = fgetc(file);
    fclose(file);
    return count == sizeof bytes && extra == EOF &&
           rns_identity_from_private(identity, bytes);
}

static bool message_loader(void *context, const lxmf_store_message_t *message) {
    tui_state *state = context;
    if (state->message_count >= TUI_MAX_MESSAGES) return false;
    tui_message *copy = &state->messages[state->message_count++];
    copy->value = *message;
    memcpy(copy->content, message->content.data, message->content.len);
    copy->value.content.data = copy->content;
    const uint8_t *peer = memcmp(message->source, state->local, 16u) == 0
                              ? message->destination : message->source;
    size_t index = 0;
    while (index < state->contact_count &&
           memcmp(state->contacts[index].peer, peer, 16u) != 0) ++index;
    if (index == state->contact_count && index < TUI_MAX_CONTACTS) {
        memcpy(state->contacts[index].peer, peer, 16u);
        state->contacts[index].trust = TRUST_UNKNOWN;
        state->contact_count++;
    }
    if (index < TUI_MAX_CONTACTS) {
        state->contacts[index].messages++;
        if (message->timestamp > state->contacts[index].latest)
            state->contacts[index].latest = message->timestamp;
        if (memcmp(message->source, state->local, 16u) != 0)
            state->contacts[index].unread++;
    }
    return true;
}

static void add_contact(tui_state *state, const uint8_t peer[16]) {
    for (size_t i = 0; i < state->contact_count; ++i)
        if (memcmp(state->contacts[i].peer, peer, 16u) == 0) return;
    if (state->contact_count < TUI_MAX_CONTACTS) {
        memcpy(state->contacts[state->contact_count].peer, peer, 16u);
        state->contacts[state->contact_count].trust = TRUST_UNKNOWN;
        state->contact_count++;
    }
}

static int state_open(tui_state *state, const char *identity_path,
                      const char *store_path, const char *destination_hex) {
    memset(state, 0, sizeof *state);
    state->messages = calloc(TUI_MAX_MESSAGES, sizeof *state->messages);
    if (!state->messages) return -1;
    if (!load_identity(identity_path, &state->identity)) goto fail;
    const char *aspects[] = {"delivery"};
    if (!rns_destination_hash(&state->identity, "lxmf", aspects, 1u, state->local))
        goto fail;
    if (lxmf_store_open(&state->store, store_path) != LXMF_OK) goto fail;
    if (lxmf_store_list(&state->store, message_loader, state) != LXMF_OK) goto fail;
    if (destination_hex) {
        uint8_t peer[16];
        if (!hex_parse_16(destination_hex, peer)) goto fail;
        add_contact(state, peer);
    }
    snprintf(state->status, sizeof state->status,
             "Offline outbox - network delivery is not connected yet");
    state->tab = TRUST_UNKNOWN;
    state->screen = SCREEN_CONVERSATIONS;
    rns_node_registry_init(&state->nodes, 3600.0);
    rns_micron_history_init(&state->browser_history);
    snprintf(state->browser_url, sizeof state->browser_url, "nomad://local/home");
    static const uint8_t home[] =
        "# Nomad Browser\n"
        "Native Micron navigation is ready.\n"
        "[Network nodes](/network)\n"
        "[Guide](/guide)\n";
    if (!rns_micron_parse(&state->page, home, sizeof home - 1u) ||
        !rns_micron_history_push(&state->browser_history, state->browser_url))
        goto fail;
    return 0;
fail:
    lxmf_store_close(&state->store);
    free(state->messages);
    state->messages = NULL;
    return -1;
}

static void state_close(tui_state *state) {
    lxmf_store_close(&state->store);
    free(state->messages);
}

static const char *marker(lxmf_delivery_status_t status) {
    switch (status) {
        case LXMF_DELIVERY_QUEUED: return "[.]";
        case LXMF_DELIVERY_SENDING: return "[>]";
        case LXMF_DELIVERY_SENT: return "[+]";
        case LXMF_DELIVERY_DELIVERED: return "[x]";
        case LXMF_DELIVERY_FAILED: return "[!]";
    }
    return "[?]";
}

static const char *trust_name(trust_group trust) {
    switch (trust) {
        case TRUST_TRUSTED: return "TRUSTED";
        case TRUST_UNKNOWN: return "UNKNOWN";
        case TRUST_UNTRUSTED: return "UNTRUSTED";
    }
    return "UNKNOWN";
}

static bool text_contains(const uint8_t *haystack, size_t haystack_length,
                          const char *needle) {
    size_t n = strlen(needle);
    if (n == 0u) return true;
    if (n > haystack_length) return false;
    for (size_t i = 0; i + n <= haystack_length; ++i) {
        size_t j = 0;
        while (j < n && tolower((unsigned char)haystack[i + j]) ==
                            tolower((unsigned char)needle[j])) ++j;
        if (j == n) return true;
    }
    return false;
}

static bool contact_visible(const tui_state *state, size_t index) {
    if (index >= state->contact_count || state->contacts[index].trust != state->tab)
        return false;
    if (state->search_length == 0u) return true;
    char peer[33];
    hex_format(state->contacts[index].peer, 16u, peer);
    if (text_contains((const uint8_t *)peer, strlen(peer), state->search) ||
        text_contains((const uint8_t *)state->contacts[index].note,
                      strlen(state->contacts[index].note), state->search)) return true;
    for (size_t i = 0; i < state->message_count; ++i) {
        const lxmf_store_message_t *message = &state->messages[i].value;
        const uint8_t *message_peer = memcmp(message->source, state->local, 16u) == 0
                                          ? message->destination : message->source;
        if (memcmp(message_peer, state->contacts[index].peer, 16u) == 0 &&
            text_contains(message->content.data, message->content.len, state->search))
            return true;
    }
    return false;
}

static size_t visible_count(const tui_state *state) {
    size_t count = 0;
    for (size_t i = 0; i < state->contact_count; ++i)
        if (contact_visible(state, i)) ++count;
    return count;
}

static size_t visible_at(const tui_state *state, size_t target) {
    for (size_t i = 0, current = 0; i < state->contact_count; ++i) {
        if (!contact_visible(state, i)) continue;
        if (current++ == target) return i;
    }
    return state->contact_count;
}

static bool selected_message(const tui_state *state, const tui_message *message) {
    if (state->selected >= state->contact_count) return false;
    const uint8_t *peer = memcmp(message->value.source, state->local, 16u) == 0
                              ? message->value.destination : message->value.source;
    return memcmp(peer, state->contacts[state->selected].peer, 16u) == 0;
}

static bool message_visible(const tui_state *state, const tui_message *message) {
    return selected_message(state, message) &&
           text_contains(message->value.content.data, message->value.content.len,
                         state->search);
}

static size_t selected_count(const tui_state *state) {
    size_t count = 0;
    for (size_t i = 0; i < state->message_count; ++i)
        if (message_visible(state, &state->messages[i])) ++count;
    return count;
}

static const tui_message *selected_at(const tui_state *state, size_t target) {
    size_t current = 0;
    for (size_t i = 0; i < state->message_count; ++i) {
        if (!message_visible(state, &state->messages[i])) continue;
        if (current++ == target) return &state->messages[i];
    }
    return NULL;
}

static void clipped(WINDOW *window, int y, int x, int width, const char *text) {
    if (width > 0) mvwaddnstr(window, y, x, text, width);
}

static void centered_box(const char *title, const char *const *lines, size_t count) {
    int rows, columns;
    getmaxyx(stdscr, rows, columns);
    int width = columns > 70 ? 68 : columns - 4;
    int height = (int)count + 4;
    if (width < 20 || rows < height + 2) return;
    int top = (rows - height) / 2, left = (columns - width) / 2;
    WINDOW *popup = newwin(height, width, top, left);
    if (!popup) return;
    wbkgd(popup, A_REVERSE);
    box(popup, 0, 0);
    wattron(popup, A_BOLD);
    clipped(popup, 1, 2, width - 4, title);
    wattroff(popup, A_BOLD);
    for (size_t i = 0; i < count; ++i)
        clipped(popup, 2 + (int)i, 2, width - 4, lines[i]);
    wrefresh(popup);
    delwin(popup);
}

static void draw_network(tui_state *state, int rows, int columns) {
    attron(A_BOLD); clipped(stdscr, 3, 1, columns - 2, "Active and known nodes"); attroff(A_BOLD);
    if (state->nodes.count == 0u) {
        clipped(stdscr, 5, 2, columns - 4,
                "No live announces received. Runtime announce wiring is the next milestone.");
    }
    for (size_t i = 0; i < state->nodes.count && (int)i < rows - 8; ++i) {
        char address[33], line[160]; hex_format(state->nodes.records[i].destination, 16u, address);
        snprintf(line, sizeof line, "%s  %u hops  if:%llu  %s", address,
                 (unsigned)state->nodes.records[i].hops,
                 (unsigned long long)state->nodes.records[i].interface_id,
                 state->nodes.records[i].reachable ? "reachable" : "stale");
        clipped(stdscr, 5 + (int)i, 2, columns - 4, line);
    }
    clipped(stdscr, rows - 2, 0, columns, "B browser  C conversations  q quit");
}

static size_t browser_link_count(const tui_state *state) {
    size_t n=0; for(size_t i=0;i<state->page.count;i++) if(state->page.items[i].kind==RNS_MICRON_LINK)n++; return n;
}

static const rns_micron_item *browser_link_at(const tui_state *state,size_t target){size_t n=0;for(size_t i=0;i<state->page.count;i++)if(state->page.items[i].kind==RNS_MICRON_LINK&&n++==target)return &state->page.items[i];return NULL;}

static void draw_browser(tui_state *state, int rows, int columns) {
    char title[600]; snprintf(title,sizeof title,"Browser  %s",state->browser_url);
    attron(A_BOLD); clipped(stdscr,3,1,columns-2,title); attroff(A_BOLD);
    size_t link=0;
    for(size_t i=0;i<state->page.count&&(int)i<rows-8;i++){
        const rns_micron_item *item=&state->page.items[i]; char line[1100];
        if(item->kind==RNS_MICRON_LINK){snprintf(line,sizeof line,"[%c] %s -> %s",link==state->browser_selected?'>':' ',item->text,item->target);link++;}
        else if(item->kind==RNS_MICRON_MEDIA)snprintf(line,sizeof line,"[media] %s",item->target);
        else snprintf(line,sizeof line,"%s",item->text);
        if(item->kind==RNS_MICRON_HEADING)attron(A_BOLD);clipped(stdscr,5+(int)i,2,columns-4,line);if(item->kind==RNS_MICRON_HEADING)attroff(A_BOLD);
    }
    clipped(stdscr,rows-2,0,columns,"j/k select  Enter open  Backspace back  N network  C conversations");
}

static void draw(tui_state *state) {
    int rows, columns;
    getmaxyx(stdscr, rows, columns);
    erase();
    if (rows < 10 || columns < 38) {
        attron(A_BOLD);
        clipped(stdscr, 0, 0, columns, "Nomad Chat");
        attroff(A_BOLD);
        clipped(stdscr, 2, 0, columns, "Terminal too small (need 38x10)");
        clipped(stdscr, rows - 1, 0, columns, "q quit");
        refresh();
        return;
    }
    bool narrow = columns < 72;
    int sidebar = narrow ? 0 : 24;
    char address[33];
    hex_format(state->local, 16u, address);
    attron(A_REVERSE | A_BOLD);
    char header[160];
    snprintf(header, sizeof header, " Nomad Chat  %.32s  OFFLINE ", address);
    clipped(stdscr, 0, 0, columns, header);
    for (int x = (int)strlen(header); x < columns; ++x) addch(' ');
    attroff(A_REVERSE | A_BOLD);

    clipped(stdscr, 1, 0, columns,
            "[C]hats [N]etwork [B]rowser N[o]de [S]ettings [G]uide [L]ogs [R]RC");

    if (state->screen == SCREEN_NETWORK) { draw_network(state, rows, columns); refresh(); return; }
    if (state->screen == SCREEN_BROWSER) { draw_browser(state, rows, columns); refresh(); return; }

    if (!narrow) {
        attron(A_BOLD);
        char tabline[48];
        snprintf(tabline, sizeof tabline, "%s (%zu)", trust_name(state->tab),
                 visible_count(state));
        clipped(stdscr, 2, 1, sidebar - 2, tabline);
        attroff(A_BOLD);
        size_t shown = 0;
        for (size_t pos = 0; pos < visible_count(state) && (int)shown < rows - 7; ++pos) {
            size_t i = visible_at(state, pos);
            char peer[33];
            hex_format(state->contacts[i].peer, 16u, peer);
            if (i == state->selected) attron(A_REVERSE);
            char line[64];
            snprintf(line, sizeof line, "%c%-11.11s %c%2zu ",
                     state->contacts[i].pinned ? '*' : ' ', peer,
                     state->contacts[i].unread ? '!' : ' ',
                     state->contacts[i].unread ? state->contacts[i].unread
                                               : state->contacts[i].messages);
            clipped(stdscr, 3 + (int)shown++, 0, sidebar - 1, line);
            if (i == state->selected) attroff(A_REVERSE);
        }
        mvvline(1, sidebar - 1, ACS_VLINE, rows - 4);
    }

    int pane_x = sidebar + (narrow ? 0 : 1);
    int pane_width = columns - pane_x;
    char peer_line[80] = "No conversation selected";
    if (state->selected < state->contact_count) {
        char peer[33];
        hex_format(state->contacts[state->selected].peer, 16u, peer);
        snprintf(peer_line, sizeof peer_line, "%s%s", narrow ? "Chat: " : "Conversation: ", peer);
    }
    attron(A_BOLD);
    clipped(stdscr, 2, pane_x, pane_width, peer_line);
    attroff(A_BOLD);

    int message_rows = rows - 7;
    size_t count = selected_count(state);
    size_t first = count > (size_t)message_rows + state->scroll
                       ? count - (size_t)message_rows - state->scroll : 0u;
    for (int line = 0; line < message_rows; ++line) {
        const tui_message *message = selected_at(state, first + (size_t)line);
        if (!message || first + (size_t)line >= count - state->scroll) break;
        bool outgoing = memcmp(message->value.source, state->local, 16u) == 0;
        char text[LXMF_STORE_MAX_CONTENT + 1u];
        size_t length = message->value.content.len;
        memcpy(text, message->value.content.data, length);
        text[length] = '\0';
        for (size_t i = 0; i < length; ++i)
            if (text[i] == '\n' || text[i] == '\r' || text[i] == '\t') text[i] = ' ';
        char rendered[LXMF_STORE_MAX_CONTENT + 32u];
        snprintf(rendered, sizeof rendered, "%s %s %s", outgoing ? ">" : "<",
                 outgoing ? marker(message->value.status) : "   ", text);
        clipped(stdscr, 3 + line, pane_x, pane_width, rendered);
    }

    mvhline(rows - 4, 0, ACS_HLINE, columns);
    const bool entering = state->input != INPUT_NONE;
    const char *prompt = state->input == INPUT_COMPOSE ? "Message: " : "Search: ";
    clipped(stdscr, rows - 3, 0, columns, entering ? prompt : state->status);
    if (entering) {
        const char *value = state->input == INPUT_COMPOSE ? state->composer : state->search;
        size_t cursor = state->input == INPUT_COMPOSE ? state->composer_cursor : state->search_cursor;
        clipped(stdscr, rows - 3, 9, columns - 9, value);
        int cursor_x = 9 + (int)cursor;
        move(rows - 3, cursor_x < columns ? cursor_x : columns - 1);
    }
    clipped(stdscr, rows - 2, 0, columns,
            entering ? "Enter accept  Esc cancel  Home/End  Ctrl-A/E/U/K/W"
                     : "1/2/3 trust tabs  / search  i info  p pin  t/u trust  ? help");
    clipped(stdscr, rows - 1, 0, columns,
            "[.] queued  [>] sending  [+] sent  [x] delivered  [!] failed");
    refresh();
    if (state->help) {
        static const char *const help[] = {
            "j/k or arrows: conversation    PgUp/PgDn: history",
            "1/2/3: trusted/unknown/untrusted    /: search",
            "Enter: compose    i: peer info    p: pin    x: block",
            "t/u: trust/untrust    n: local note    y: copy fallback",
            "Composer: arrows Home End Del Backspace Ctrl-A/E/U/K/W",
            "Network actions remain OFFLINE. Press ? or Esc to close."
        };
        centered_box("Help", help, sizeof help / sizeof help[0]);
    } else if (state->peer_info && state->selected < state->contact_count) {
        char peer[33], trust[64], note[80], flags[80];
        hex_format(state->contacts[state->selected].peer, 16u, peer);
        snprintf(trust, sizeof trust, "Trust: %s", trust_name(state->contacts[state->selected].trust));
        snprintf(note, sizeof note, "Note: %s", state->contacts[state->selected].note[0] ? state->contacts[state->selected].note : "none (n toggles a local placeholder)");
        snprintf(flags, sizeof flags, "Pinned: %s  Blocked: %s  Unread: %zu",
                 state->contacts[state->selected].pinned ? "yes" : "no",
                 state->contacts[state->selected].blocked ? "yes" : "no",
                 state->contacts[state->selected].unread);
        const char *lines[] = {peer, "[ QR display unavailable in text-only build ]", trust, note, flags,
                               "Session metadata is not persisted yet. Esc/i closes."};
        centered_box("Peer information", lines, sizeof lines / sizeof lines[0]);
    }
}

static lxmf_status_t queue_message(tui_state *state) {
    if (state->selected >= state->contact_count || state->composer_length == 0u)
        return LXMF_ERR_ARGUMENT;
    lxmf_message_t source = {0}, decoded;
    memcpy(source.destination, state->contacts[state->selected].peer, 16u);
    memcpy(source.source, state->local, 16u);
    source.timestamp = (double)time(NULL);
    source.content.data = (const uint8_t *)state->composer;
    source.content.len = state->composer_length;
    uint8_t packed[LXMF_STORE_MAX_CONTENT + 256u];
    size_t packed_length = 0;
    lxmf_status_t status = lxmf_pack(&source, lxmf_identity_signer, &state->identity,
                                     packed, sizeof packed, &packed_length);
    if (status != LXMF_OK) return status;
    status = lxmf_unpack(packed, packed_length, NULL, NULL, &decoded);
    if (status != LXMF_OK) return status;
    lxmf_store_message_t stored = {0};
    memcpy(stored.message_id, decoded.message_id, 32u);
    memcpy(stored.destination, source.destination, 16u);
    memcpy(stored.source, source.source, 16u);
    stored.timestamp = source.timestamp;
    stored.status = LXMF_DELIVERY_QUEUED;
    stored.content = source.content;
    bool inserted = false;
    status = lxmf_store_put(&state->store, &stored, &inserted);
    if (status == LXMF_OK && inserted && state->message_count < TUI_MAX_MESSAGES) {
        tui_message *copy = &state->messages[state->message_count++];
        copy->value = stored;
        memcpy(copy->content, stored.content.data, stored.content.len);
        copy->value.content.data = copy->content;
        state->contacts[state->selected].messages++;
        state->contacts[state->selected].latest = stored.timestamp;
    }
    return status;
}

static void select_delta(tui_state *state, int delta) {
    size_t count = visible_count(state);
    if (count == 0u) return;
    size_t position = 0;
    while (position < count && visible_at(state, position) != state->selected) ++position;
    if (position == count) position = 0;
    else if (delta < 0) position = position == 0u ? count - 1u : position - 1u;
    else position = (position + 1u) % count;
    state->selected = visible_at(state, position);
    state->contacts[state->selected].unread = 0u;
    state->scroll = 0u;
}

static void set_tab(tui_state *state, trust_group tab) {
    state->tab = tab;
    state->scroll = 0u;
    size_t first = visible_at(state, 0u);
    if (first < state->contact_count) state->selected = first;
}

static void unavailable_screen(tui_state *state, tui_screen screen,
                               const char *name) {
    state->screen = screen;
    snprintf(state->status, sizeof state->status,
             "%s screen is not implemented; remaining in Conversations", name);
    state->screen = SCREEN_CONVERSATIONS;
}

static void browser_open_selected(tui_state *state){const rns_micron_item *item=browser_link_at(state,state->browser_selected);if(!item)return;char url[RNS_MICRON_TEXT_MAX];if(!rns_micron_normalize_url(state->browser_url,item->target,url,sizeof url)||!rns_micron_history_push(&state->browser_history,url)){snprintf(state->status,sizeof state->status,"Invalid or oversized link");return;}snprintf(state->browser_url,sizeof state->browser_url,"%s",url);char page[RNS_MICRON_TEXT_MAX+80];snprintf(page,sizeof page,"# %s\nPage request transport is not connected yet.\n[Back to home](/home)\n",url);(void)rns_micron_parse(&state->page,(const uint8_t*)page,strlen(page));state->browser_selected=0;}

static int run_loop(tui_state *state) {
    if (!setlocale(LC_ALL, "")) return -1;
    if (!initscr()) return -1;
    cbreak();
    noecho();
    keypad(stdscr, TRUE);
    curs_set(0);
    int result = 0;
    bool running = true;
    while (running) {
        draw(state);
        int key = getch();
        if (state->help || state->peer_info) {
            if (key == 27 || key == '?' || key == 'i') {
                state->help = false;
                state->peer_info = false;
            }
            continue;
        }
        if (state->input != INPUT_NONE) {
            char *buffer = state->input == INPUT_COMPOSE ? state->composer : state->search;
            size_t *length = state->input == INPUT_COMPOSE ? &state->composer_length : &state->search_length;
            size_t *cursor = state->input == INPUT_COMPOSE ? &state->composer_cursor : &state->search_cursor;
            size_t capacity = state->input == INPUT_COMPOSE ? TUI_COMPOSER_MAX : TUI_SEARCH_MAX;
            if (key == 27) {
                state->input = INPUT_NONE;
                curs_set(0);
            } else if (key == '\n' || key == KEY_ENTER) {
                if (state->input == INPUT_SEARCH) {
                    state->input = INPUT_NONE;
                    size_t first = visible_at(state, 0u);
                    if (first < state->contact_count) state->selected = first;
                    curs_set(0);
                } else {
                    lxmf_status_t status = queue_message(state);
                    if (status == LXMF_OK)
                        snprintf(state->status, sizeof state->status,
                                 "Queued locally; network delivery is offline");
                    else
                        snprintf(state->status, sizeof state->status,
                                 "Could not queue message (%d)", status);
                    if (status == LXMF_OK) {
                        state->composer_length = state->composer_cursor = 0u;
                        state->composer[0] = '\0';
                        state->input = INPUT_NONE;
                        curs_set(0);
                    }
                }
            } else if (key == KEY_BACKSPACE || key == 127 || key == 8) {
                if (*cursor > 0u) {
                    memmove(buffer + *cursor - 1u, buffer + *cursor,
                            *length - *cursor + 1u);
                    --*cursor;
                    --*length;
                }
            } else if (key == KEY_DC && *cursor < *length) {
                memmove(buffer + *cursor, buffer + *cursor + 1u, *length - *cursor);
                --*length;
            } else if ((key == KEY_LEFT || key == 2) && *cursor > 0u) {
                --*cursor;
            } else if ((key == KEY_RIGHT || key == 6) && *cursor < *length) {
                ++*cursor;
            } else if (key == KEY_HOME || key == 1) {
                *cursor = 0u;
            } else if (key == KEY_END || key == 5) {
                *cursor = *length;
            } else if (key == 21) {
                memmove(buffer, buffer + *cursor, *length - *cursor + 1u);
                *length -= *cursor;
                *cursor = 0u;
            } else if (key == 11) {
                buffer[*cursor] = '\0';
                *length = *cursor;
            } else if (key == 23) {
                size_t start = *cursor;
                while (start > 0u && isspace((unsigned char)buffer[start - 1u])) --start;
                while (start > 0u && !isspace((unsigned char)buffer[start - 1u])) --start;
                memmove(buffer + start, buffer + *cursor, *length - *cursor + 1u);
                *length -= *cursor - start;
                *cursor = start;
            } else if (key >= 32 && key <= 126 && *length < capacity) {
                memmove(buffer + *cursor + 1u, buffer + *cursor,
                        *length - *cursor + 1u);
                buffer[(*cursor)++] = (char)key;
                ++*length;
            }
            continue;
        }
        switch (key) {
            case 'q': case 'Q': case 27: running = false; break;
            case '?': state->help = true; break;
            case '1': set_tab(state, TRUST_TRUSTED); break;
            case '2': set_tab(state, TRUST_UNKNOWN); break;
            case '3': set_tab(state, TRUST_UNTRUSTED); break;
            case KEY_UP: case 'k':
                if(state->screen==SCREEN_BROWSER){if(state->browser_selected>0)state->browser_selected--;}else select_delta(state, -1); break;
            case KEY_DOWN: case 'j': case '\t':
                if(state->screen==SCREEN_BROWSER){size_t n=browser_link_count(state);if(n)state->browser_selected=(state->browser_selected+1u)%n;}else select_delta(state, 1); break;
            case KEY_PPAGE: state->scroll += 5u; break;
            case KEY_NPAGE: state->scroll = state->scroll > 5u ? state->scroll - 5u : 0u; break;
            case '/':
                state->input = INPUT_SEARCH;
                state->search_cursor = state->search_length;
                curs_set(1);
                break;
            case 'i': if (state->selected < state->contact_count) state->peer_info = true; break;
            case 'p':
                if (state->selected < state->contact_count) {
                    state->contacts[state->selected].pinned = !state->contacts[state->selected].pinned;
                    snprintf(state->status, sizeof state->status,
                             "Pin is session-only until contact storage is available");
                }
                break;
            case 'x':
                if (state->selected < state->contact_count) {
                    state->contacts[state->selected].blocked = !state->contacts[state->selected].blocked;
                    snprintf(state->status, sizeof state->status,
                             "Block is session-only; offline outbox has no receive filter");
                }
                break;
            case 't':
                if (state->selected < state->contact_count) {
                    state->contacts[state->selected].trust = TRUST_TRUSTED;
                    set_tab(state, TRUST_TRUSTED);
                    snprintf(state->status, sizeof state->status, "Trust change is session-only");
                }
                break;
            case 'u':
                if (state->selected < state->contact_count) {
                    state->contacts[state->selected].trust = TRUST_UNTRUSTED;
                    set_tab(state, TRUST_UNTRUSTED);
                    snprintf(state->status, sizeof state->status, "Trust change is session-only");
                }
                break;
            case 'n':
                if (state->selected < state->contact_count) {
                    tui_contact *contact = &state->contacts[state->selected];
                    snprintf(contact->note, sizeof contact->note, "%s",
                             contact->note[0] ? "" : "Local note (session only)");
                    snprintf(state->status, sizeof state->status,
                             "Contact note placeholder is session-only");
                }
                break;
            case 'y':
                snprintf(state->status, sizeof state->status,
                         "Clipboard unavailable; use history or --dump-ui to copy text");
                break;
            case 'c': case 'C': state->screen = SCREEN_CONVERSATIONS; break;
            case 'N': state->screen = SCREEN_NETWORK; break;
            case 'B': state->screen = SCREEN_BROWSER; break;
            case 'o': case 'O': unavailable_screen(state, SCREEN_NODE, "Node"); break;
            case 's': case 'S': unavailable_screen(state, SCREEN_CONFIG, "Settings"); break;
            case 'g': case 'G': unavailable_screen(state, SCREEN_GUIDE, "Guide"); break;
            case 'l': case 'L': unavailable_screen(state, SCREEN_LOGS, "Logs"); break;
            case 'r': case 'R': unavailable_screen(state, SCREEN_RRC, "RRC"); break;
            case '\n': case KEY_ENTER:
                if(state->screen==SCREEN_BROWSER) browser_open_selected(state);
                else if (state->contact_count > 0u) {
                    state->input = INPUT_COMPOSE;
                    curs_set(1);
                } else {
                    snprintf(state->status, sizeof state->status,
                             "No conversation: provide a destination address");
                }
                break;
            case KEY_BACKSPACE: case 127: case 8:
                if(state->screen==SCREEN_BROWSER){const char *url=rns_micron_history_back(&state->browser_history);if(url){snprintf(state->browser_url,sizeof state->browser_url,"%s",url);static const uint8_t home[]="# Nomad Browser\n[Network nodes](/network)\n[Guide](/guide)\n";(void)rns_micron_parse(&state->page,home,sizeof home-1u);state->browser_selected=0;}}
                break;
            case KEY_RESIZE: break;
            default: break;
        }
    }
    if (endwin() == ERR) result = -1;
    return result;
}

int nomad_tui_run_destination(const char *identity_path, const char *store_path,
                              const char *destination_hex) {
    tui_state state;
    if (!identity_path || !store_path ||
        state_open(&state, identity_path, store_path, destination_hex) != 0) return -1;
    int result = run_loop(&state);
    state_close(&state);
    return result;
}

int nomad_tui_run(const char *identity_path, const char *store_path) {
    return nomad_tui_run_destination(identity_path, store_path, NULL);
}

int nomad_tui_dump(const char *identity_path, const char *store_path,
                   const char *destination_hex, FILE *output) {
    if (!output) return -1;
    tui_state state;
    if (!identity_path || !store_path ||
        state_open(&state, identity_path, store_path, destination_hex) != 0) return -1;
    char local[33];
    hex_format(state.local, 16u, local);
    fprintf(output, "Nomad Chat\nIdentity: %s\n", local);
    if (state.selected < state.contact_count) {
        char peer[33];
        hex_format(state.contacts[state.selected].peer, 16u, peer);
        fprintf(output, "Conversation: %s\n", peer);
    } else {
        fprintf(output, "Conversation: none\n");
    }
    fprintf(output, "Network: OFFLINE (local outbox only)\n");
    fprintf(output, "Conversations: %zu\n", state.contact_count);
    for (size_t i = 0; i < state.contact_count; ++i) {
        char peer[33];
        hex_format(state.contacts[i].peer, 16u, peer);
        fprintf(output, "%c %s (%zu messages)\n", i == state.selected ? '>' : ' ',
                peer, state.contacts[i].messages);
    }
    size_t count = selected_count(&state);
    fprintf(output, "Messages: %zu\n", count);
    for (size_t i = 0; i < count; ++i) {
        const tui_message *message = selected_at(&state, i);
        bool outgoing = memcmp(message->value.source, state.local, 16u) == 0;
        fprintf(output, "%s %s ", outgoing ? ">" : "<",
                outgoing ? marker(message->value.status) : "   ");
        for (size_t j = 0; j < message->value.content.len; ++j) {
            unsigned char byte = message->value.content.data[j];
            if (byte == '\n') fputs("\\n", output);
            else if (byte == '\r') fputs("\\r", output);
            else if (byte == '\t') fputs("\\t", output);
            else if (byte == '\\') fputs("\\\\", output);
            else if (byte >= 32u && byte <= 126u) fputc((int)byte, output);
            else fprintf(output, "\\x%02x", (unsigned)byte);
        }
        fputc('\n', output);
    }
    fprintf(output, "Status: %s\n", state.status);
    state_close(&state);
    return ferror(output) ? -1 : 0;
}
