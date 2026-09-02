#include "tui.h"

#include "reticulum/destination.h"
#include "reticulum/identity.h"
#include "reticulum/lxmf.h"
#include "reticulum/lxmf_store.h"

#include <curses.h>
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

typedef struct {
    uint8_t peer[16];
    size_t messages;
    double latest;
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
    bool composing;
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
        state->contact_count++;
    }
    if (index < TUI_MAX_CONTACTS) {
        state->contacts[index].messages++;
        if (message->timestamp > state->contacts[index].latest)
            state->contacts[index].latest = message->timestamp;
    }
    return true;
}

static void add_contact(tui_state *state, const uint8_t peer[16]) {
    for (size_t i = 0; i < state->contact_count; ++i)
        if (memcmp(state->contacts[i].peer, peer, 16u) == 0) return;
    if (state->contact_count < TUI_MAX_CONTACTS)
        memcpy(state->contacts[state->contact_count++].peer, peer, 16u);
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

static bool selected_message(const tui_state *state, const tui_message *message) {
    if (state->selected >= state->contact_count) return false;
    const uint8_t *peer = memcmp(message->value.source, state->local, 16u) == 0
                              ? message->value.destination : message->value.source;
    return memcmp(peer, state->contacts[state->selected].peer, 16u) == 0;
}

static size_t selected_count(const tui_state *state) {
    size_t count = 0;
    for (size_t i = 0; i < state->message_count; ++i)
        if (selected_message(state, &state->messages[i])) ++count;
    return count;
}

static const tui_message *selected_at(const tui_state *state, size_t target) {
    size_t current = 0;
    for (size_t i = 0; i < state->message_count; ++i) {
        if (!selected_message(state, &state->messages[i])) continue;
        if (current++ == target) return &state->messages[i];
    }
    return NULL;
}

static void clipped(WINDOW *window, int y, int x, int width, const char *text) {
    if (width > 0) mvwaddnstr(window, y, x, text, width);
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

    if (!narrow) {
        attron(A_BOLD);
        clipped(stdscr, 2, 1, sidebar - 2, "CONVERSATIONS");
        attroff(A_BOLD);
        for (size_t i = 0; i < state->contact_count && (int)i < rows - 7; ++i) {
            char peer[33];
            hex_format(state->contacts[i].peer, 16u, peer);
            if (i == state->selected) attron(A_REVERSE);
            char line[64];
            snprintf(line, sizeof line, " %-12.12s  %3zu ", peer,
                     state->contacts[i].messages);
            clipped(stdscr, 3 + (int)i, 0, sidebar - 1, line);
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
    clipped(stdscr, rows - 3, 0, columns, state->composing ? "Message: " : state->status);
    if (state->composing) {
        clipped(stdscr, rows - 3, 9, columns - 9, state->composer);
        int cursor_x = 9 + (int)state->composer_cursor;
        move(rows - 3, cursor_x < columns ? cursor_x : columns - 1);
    }
    clipped(stdscr, rows - 2, 0, columns,
            state->composing ? "Enter queue  Esc cancel  Left/Right edit"
                             : "Up/Down or j/k select  Tab switch  Enter compose  q quit");
    clipped(stdscr, rows - 1, 0, columns,
            "[.] queued  [>] sending  [+] sent  [x] delivered  [!] failed");
    refresh();
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
    if (state->contact_count == 0u) return;
    if (delta < 0)
        state->selected = state->selected == 0u ? state->contact_count - 1u : state->selected - 1u;
    else
        state->selected = (state->selected + 1u) % state->contact_count;
    state->scroll = 0u;
}

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
        if (state->composing) {
            if (key == 27) {
                state->composing = false;
                state->composer_length = state->composer_cursor = 0u;
                state->composer[0] = '\0';
                curs_set(0);
            } else if (key == '\n' || key == KEY_ENTER) {
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
                    state->composing = false;
                    curs_set(0);
                }
            } else if (key == KEY_BACKSPACE || key == 127 || key == 8) {
                if (state->composer_cursor > 0u) {
                    memmove(state->composer + state->composer_cursor - 1u,
                            state->composer + state->composer_cursor,
                            state->composer_length - state->composer_cursor + 1u);
                    --state->composer_cursor;
                    --state->composer_length;
                }
            } else if (key == KEY_LEFT && state->composer_cursor > 0u) {
                --state->composer_cursor;
            } else if (key == KEY_RIGHT && state->composer_cursor < state->composer_length) {
                ++state->composer_cursor;
            } else if (key >= 32 && key <= 126 && state->composer_length < TUI_COMPOSER_MAX) {
                memmove(state->composer + state->composer_cursor + 1u,
                        state->composer + state->composer_cursor,
                        state->composer_length - state->composer_cursor + 1u);
                state->composer[state->composer_cursor++] = (char)key;
                ++state->composer_length;
            }
            continue;
        }
        switch (key) {
            case 'q': case 'Q': case 27: running = false; break;
            case KEY_UP: case 'k': select_delta(state, -1); break;
            case KEY_DOWN: case 'j': case '\t': select_delta(state, 1); break;
            case KEY_PPAGE: state->scroll += 5u; break;
            case KEY_NPAGE: state->scroll = state->scroll > 5u ? state->scroll - 5u : 0u; break;
            case '\n': case KEY_ENTER:
                if (state->contact_count > 0u) {
                    state->composing = true;
                    curs_set(1);
                } else {
                    snprintf(state->status, sizeof state->status,
                             "No conversation: provide a destination address");
                }
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
