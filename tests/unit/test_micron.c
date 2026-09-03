#include "reticulum/micron.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static rns_micron_page page;

static void parse(const char *markup) {
    assert(rns_micron_parse(&page, (const uint8_t *)markup, strlen(markup)) == 1);
}

static const rns_micron_span *span_at(size_t line, size_t index) {
    assert(line < page.line_count);
    assert(index < page.lines[line].span_count);
    return &page.spans[page.lines[line].first_span + index];
}

static const char *text_at(size_t line, size_t index) {
    return rns_micron_span_text(&page, span_at(line, index));
}

/* Sections latch: depth persists until a deeper heading or a '<' reset. */
static void test_sections(void) {
    parse(">Title\nbody\n>>Sub\nmore\n<\nflat\n");
    assert(page.line_count == 6u);
    assert(page.lines[0].heading == 1u && page.lines[0].depth == 1u);
    assert(strcmp(text_at(0u, 0u), "Title") == 0);
    assert(page.lines[1].heading == 0u && page.lines[1].depth == 1u);
    assert(page.lines[2].heading == 2u && page.lines[2].depth == 2u);
    assert(page.lines[3].depth == 2u);
    /* '<' alone resets depth and produces no line of its own. */
    assert(page.lines[4].depth == 0u);
    assert(strcmp(text_at(4u, 0u), "flat") == 0);
    /* Deeper nesting than there are heading styles still clamps the level. */
    parse(">>>>>deep\n");
    assert(page.lines[0].depth == 5u);
    assert(page.lines[0].heading == RNS_MICRON_MAX_HEADING);
}

/* Formatting is a latch, not a wrapper, and carries across lines. */
static void test_formatting(void) {
    parse("plain `!bold`! plain\nstill plain\n");
    assert(page.lines[0].span_count == 3u);
    assert(!span_at(0u, 0u)->style.bold);
    assert(strcmp(text_at(0u, 1u), "bold") == 0 && span_at(0u, 1u)->style.bold);
    assert(!span_at(0u, 2u)->style.bold);

    parse("`_under\nnext line\n");
    assert(span_at(0u, 0u)->style.underline);
    assert(span_at(1u, 0u)->style.underline);

    /* The double backtick clears every latched attribute at once. */
    parse("`!`_`*mixed``clean\n");
    assert(span_at(0u, 0u)->style.bold && span_at(0u, 0u)->style.underline &&
           span_at(0u, 0u)->style.italic);
    assert(!span_at(0u, 1u)->style.bold && !span_at(0u, 1u)->style.italic);
}

static void test_colors(void) {
    parse("`F00fblue`f default\n");
    assert(span_at(0u, 0u)->style.foreground == 0x0000ffu);
    assert(span_at(0u, 1u)->style.foreground == RNS_MICRON_COLOR_DEFAULT);
    parse("`FT123456true`BT654321both\n");
    assert(span_at(0u, 0u)->style.foreground == 0x123456u);
    assert(span_at(0u, 1u)->style.background == 0x654321u);
    /* Three nibbles expand, so ccc is a light grey and not 0x000ccc. */
    parse("`Fcccgrey\n");
    assert(span_at(0u, 0u)->style.foreground == 0xccccccu);
    /* A malformed colour still consumes its digits so the text survives. */
    parse("`Fzzzkept\n");
    assert(strcmp(text_at(0u, 0u), "kept") == 0);
    assert(span_at(0u, 0u)->style.foreground == RNS_MICRON_COLOR_DEFAULT);
}

static void test_alignment(void) {
    parse("`ccentre\n`rright\n`aback\n");
    assert(page.lines[0].align == RNS_MICRON_ALIGN_CENTER);
    assert(page.lines[1].align == RNS_MICRON_ALIGN_RIGHT);
    assert(page.lines[2].align == RNS_MICRON_ALIGN_LEFT);
}

static void test_links(void) {
    parse("`[Network`/page/network.mu] and `[/page/bare.mu]\n");
    assert(rns_micron_link_count(&page) == 2u);
    const rns_micron_span *first = rns_micron_link(&page, 0u);
    assert(strcmp(rns_micron_span_text(&page, first), "Network") == 0);
    assert(strcmp(rns_micron_span_target(&page, first), "/page/network.mu") == 0);
    /* A link with no label displays its own target. */
    const rns_micron_span *second = rns_micron_link(&page, 1u);
    assert(strcmp(rns_micron_span_text(&page, second), "/page/bare.mu") == 0);
    assert(strcmp(rns_micron_span_target(&page, second), "/page/bare.mu") == 0);
    assert(rns_micron_link(&page, 2u) == NULL);
    assert(rns_micron_link_index(&page, 9u) == SIZE_MAX);

    parse("`[Submit`/page/form.mu`name|email]\n");
    assert(rns_micron_link_count(&page) == 1u);
    assert(strcmp(rns_micron_span_value(&page, rns_micron_link(&page, 0u)),
                  "name|email") == 0);

    /* An empty label takes the target as its text, as upstream does. */
    parse("`[`/page/labelless.mu]\n");
    assert(rns_micron_link_count(&page) == 1u);
    assert(strcmp(rns_micron_span_text(&page, rns_micron_link(&page, 0u)),
                  "/page/labelless.mu") == 0);

    /* Malformed links are dropped rather than rendered as markup. */
    parse("`[unterminated\n`[a`b`c`d]\n`[label`]\n`[]\n");
    assert(rns_micron_link_count(&page) == 0u);
}

static void test_escapes_and_comments(void) {
    parse("# a comment\nvisible\n");
    /* A trailing newline ends a final, empty line, as upstream renders it. */
    assert(page.line_count == 2u && page.lines[1].span_count == 0u);
    assert(strcmp(text_at(0u, 0u), "visible") == 0);
    /* A leading backslash escapes the line's first character only. */
    parse("\\# not a comment\n");
    assert(strcmp(text_at(0u, 0u), "# not a comment") == 0);
    parse("a \\`b and \\\\c\n");
    assert(strcmp(text_at(0u, 0u), "a `b and \\c") == 0);
}

static void test_literal(void) {
    parse("before\n`=\n`!raw`[not`a link]\n`=\nafter\n");
    assert(page.line_count == 4u);
    assert(strcmp(text_at(0u, 0u), "before") == 0);
    assert(strcmp(text_at(1u, 0u), "`!raw`[not`a link]") == 0);
    assert(!span_at(1u, 0u)->style.bold);
    assert(strcmp(text_at(2u, 0u), "after") == 0);
    assert(rns_micron_link_count(&page) == 0u);
}

static void test_dividers(void) {
    parse("-\n-=\n---\n");
    assert(page.line_count == 4u);
    assert(page.lines[0].divider &&
           strcmp(page.lines[0].divider_char, "\xE2\x94\x80") == 0);
    assert(page.lines[1].divider && strcmp(page.lines[1].divider_char, "=") == 0);
    /* More than one trailing character is not a divider glyph. */
    assert(page.lines[2].divider &&
           strcmp(page.lines[2].divider_char, "\xE2\x94\x80") == 0);
}

static void test_fields(void) {
    parse("`<user`alice>\n");
    const rns_micron_span *field = span_at(0u, 0u);
    assert(field->kind == RNS_MICRON_SPAN_FIELD);
    assert(strcmp(rns_micron_span_target(&page, field), "user") == 0);
    assert(strcmp(rns_micron_span_text(&page, field), "alice") == 0);
    assert(field->width == RNS_MICRON_FIELD_WIDTH_DEFAULT && !field->masked);

    parse("`<!32|secret`>\n");
    field = span_at(0u, 0u);
    assert(field->masked && field->width == 32u);
    assert(strcmp(rns_micron_span_target(&page, field), "secret") == 0);

    parse("`<?|opt|yes|*`Enable it>\n");
    field = span_at(0u, 0u);
    assert(field->kind == RNS_MICRON_SPAN_CHECKBOX && field->prechecked);
    assert(strcmp(rns_micron_span_target(&page, field), "opt") == 0);
    assert(strcmp(rns_micron_span_value(&page, field), "yes") == 0);
    assert(strcmp(rns_micron_span_text(&page, field), "Enable it") == 0);

    /* A toggle with no explicit value submits its own label. */
    parse("`<^|choice`Second>\n");
    field = span_at(0u, 0u);
    assert(field->kind == RNS_MICRON_SPAN_RADIO && !field->prechecked);
    assert(strcmp(rns_micron_span_value(&page, field), "Second") == 0);

    /* A line carrying a field is never also a heading. */
    parse(">>Name: `<who`>\n");
    assert(page.lines[0].heading == 0u);
    assert(span_at(0u, 1u)->kind == RNS_MICRON_SPAN_FIELD);

    /* An unterminated field degrades to text; no control is created. */
    parse("`<broken`no close\n");
    for (uint16_t i = 0u; i < page.span_count; ++i)
        assert(page.spans[i].kind == RNS_MICRON_SPAN_TEXT);
    assert(strcmp(text_at(0u, 0u), "broken") == 0);
}

static void test_blank_lines_and_unsupported(void) {
    parse("one\n\n\ntwo\n");
    assert(page.line_count == 5u);
    assert(page.lines[1].span_count == 0u && page.lines[2].span_count == 0u);

    parse("`t\na|b\n`t\n");
    assert(page.unsupported);
    parse("`{/page/live.mu`10}\n");
    assert(page.unsupported && page.line_count == 2u);
}

/* A remote node controls the page, so every bound must hold without crashing. */
static void test_bounds(void) {
    static char huge[128u * 1024u];
    size_t offset = 0u;
    for (size_t i = 0u; i < 4000u; ++i) {
        int written = snprintf(huge + offset, sizeof huge - offset,
                               "line %zu with `!markup`! and `[a`/b]\n", i);
        if (written <= 0 || (size_t)written >= sizeof huge - offset) break;
        offset += (size_t)written;
    }
    assert(rns_micron_parse(&page, (const uint8_t *)huge, offset) == 1);
    assert(page.truncated);
    assert(page.line_count <= RNS_MICRON_MAX_LINES);
    assert(page.span_count <= RNS_MICRON_MAX_SPANS);
    assert(page.pool_used <= RNS_MICRON_POOL_SIZE);

    /* A single line longer than one span still parses and reports truncation. */
    static char long_line[4096];
    memset(long_line, 'x', sizeof long_line - 1u);
    long_line[sizeof long_line - 1u] = '\0';
    assert(rns_micron_parse(&page, (const uint8_t *)long_line,
                            sizeof long_line - 1u) == 1);
    assert(page.truncated);

    assert(rns_micron_parse(NULL, (const uint8_t *)"x", 1u) == 0);
    assert(rns_micron_parse(&page, NULL, 1u) == 0);
    /* An empty document is one empty line, not a malformed page. */
    assert(rns_micron_parse(&page, NULL, 0u) == 1);
    assert(page.line_count == 1u && page.span_count == 0u && !page.truncated);

    /* Truncated UTF-8 and lone escapes must not read past the end. */
    parse("`F\n`\n\\\n`[\n`<\n`:\n\xC3\n");
    assert(rns_micron_span_text(&page, NULL)[0] == '\0');
    assert(rns_micron_span_target(&page, NULL)[0] == '\0');
    assert(rns_micron_span_value(&page, NULL)[0] == '\0');
}

static void test_urls_and_history(void) {
    char url[128];
    assert(rns_micron_normalize_url("lxmf://node/apps/home", "next", url, sizeof url) &&
           strcmp(url, "lxmf://node/apps/next") == 0);
    assert(rns_micron_normalize_url("0123456789abcdef0123456789abcdef:/page/index.mu",
                                    "/page/about.mu", url, sizeof url) &&
           strcmp(url, "0123456789abcdef0123456789abcdef:/page/about.mu") == 0);
    assert(rns_micron_normalize_url("0123456789abcdef0123456789abcdef:/page/index.mu",
                                    "sub.mu", url, sizeof url) &&
           strcmp(url, "0123456789abcdef0123456789abcdef:/page/sub.mu") == 0);
    assert(rns_micron_normalize_url("0123456789abcdef0123456789abcdef:/page/index.mu",
                                    ":/page/form.mu", url, sizeof url) &&
           strcmp(url, "0123456789abcdef0123456789abcdef:/page/form.mu") == 0);
    assert(rns_micron_normalize_url("0123456789abcdef0123456789abcdef:/page/index.mu",
                                    "fedcba9876543210fedcba9876543210:/page/home.mu",
                                    url, sizeof url) &&
           strcmp(url, "fedcba9876543210fedcba9876543210:/page/home.mu") == 0);
    assert(rns_micron_normalize_url(
               "0123456789abcdef0123456789abcdef:/page/index.mu#old", "#notes",
               url, sizeof url) &&
           strcmp(url, "0123456789abcdef0123456789abcdef:/page/index.mu#notes") == 0);
    assert(rns_micron_normalize_url(NULL, "x", url, 1u) == 0);

    rns_micron_history history;
    rns_micron_history_init(&history);
    assert(rns_micron_history_push(&history, "a"));
    assert(rns_micron_history_push(&history, "b"));
    assert(strcmp(rns_micron_history_back(&history), "a") == 0);
    assert(strcmp(rns_micron_history_forward(&history), "b") == 0);
    assert(rns_micron_history_forward(&history) == NULL);
}

int main(void) {
    test_sections();
    test_formatting();
    test_colors();
    test_alignment();
    test_links();
    test_escapes_and_comments();
    test_literal();
    test_dividers();
    test_fields();
    test_blank_lines_and_unsupported();
    test_bounds();
    test_urls_and_history();
    return 0;
}
