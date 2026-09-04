#ifndef NOMAD_TUI_TEXT_H
#define NOMAD_TUI_TEXT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

/* Formats length bytes as lowercase hex. out must hold 2 * length + 1 bytes. */
void tui_hex_format(const uint8_t *bytes, size_t length, char *out);
/* Parses exactly 2 * length hex digits. Any other input is rejected. */
bool tui_hex_parse(const char *text, uint8_t *out, size_t length);

/* ASCII case-insensitive substring search. An empty needle always matches. */
bool tui_text_contains(const uint8_t *haystack, size_t haystack_length,
                       const char *needle);

/*
 * Copies data with every control byte replaced by a space so that untrusted
 * message content can never emit a terminal control sequence. Bytes above
 * 0x7f are preserved so valid UTF-8 survives. Returns the length written,
 * excluding the terminator.
 */
size_t tui_text_sanitize(const uint8_t *data, size_t length, char *out,
                         size_t capacity);

/* Writes data with control, backslash and non-ASCII bytes escaped. */
void tui_text_escape(FILE *output, const uint8_t *data, size_t length);

/*
 * Returns the length of the complete, canonical UTF-8 sequence starting at
 * data, or 0 when the available bytes do not begin one.
 */
size_t tui_utf8_length(const uint8_t *data, size_t available);
bool tui_utf8_valid(const uint8_t *data, size_t length);
/* Counts display columns, treating each UTF-8 sequence as a single column. */
size_t tui_utf8_columns(const char *text, size_t length);

#endif
