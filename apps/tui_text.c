#define _XOPEN_SOURCE 700
#include "tui_text.h"

#include <ctype.h>
#include <string.h>
#include <wchar.h>

void tui_hex_format(const uint8_t *bytes, size_t length, char *out) {
    static const char digits[] = "0123456789abcdef";
    if (out == NULL || (bytes == NULL && length != 0u)) return;
    for (size_t i = 0u; i < length; ++i) {
        out[i * 2u] = digits[bytes[i] >> 4u];
        out[i * 2u + 1u] = digits[bytes[i] & 0x0fu];
    }
    out[length * 2u] = '\0';
}

static int hex_digit(char value) {
    if (value >= '0' && value <= '9') return value - '0';
    if (value >= 'a' && value <= 'f') return value - 'a' + 10;
    if (value >= 'A' && value <= 'F') return value - 'A' + 10;
    return -1;
}

bool tui_hex_parse(const char *text, uint8_t *out, size_t length) {
    if (text == NULL || out == NULL || strlen(text) != length * 2u) return false;
    for (size_t i = 0u; i < length; ++i) {
        int high = hex_digit(text[i * 2u]);
        int low = hex_digit(text[i * 2u + 1u]);
        if (high < 0 || low < 0) return false;
        out[i] = (uint8_t)((high << 4) | low);
    }
    return true;
}

bool tui_text_contains(const uint8_t *haystack, size_t haystack_length,
                       const char *needle) {
    if (needle == NULL) return true;
    size_t needle_length = strlen(needle);
    if (needle_length == 0u) return true;
    if (haystack == NULL || needle_length > haystack_length) return false;
    for (size_t start = 0u; start + needle_length <= haystack_length; ++start) {
        size_t i = 0u;
        while (i < needle_length &&
               tolower((unsigned char)haystack[start + i]) ==
                   tolower((unsigned char)needle[i])) ++i;
        if (i == needle_length) return true;
    }
    return false;
}

size_t tui_text_sanitize(const uint8_t *data, size_t length, char *out,
                         size_t capacity) {
    if (out == NULL || capacity == 0u) return 0u;
    size_t written = 0u;
    for (size_t i = 0u; i < length && written + 1u < capacity; ++i) {
        uint8_t byte = data[i];
        out[written++] = (byte < 0x20u || byte == 0x7fu) ? ' ' : (char)byte;
    }
    out[written] = '\0';
    return written;
}

void tui_text_escape(FILE *output, const uint8_t *data, size_t length) {
    if (output == NULL || (data == NULL && length != 0u)) return;
    for (size_t i = 0u; i < length; ++i) {
        uint8_t byte = data[i];
        if (byte == '\n') fputs("\\n", output);
        else if (byte == '\r') fputs("\\r", output);
        else if (byte == '\t') fputs("\\t", output);
        else if (byte == '\\') fputs("\\\\", output);
        else if (byte >= 0x20u && byte <= 0x7eu) fputc((int)byte, output);
        else fprintf(output, "\\x%02x", (unsigned)byte);
    }
}

size_t tui_utf8_length(const uint8_t *data, size_t available) {
    if (data == NULL || available == 0u) return 0u;
    uint8_t lead = data[0];
    size_t width;
    uint32_t codepoint;
    if (lead < 0x80u) return 1u;
    if (lead >= 0xc2u && lead <= 0xdfu) { width = 2u; codepoint = lead & 0x1fu; }
    else if (lead >= 0xe0u && lead <= 0xefu) { width = 3u; codepoint = lead & 0x0fu; }
    else if (lead >= 0xf0u && lead <= 0xf4u) { width = 4u; codepoint = lead & 0x07u; }
    else return 0u;
    if (available < width) return 0u;
    for (size_t i = 1u; i < width; ++i) {
        if ((data[i] & 0xc0u) != 0x80u) return 0u;
        codepoint = (codepoint << 6u) | (data[i] & 0x3fu);
    }
    if ((width == 2u && codepoint < 0x80u) ||
        (width == 3u && codepoint < 0x800u) ||
        (width == 4u && codepoint < 0x10000u) ||
        codepoint > 0x10ffffu ||
        (codepoint >= 0xd800u && codepoint <= 0xdfffu)) return 0u;
    return width;
}

bool tui_utf8_valid(const uint8_t *data, size_t length) {
    for (size_t offset = 0u; offset < length;) {
        size_t width = tui_utf8_length(data + offset, length - offset);
        if (width == 0u) return false;
        offset += width;
    }
    return true;
}

size_t tui_utf8_columns(const char *text, size_t length) {
    size_t columns = 0u;
    if (text == NULL) return 0u;
    for (size_t offset = 0u; offset < length; ++columns) {
        size_t width = tui_utf8_length((const uint8_t *)text + offset, length - offset);
        offset += width != 0u ? width : 1u;
    }
    return columns;
}

size_t tui_text_cell_width(const char *text, size_t length) {
    if (text == NULL || length == 0u) return 0u;
    size_t n = tui_utf8_length((const uint8_t *)text, length);
    if (n == 0u) return 1u;
    uint32_t cp = (uint8_t)text[0];
    if (n > 1u) {
        cp &= n == 2u ? 0x1fu : n == 3u ? 0x0fu : 0x07u;
        for (size_t i = 1u; i < n; ++i)
            cp = (cp << 6u) | ((uint8_t)text[i] & 0x3fu);
    }
    int width = wcwidth((wchar_t)cp);
    return width < 0 ? 1u : (size_t)width;
}

bool tui_text_wrap_next(const char *text, size_t length, size_t columns,
                        size_t *offset, size_t *start, size_t *bytes) {
    if (text == NULL || offset == NULL || start == NULL || bytes == NULL ||
        columns == 0u || *offset >= length) return false;
    size_t begin = *offset, at = begin, cells = 0u, word = begin;
    while (at < length) {
        if (text[at] == '\n') {
            *start = begin; *bytes = at - begin; *offset = at + 1u;
            return true;
        }
        size_t n = tui_utf8_length((const uint8_t *)text + at, length - at);
        if (n == 0u) n = 1u;
        size_t width = tui_text_cell_width(text + at, length - at);
        if (cells + width > columns && at > begin) break;
        cells += width;
        at += n;
        if (text[at - 1u] == ' ') word = at;
        if (cells > columns) break; /* A wide glyph in a one-cell pane. */
    }
    if (at < length && text[at] != '\n' && word > begin) at = word;
    *start = begin; *bytes = at - begin; *offset = at;
    return true;
}
