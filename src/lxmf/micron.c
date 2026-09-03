#include "reticulum/micron.h"

#include <stdio.h>
#include <string.h>

/*
 * Micron parser. The grammar is the one implemented by the pinned Nomad
 * Network release recorded in docs/COMPATIBILITY.md: an escape character
 * (backtick) introduces a single-character formatting command, and line
 * leading characters select block behaviour. Formatting, alignment, section
 * depth and literal mode latch across lines for the whole document.
 */

#define MICRON_DIVIDER_DEFAULT "\xE2\x94\x80" /* U+2500 BOX DRAWINGS LIGHT HORIZONTAL */

typedef struct {
    rns_micron_style style;
    uint32_t default_foreground;
    uint32_t default_background;
    uint8_t align;
    uint8_t default_align;
    uint8_t depth;
    bool literal;
    bool table_mode;
} micron_state;

typedef struct {
    rns_micron_page *page;
    micron_state state;
    uint16_t line_first_span;
} micron_parser;

/* ------------------------------------------------------------------ storage */

static bool pool_add(micron_parser *parser, const char *text, size_t length,
                     uint16_t *offset, uint16_t *stored) {
    rns_micron_page *page = parser->page;
    if (length > RNS_MICRON_TEXT_MAX) length = RNS_MICRON_TEXT_MAX;
    if (length + 1u > (size_t)(RNS_MICRON_POOL_SIZE - page->pool_used)) {
        page->truncated = true;
        return false;
    }
    *offset = page->pool_used;
    *stored = (uint16_t)length;
    if (length != 0u) memcpy(page->pool + page->pool_used, text, length);
    page->pool[page->pool_used + length] = '\0';
    page->pool_used = (uint16_t)(page->pool_used + length + 1u);
    return true;
}

static rns_micron_span *span_new(micron_parser *parser, rns_micron_span_kind kind) {
    rns_micron_page *page = parser->page;
    if (page->span_count >= RNS_MICRON_MAX_SPANS) {
        page->truncated = true;
        return NULL;
    }
    rns_micron_span *span = &page->spans[page->span_count++];
    memset(span, 0, sizeof *span);
    span->kind = kind;
    span->style = parser->state.style;
    return span;
}

/* Flushes accumulated plain text as one styled span. */
static void emit_text(micron_parser *parser, const char *text, size_t length) {
    if (length == 0u) return;
    rns_micron_span *span = span_new(parser, RNS_MICRON_SPAN_TEXT);
    if (span == NULL) return;
    if (!pool_add(parser, text, length, &span->text_offset, &span->text_length))
        --parser->page->span_count;
}

static void emit_line(micron_parser *parser, uint8_t heading, bool divider,
                      const char *divider_char) {
    rns_micron_page *page = parser->page;
    if (page->line_count >= RNS_MICRON_MAX_LINES) {
        page->truncated = true;
        return;
    }
    rns_micron_line *line = &page->lines[page->line_count++];
    memset(line, 0, sizeof *line);
    line->first_span = parser->line_first_span;
    line->span_count = (uint16_t)(page->span_count - parser->line_first_span);
    line->depth = parser->state.depth;
    line->heading = heading;
    line->align = parser->state.align;
    line->divider = divider;
    if (divider && divider_char != NULL)
        (void)snprintf(line->divider_char, sizeof line->divider_char, "%s", divider_char);
}

/* --------------------------------------------------------------- primitives */

static bool hex_value(char c, uint32_t *value) {
    if (c >= '0' && c <= '9') *value = (uint32_t)(c - '0');
    else if (c >= 'a' && c <= 'f') *value = (uint32_t)(c - 'a') + 10u;
    else if (c >= 'A' && c <= 'F') *value = (uint32_t)(c - 'A') + 10u;
    else return false;
    return true;
}

/* Three nibbles expand to eight bits per channel; six are taken verbatim. */
static bool parse_color(const char *digits, size_t count, uint32_t *color) {
    uint32_t packed = 0u;
    for (size_t i = 0u; i < count; ++i) {
        uint32_t nibble;
        if (!hex_value(digits[i], &nibble)) return false;
        packed = (packed << 4) | nibble;
    }
    if (count == 3u)
        packed = ((packed & 0xF00u) * 0x1100u) | ((packed & 0x0F0u) * 0x110u) |
                 ((packed & 0x00Fu) * 0x11u);
    *color = packed;
    return true;
}

static bool name_char(char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
           (c >= '0' && c <= '9') || c == '_' || c == '-';
}

/* Length of the UTF-8 sequence starting at text, or 0 when it is not one. */
static size_t utf8_sequence(const char *text, size_t length) {
    unsigned char lead = (unsigned char)text[0];
    size_t needed;
    if (lead < 0x80u) needed = 1u;
    else if ((lead & 0xE0u) == 0xC0u) needed = 2u;
    else if ((lead & 0xF0u) == 0xE0u) needed = 3u;
    else if ((lead & 0xF8u) == 0xF0u) needed = 4u;
    else return 0u;
    if (needed > length) return 0u;
    for (size_t i = 1u; i < needed; ++i)
        if (((unsigned char)text[i] & 0xC0u) != 0x80u) return 0u;
    return needed;
}

static size_t find_byte(const char *text, size_t start, size_t length, char wanted) {
    for (size_t i = start; i < length; ++i)
        if (text[i] == wanted) return i;
    return length;
}

/* ------------------------------------------------------------------- inline */

/*
 * A link body is up to three backtick separated components. One component is
 * a bare URL, two are label and URL, three add the field selector list. More
 * than three is malformed and the whole link is dropped, as upstream does.
 */
static size_t emit_link(micron_parser *parser, const char *body, size_t length) {
    const char *label = NULL, *url = NULL, *fields = NULL;
    size_t label_length = 0u, url_length = 0u, fields_length = 0u;
    size_t first = find_byte(body, 0u, length, '`');
    if (first == length) {
        url = body;
        url_length = length;
    } else {
        size_t second = find_byte(body, first + 1u, length, '`');
        label = body;
        label_length = first;
        url = body + first + 1u;
        if (second == length) url_length = length - first - 1u;
        else {
            url_length = second - first - 1u;
            fields = body + second + 1u;
            fields_length = length - second - 1u;
            if (find_byte(body, second + 1u, length, '`') != length) return 0u;
        }
    }
    if (url_length == 0u) return 0u;
    if (label_length == 0u) {
        label = url;
        label_length = url_length;
    }
    rns_micron_span *span = span_new(parser, RNS_MICRON_SPAN_LINK);
    if (span == NULL) return 0u;
    if (!pool_add(parser, label, label_length, &span->text_offset, &span->text_length) ||
        !pool_add(parser, url, url_length, &span->target_offset, &span->target_length) ||
        (fields != NULL &&
         !pool_add(parser, fields, fields_length, &span->value_offset,
                   &span->value_length))) {
        --parser->page->span_count;
        return 0u;
    }
    return 1u;
}

/*
 * A field body is `<flags|name`value>. The flag group selects the control:
 * '^' radio, '?' checkbox, '!' masked text, and any remaining digits give the
 * display width of a text entry.
 */
static void emit_field(micron_parser *parser, const char *head, size_t head_length,
                       const char *data, size_t data_length) {
    rns_micron_span_kind kind = RNS_MICRON_SPAN_FIELD;
    const char *name = head, *value = NULL;
    size_t name_length = head_length, value_length = 0u;
    unsigned width = RNS_MICRON_FIELD_WIDTH_DEFAULT;
    bool masked = false, prechecked = false;
    size_t bar = find_byte(head, 0u, head_length, '|');
    if (bar != head_length) {
        const char *flags = head;
        size_t flags_length = bar;
        size_t next = find_byte(head, bar + 1u, head_length, '|');
        name = head + bar + 1u;
        name_length = next - bar - 1u;
        if (next != head_length) {
            size_t third = find_byte(head, next + 1u, head_length, '|');
            value = head + next + 1u;
            value_length = third - next - 1u;
            if (third != head_length && head_length - third == 2u && head[third + 1u] == '*')
                prechecked = true;
        }
        for (size_t i = 0u; i < flags_length; ++i) {
            if (flags[i] == '^') kind = RNS_MICRON_SPAN_RADIO;
            else if (flags[i] == '?') kind = RNS_MICRON_SPAN_CHECKBOX;
            else if (flags[i] == '!') masked = true;
        }
        unsigned parsed = 0u;
        bool digits = false;
        for (size_t i = 0u; i < flags_length; ++i) {
            if (flags[i] < '0' || flags[i] > '9') continue;
            digits = true;
            if (parsed <= RNS_MICRON_FIELD_WIDTH_MAX)
                parsed = parsed * 10u + (unsigned)(flags[i] - '0');
        }
        if (digits && parsed != 0u)
            width = parsed > RNS_MICRON_FIELD_WIDTH_MAX ? RNS_MICRON_FIELD_WIDTH_MAX
                                                        : parsed;
    }
    /* Toggles label themselves; only a text entry carries an initial value. */
    if (kind != RNS_MICRON_SPAN_FIELD && value_length == 0u) {
        value = data;
        value_length = data_length;
    }
    rns_micron_span *span = span_new(parser, kind);
    if (span == NULL) return;
    span->width = (uint16_t)width;
    span->masked = masked;
    span->prechecked = prechecked;
    if (!pool_add(parser, data, data_length, &span->text_offset, &span->text_length) ||
        !pool_add(parser, name, name_length, &span->target_offset,
                  &span->target_length) ||
        !pool_add(parser, value == NULL ? "" : value, value_length,
                  &span->value_offset, &span->value_length))
        --parser->page->span_count;
}

/* Returns the number of further bytes to skip after the command character. */
static size_t apply_formatting(micron_parser *parser, const char *line, size_t length,
                               size_t index) {
    micron_state *state = &parser->state;
    char command = line[index];
    switch (command) {
        case '_': state->style.underline = !state->style.underline; return 0u;
        case '!': state->style.bold = !state->style.bold; return 0u;
        case '*': state->style.italic = !state->style.italic; return 0u;
        case 'f': state->style.foreground = state->default_foreground; return 0u;
        case 'b': state->style.background = state->default_background; return 0u;
        case 'c': state->align = RNS_MICRON_ALIGN_CENTER; return 0u;
        case 'l': state->align = RNS_MICRON_ALIGN_LEFT; return 0u;
        case 'r': state->align = RNS_MICRON_ALIGN_RIGHT; return 0u;
        case 'a': state->align = state->default_align; return 0u;
        case '`':
            state->style.bold = false;
            state->style.underline = false;
            state->style.italic = false;
            state->style.foreground = state->default_foreground;
            state->style.background = state->default_background;
            state->align = state->default_align;
            return 0u;
        case 'F': case 'B': {
            uint32_t color;
            uint32_t *slot = command == 'F' ? &state->style.foreground
                                            : &state->style.background;
            if (length >= index + 8u && line[index + 1u] == 'T') {
                /*
                 * A malformed colour still consumes its digits, so the rest of
                 * the line keeps its intended meaning; only the colour is
                 * ignored.
                 */
                if (parse_color(line + index + 2u, 6u, &color)) *slot = color;
                return 7u;
            }
            if (length >= index + 4u) {
                if (parse_color(line + index + 1u, 3u, &color)) *slot = color;
                return 3u;
            }
            return 0u;
        }
        case ':': {
            /* Anchor declaration. Anchors are parsed away but not yet linkable. */
            size_t end = index + 1u;
            while (end < length && name_char(line[end])) ++end;
            return end - index - 1u;
        }
        default: return 0u;
    }
}

static void parse_content(micron_parser *parser, const char *line, size_t length,
                          bool pre_escape) {
    char part[RNS_MICRON_TEXT_MAX];
    size_t part_length = 0u;
    bool formatting = false, escape = pre_escape;
    size_t skip = 0u;

    if (parser->state.literal) {
        if (length == 3u && memcmp(line, "\\`=", 3u) == 0) emit_text(parser, "`=", 2u);
        else emit_text(parser, line, length);
        return;
    }
    for (size_t i = 0u; i < length; ++i) {
        if (skip != 0u) {
            --skip;
            continue;
        }
        if (formatting) {
            if (line[i] == '<') {
                size_t backtick = find_byte(line, i + 1u, length, '`');
                size_t end = backtick == length ? length
                                                : find_byte(line, backtick + 1u, length, '>');
                if (end != length) {
                    emit_field(parser, line + i + 1u, backtick - i - 1u,
                               line + backtick + 1u, end - backtick - 1u);
                    skip = end - i;
                }
            } else if (line[i] == '[') {
                size_t end = find_byte(line, i + 1u, length, ']');
                if (end != length) {
                    (void)emit_link(parser, line + i + 1u, end - i - 1u);
                    skip = end - i;
                }
            } else {
                skip = apply_formatting(parser, line, length, i);
            }
            formatting = false;
        } else if (line[i] == '\\' && !escape) {
            escape = true;
        } else if (line[i] == '`' && !escape) {
            emit_text(parser, part, part_length);
            part_length = 0u;
            formatting = true;
        } else {
            if (part_length < sizeof part) part[part_length++] = line[i];
            else parser->page->truncated = true;
            escape = false;
        }
    }
    emit_text(parser, part, part_length);
}

/* ------------------------------------------------------------------- blocks */

static void parse_line(micron_parser *parser, const char *line, size_t length) {
    micron_state *state = &parser->state;
    bool pre_escape = false;

    parser->line_first_span = parser->page->span_count;
    if (length == 0u) return;
    if (length == 2u && line[0] == '`' && line[1] == '=') {
        state->literal = !state->literal;
        return;
    }
    if (state->literal) {
        parse_content(parser, line, length, false);
        emit_line(parser, 0u, false, NULL);
        return;
    }
    /* A line carrying an input field is never also a heading. */
    if (line[0] == '>' && length >= 2u) {
        for (size_t i = 0u; i + 1u < length; ++i) {
            if (line[i] != '`' || line[i + 1u] != '<') continue;
            while (length != 0u && line[0] == '>') { ++line; --length; }
            break;
        }
        if (length == 0u) return;
    }
    if (line[0] == '\\') {
        ++line;
        --length;
        pre_escape = true;
    } else if (line[0] == '#') {
        return;
    }
    if (length == 0u) return;

    if (length >= 2u && line[0] == '`' && line[1] == 't') {
        /* Tables are recognised so they cannot corrupt the rest of the page,
         * but the column layout is not implemented; rows render as text. */
        state->table_mode = !state->table_mode;
        parser->page->unsupported = true;
        return;
    }
    if (!state->table_mode && length >= 2u && line[0] == '`' && line[1] == '{') {
        /* A partial is a deferred sub-fetch. Show the upstream placeholder. */
        parser->page->unsupported = true;
        emit_text(parser, "\xE2\xA7\x96", 3u);
        emit_line(parser, 0u, false, NULL);
        return;
    }
    if (line[0] == '<') {
        state->depth = 0u;
        parse_line(parser, line + 1u, length - 1u);
        return;
    }
    if (line[0] == '>') {
        size_t level = 0u;
        while (level < length && line[level] == '>') ++level;
        if (level > UINT8_MAX) level = UINT8_MAX;
        state->depth = (uint8_t)level;
        line += level;
        length -= level;
        if (length == 0u) return;
        parse_content(parser, line, length, false);
        emit_line(parser,
                  level > RNS_MICRON_MAX_HEADING ? (uint8_t)RNS_MICRON_MAX_HEADING
                                                 : (uint8_t)level,
                  false, NULL);
        return;
    }
    if (line[0] == '-') {
        const char *divider = MICRON_DIVIDER_DEFAULT;
        size_t rest = length - 1u;
        if (rest != 0u && utf8_sequence(line + 1u, rest) == rest &&
            (unsigned char)line[1] >= 0x20u)
            divider = NULL;
        emit_line(parser, 0u, true, divider);
        if (divider == NULL) {
            rns_micron_line *emitted = &parser->page->lines[parser->page->line_count - 1u];
            (void)snprintf(emitted->divider_char, sizeof emitted->divider_char, "%.*s",
                           (int)rest, line + 1u);
        }
        return;
    }
    parse_content(parser, line, length, pre_escape);
    if (parser->page->span_count != parser->line_first_span)
        emit_line(parser, 0u, false, NULL);
}

int rns_micron_parse(rns_micron_page *page, const uint8_t *data, size_t length) {
    micron_parser parser;
    if (page == NULL || (data == NULL && length != 0u)) return 0;
    memset(page, 0, sizeof *page);
    memset(&parser, 0, sizeof parser);
    parser.page = page;
    parser.state.style.foreground = RNS_MICRON_COLOR_DEFAULT;
    parser.state.style.background = RNS_MICRON_COLOR_DEFAULT;
    parser.state.default_foreground = RNS_MICRON_COLOR_DEFAULT;
    parser.state.default_background = RNS_MICRON_COLOR_DEFAULT;
    parser.state.align = RNS_MICRON_ALIGN_LEFT;
    parser.state.default_align = RNS_MICRON_ALIGN_LEFT;

    size_t start = 0u;
    while (start <= length) {
        size_t end = start;
        while (end < length && data[end] != '\n') ++end;
        if (end == start) {
            /* A blank source line is a blank rendered line, whatever the
             * latched state is; only non-empty lines carry markup. */
            parser.line_first_span = page->span_count;
            emit_line(&parser, 0u, false, NULL);
        } else {
            parse_line(&parser, (const char *)data + start, end - start);
        }
        if (end == length) break;
        start = end + 1u;
    }
    return 1;
}

/* --------------------------------------------------------------- accessors */

static const char *span_string(const rns_micron_page *page, uint16_t offset,
                               uint16_t stored) {
    if (page == NULL || (size_t)offset + stored >= RNS_MICRON_POOL_SIZE) return "";
    return page->pool + offset;
}

const char *rns_micron_span_text(const rns_micron_page *page,
                                 const rns_micron_span *span) {
    if (span == NULL) return "";
    return span_string(page, span->text_offset, span->text_length);
}

const char *rns_micron_span_target(const rns_micron_page *page,
                                   const rns_micron_span *span) {
    if (span == NULL) return "";
    return span_string(page, span->target_offset, span->target_length);
}

const char *rns_micron_span_value(const rns_micron_page *page,
                                  const rns_micron_span *span) {
    if (span == NULL) return "";
    return span_string(page, span->value_offset, span->value_length);
}

size_t rns_micron_link_count(const rns_micron_page *page) {
    size_t count = 0u;
    if (page == NULL) return 0u;
    for (size_t i = 0u; i < page->span_count; ++i)
        if (page->spans[i].kind == RNS_MICRON_SPAN_LINK) ++count;
    return count;
}

size_t rns_micron_link_index(const rns_micron_page *page, size_t nth) {
    size_t seen = 0u;
    if (page == NULL) return SIZE_MAX;
    for (size_t i = 0u; i < page->span_count; ++i) {
        if (page->spans[i].kind != RNS_MICRON_SPAN_LINK) continue;
        if (seen++ == nth) return i;
    }
    return SIZE_MAX;
}

const rns_micron_span *rns_micron_link(const rns_micron_page *page, size_t nth) {
    size_t index = rns_micron_link_index(page, nth);
    return index == SIZE_MAX ? NULL : &page->spans[index];
}

/* ---------------------------------------------------------------- addresses */

static int copy_url(char *out, size_t capacity, const char *text) {
    size_t length = strlen(text);
    if (out == NULL || length + 1u > capacity) return 0;
    memcpy(out, text, length + 1u);
    return 1;
}

static bool nomad_destination_prefix(const char *url) {
    if (url == NULL || strlen(url) < 33u || url[32] != ':') return false;
    for (size_t i = 0u; i < 32u; ++i) {
        char value = url[i];
        bool decimal = value >= '0' && value <= '9';
        bool lower = value >= 'a' && value <= 'f';
        bool upper = value >= 'A' && value <= 'F';
        if (!decimal && !lower && !upper) return false;
    }
    return true;
}

int rns_micron_normalize_url(const char *base, const char *target, char *out,
                             size_t capacity) {
    char root[RNS_MICRON_TEXT_MAX];
    char joined[RNS_MICRON_TEXT_MAX];
    if (target == NULL || out == NULL || capacity == 0u) return 0;
    if (strstr(target, "://") != NULL || strncmp(target, "lxmf:", 5u) == 0)
        return copy_url(out, capacity, target);
    if (base == NULL) return copy_url(out, capacity, target);
    if (!copy_url(root, sizeof root, base)) return 0;

    /* A complete Nomad URL changes destination and needs no base at all. */
    if (nomad_destination_prefix(target)) return copy_url(out, capacity, target);

    /* Anchors address the current document, without stacking old anchors. */
    if (target[0] == '#') {
        char *anchor = strchr(root, '#');
        if (anchor != NULL) *anchor = '\0';
        int written = snprintf(joined, sizeof joined, "%s%s", root, target);
        if (written < 0 || (size_t)written >= sizeof joined) return 0;
        return copy_url(out, capacity, joined);
    }

    /* Nomad's :/path shorthand keeps the current destination. */
    if (target[0] == ':' && target[1] == '/' && nomad_destination_prefix(root)) {
        root[32] = '\0';
    /* A <hash>:/path base is rooted at the colon, not at a path separator. */
    } else if (target[0] == '/' && nomad_destination_prefix(root)) {
        root[33] = '\0';
    } else {
        char *anchor = strchr(root, '#');
        if (anchor != NULL) *anchor = '\0';
        char *slash = strrchr(root, '/');
        if (target[0] == '/') {
            char *scheme = strstr(root, "://");
            slash = scheme != NULL ? strchr(scheme + 3, '/') : strchr(root, '/');
        }
        if (slash != NULL) slash[1] = '\0';
        else root[0] = '\0';
    }
    int written = snprintf(joined, sizeof joined, "%s%s", root, target);
    if (written < 0 || (size_t)written >= sizeof joined) return 0;
    return copy_url(out, capacity, joined);
}

void rns_micron_history_init(rns_micron_history *history) {
    if (history != NULL) memset(history, 0, sizeof *history);
}

int rns_micron_history_push(rns_micron_history *history, const char *url) {
    if (history == NULL || url == NULL || strlen(url) >= RNS_MICRON_TEXT_MAX) return 0;
    if (history->count != 0u && history->cursor < history->count &&
        strcmp(history->urls[history->cursor], url) == 0)
        return 1;
    if (history->cursor + 1u < history->count) history->count = history->cursor + 1u;
    if (history->count == RNS_MICRON_HISTORY_MAX) {
        memmove(history->urls, history->urls + 1,
                sizeof history->urls[0] * (RNS_MICRON_HISTORY_MAX - 1u));
        history->count = RNS_MICRON_HISTORY_MAX - 1u;
    }
    memcpy(history->urls[history->count], url, strlen(url) + 1u);
    history->cursor = history->count++;
    return 1;
}

const char *rns_micron_history_back(rns_micron_history *history) {
    if (history == NULL || history->count == 0u || history->cursor == 0u) return NULL;
    return history->urls[--history->cursor];
}

const char *rns_micron_history_forward(rns_micron_history *history) {
    if (history == NULL || history->cursor + 1u >= history->count) return NULL;
    return history->urls[++history->cursor];
}
