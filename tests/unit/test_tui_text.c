#include "tui_text.h"

#include <assert.h>
#include <string.h>

static void test_hex(void) {
    const uint8_t bytes[4] = {0x00u, 0x0fu, 0xa5u, 0xffu};
    char text[9];
    uint8_t parsed[4];
    tui_hex_format(bytes, sizeof bytes, text);
    assert(strcmp(text, "000fa5ff") == 0);
    assert(tui_hex_parse(text, parsed, sizeof parsed));
    assert(memcmp(parsed, bytes, sizeof bytes) == 0);
    assert(tui_hex_parse("000FA5FF", parsed, sizeof parsed));
    assert(memcmp(parsed, bytes, sizeof bytes) == 0);
    assert(!tui_hex_parse("000fa5f", parsed, sizeof parsed));
    assert(!tui_hex_parse("000fa5fff", parsed, sizeof parsed));
    assert(!tui_hex_parse("000fa5fg", parsed, sizeof parsed));
    assert(!tui_hex_parse(NULL, parsed, sizeof parsed));
}

static void test_contains(void) {
    const uint8_t text[] = "Hello Reticulum";
    assert(tui_text_contains(text, sizeof text - 1u, "RETICULUM"));
    assert(tui_text_contains(text, sizeof text - 1u, "hello"));
    assert(tui_text_contains(text, sizeof text - 1u, ""));
    assert(!tui_text_contains(text, sizeof text - 1u, "lxmf"));
    /* A needle longer than the haystack never matches. */
    assert(!tui_text_contains(text, 3u, "Hello"));
}

static void test_sanitize(void) {
    const uint8_t raw[] = {'a', 0x1bu, '[', '2', 'J', '\n', 0x7fu, 0xc3u, 0xa9u};
    char out[16];
    size_t written = tui_text_sanitize(raw, sizeof raw, out, sizeof out);
    assert(written == sizeof raw);
    /* Escape, newline and DEL become spaces; UTF-8 bytes are preserved. */
    assert(memcmp(out, "a [2J  \xc3\xa9", written) == 0);
    assert(out[written] == '\0');
    /* Truncation still terminates. */
    assert(tui_text_sanitize(raw, sizeof raw, out, 4u) == 3u);
    assert(out[3] == '\0');
    assert(tui_text_sanitize(raw, sizeof raw, out, 0u) == 0u);
}

static void test_escape(void) {
    const uint8_t raw[] = {'o', 'k', '\n', '\t', '\\', 0x1bu, 0xffu};
    char buffer[64];
    FILE *file = tmpfile();
    assert(file != NULL);
    tui_text_escape(file, raw, sizeof raw);
    rewind(file);
    size_t length = fread(buffer, 1u, sizeof buffer - 1u, file);
    buffer[length] = '\0';
    assert(strcmp(buffer, "ok\\n\\t\\\\\\x1b\\xff") == 0);
    (void)fclose(file);
}

static void test_utf8(void) {
    const uint8_t two[] = {0xc3u, 0xa9u};
    const uint8_t three[] = {0xe2u, 0x82u, 0xacu};
    const uint8_t overlong[] = {0xc0u, 0x80u};
    const uint8_t surrogate[] = {0xedu, 0xa0u, 0x80u};
    const uint8_t truncated[] = {0xc3u};
    assert(tui_utf8_length((const uint8_t *)"a", 1u) == 1u);
    assert(tui_utf8_length(two, sizeof two) == 2u);
    assert(tui_utf8_length(three, sizeof three) == 3u);
    assert(tui_utf8_length(overlong, sizeof overlong) == 0u);
    assert(tui_utf8_length(surrogate, sizeof surrogate) == 0u);
    assert(tui_utf8_length(truncated, sizeof truncated) == 0u);
    assert(tui_utf8_valid((const uint8_t *)"h\xc3\xa9llo", 6u));
    assert(!tui_utf8_valid(overlong, sizeof overlong));
    assert(tui_utf8_columns("h\xc3\xa9llo", 6u) == 5u);
    assert(tui_utf8_columns("", 0u) == 0u);
}

static void test_wrap(void) {
    const char *text = "hello world\n\nabcdefgh";
    const char *expected[] = {"hello ", "world", "", "abcdef", "gh"};
    size_t at = 0u, start, bytes;
    for (size_t i = 0u; i < sizeof expected / sizeof expected[0]; ++i) {
        assert(tui_text_wrap_next(text, strlen(text), 6u, &at, &start, &bytes));
        assert(bytes == strlen(expected[i]));
        assert(memcmp(text + start, expected[i], bytes) == 0);
    }
    assert(at == strlen(text));
    assert(!tui_text_wrap_next(text, strlen(text), 6u, &at, &start, &bytes));
    at = 0u;
    assert(!tui_text_wrap_next(text, strlen(text), 0u, &at, &start, &bytes));
    const char invalid[] = {'a', (char)0xff, 'b'};
    assert(tui_text_wrap_next(invalid, sizeof invalid, 2u, &at, &start, &bytes));
    assert(bytes == 2u && at == 2u);
}

int main(void) {
    test_wrap();
    test_hex();
    test_contains();
    test_sanitize();
    test_escape();
    test_utf8();
    return 0;
}
