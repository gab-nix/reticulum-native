#ifndef RETICULUM_MICRON_H
#define RETICULUM_MICRON_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Micron is the markup Nomad Network pages are written in. A document is a
 * sequence of lines; a line carries block attributes (section depth, heading
 * level, alignment, dividers) and a run of styled spans. Formatting latches
 * across lines, so a page must be parsed as a whole rather than line by line.
 *
 * Text is kept in one pool and referenced by offset so a page stays a plain
 * value the caller can own by copy, with no allocation and no interior
 * pointers to fix up.
 */

#define RNS_MICRON_TEXT_MAX 512u
#define RNS_MICRON_MAX_LINES 512u
#define RNS_MICRON_MAX_SPANS 1024u
#define RNS_MICRON_POOL_SIZE 16384u
#define RNS_MICRON_HISTORY_MAX 32u
#define RNS_MICRON_SECTION_INDENT 2u
#define RNS_MICRON_MAX_HEADING 3u
#define RNS_MICRON_FIELD_WIDTH_DEFAULT 24u
#define RNS_MICRON_FIELD_WIDTH_MAX 256u

/* Packed 0xRRGGBB, or the theme default when the page sets no colour. */
#define RNS_MICRON_COLOR_DEFAULT UINT32_C(0xFF000000)

typedef enum {
    RNS_MICRON_ALIGN_LEFT = 0,
    RNS_MICRON_ALIGN_CENTER,
    RNS_MICRON_ALIGN_RIGHT
} rns_micron_align;

typedef struct {
    uint32_t foreground;
    uint32_t background;
    bool bold;
    bool underline;
    bool italic;
} rns_micron_style;

typedef enum {
    RNS_MICRON_SPAN_TEXT = 0,
    RNS_MICRON_SPAN_LINK,
    RNS_MICRON_SPAN_FIELD,    /* single-line text entry */
    RNS_MICRON_SPAN_CHECKBOX,
    RNS_MICRON_SPAN_RADIO
} rns_micron_span_kind;

/*
 * text:   displayed characters, the link label, or the checkbox/radio label.
 * target: the link URL, or the field name for an input.
 * value:  the link's `field selector list, or the input's initial value.
 */
typedef struct {
    rns_micron_span_kind kind;
    rns_micron_style style;
    uint16_t text_offset;
    uint16_t text_length;
    uint16_t target_offset;
    uint16_t target_length;
    uint16_t value_offset;
    uint16_t value_length;
    uint16_t width;
    bool masked;
    bool prechecked;
} rns_micron_span;

typedef struct {
    uint16_t first_span;
    uint16_t span_count;
    uint8_t depth;         /* section nesting; 0 is the top level */
    uint8_t heading;       /* 0 for body text, otherwise the heading level */
    uint8_t align;         /* rns_micron_align */
    bool divider;
    char divider_char[5];  /* one UTF-8 codepoint, valid when divider is set */
} rns_micron_line;

typedef struct {
    rns_micron_line lines[RNS_MICRON_MAX_LINES];
    rns_micron_span spans[RNS_MICRON_MAX_SPANS];
    char pool[RNS_MICRON_POOL_SIZE];
    uint16_t line_count;
    uint16_t span_count;
    uint16_t pool_used;
    bool truncated;    /* the document exceeded one of the bounds above */
    bool unsupported;  /* tables or partials were flattened, not rendered */
} rns_micron_page;

typedef struct {
    char urls[RNS_MICRON_HISTORY_MAX][RNS_MICRON_TEXT_MAX];
    size_t count;
    size_t cursor;
} rns_micron_history;

/* Returns 1 on success, 0 only for invalid arguments. A document that exceeds
 * the bounds parses successfully with page->truncated set. */
int rns_micron_parse(rns_micron_page *page, const uint8_t *data, size_t length);

/* Span text accessors. The returned strings are NUL terminated and live in
 * the page, so they are valid for as long as the page value is. */
const char *rns_micron_span_text(const rns_micron_page *page,
                                 const rns_micron_span *span);
const char *rns_micron_span_target(const rns_micron_page *page,
                                   const rns_micron_span *span);
const char *rns_micron_span_value(const rns_micron_page *page,
                                  const rns_micron_span *span);

size_t rns_micron_link_count(const rns_micron_page *page);
/* Index of the nth link in page->spans, or SIZE_MAX when out of range. */
size_t rns_micron_link_index(const rns_micron_page *page, size_t nth);
const rns_micron_span *rns_micron_link(const rns_micron_page *page, size_t nth);

int rns_micron_normalize_url(const char *base, const char *target, char *out,
                             size_t capacity);
void rns_micron_history_init(rns_micron_history *history);
int rns_micron_history_push(rns_micron_history *history, const char *url);
const char *rns_micron_history_back(rns_micron_history *history);
const char *rns_micron_history_forward(rns_micron_history *history);

#endif
