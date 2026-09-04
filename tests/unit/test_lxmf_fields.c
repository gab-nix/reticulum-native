#include "reticulum/lxmf_fields.h"

#include "../fixtures/lxmf_standard_fields_fixture.h"

#include <assert.h>
#include <string.h>

static bool equals(lxmf_slice_t value, const void *expected, size_t length) {
    return value.len == length && memcmp(value.data, expected, length) == 0;
}

int main(void) {
    lxmf_standard_fields_t fields;
    assert(lxmf_standard_fields_parse(lxmf_standard_fields_fixture,
               sizeof lxmf_standard_fields_fixture, &fields) == LXMF_OK);
    assert(fields.present_mask == LXMF_STANDARD_ALL);
    assert(fields.renderer == LXMF_RENDERER_MARKDOWN);
    for (size_t i = 0u; i < LXMF_MESSAGE_ID_LENGTH; ++i) {
        assert(fields.reply_to[i] == (uint8_t)i);
        assert(fields.reaction_to[i] == (uint8_t)(31u - i));
        assert(fields.thread[i] == (uint8_t)(0x80u + i));
    }
    static const uint8_t quote[] = "quoted \xf0\x9f\x8c\xb8";
    static const uint8_t reaction[] = "\xf0\x9f\x91\x8d";
    assert(equals(fields.reply_quote, quote, sizeof quote - 1u));
    assert(equals(fields.reaction_content, reaction, sizeof reaction - 1u));
    assert(fields.attachment_count == 2u);
    assert(equals(fields.attachments[0].name, "notes.txt", 9u));
    static const uint8_t first_data[] = {0u, 1u};
    assert(equals(fields.attachments[0].data, first_data, sizeof first_data));
    assert(equals(fields.attachments[1].name, "data.bin", 8u));
    static const uint8_t second_data[] = {0xffu, 0xfeu, 0xfdu};
    assert(equals(fields.attachments[1].data, second_data,
                  sizeof second_data));
    assert(fields.image.format_kind == LXMF_MEDIA_FORMAT_TEXT);
    assert(equals(fields.image.text_format, "png", 3u));
    static const uint8_t png[] = {0x89u, 'P', 'N', 'G', '\r', '\n', 0x1au, '\n'};
    assert(equals(fields.image.data, png, sizeof png));
    assert(fields.audio.format_kind == LXMF_MEDIA_FORMAT_INTEGER &&
           fields.audio.integer_format == LXMF_AUDIO_OPUS_OGG);
    assert(equals(fields.audio.data, "OggS\0\2", 6u));

    /* Reconstructing the known values is byte-identical to pinned umsgpack. */
    uint8_t output[512];
    size_t output_length = 0u;
    assert(lxmf_standard_fields_merge(NULL, 0u, &fields, LXMF_STANDARD_ALL,
               0u, output, sizeof output, &output_length) == LXMF_OK);
    assert(output_length == sizeof lxmf_standard_fields_fixture);
    assert(memcmp(output, lxmf_standard_fields_fixture, output_length) == 0);

    /* Untouched unknown entry bytes remain exact while a known field changes. */
    static const uint8_t unknown[] = {
        0x82u, 0xccu, 0xfeu, 0x81u, 0xa1u, 'x', 0x93u, 0x01u, 0xc3u,
        0xc0u, LXMF_FIELD_RENDERER, LXMF_RENDERER_PLAIN};
    fields = (lxmf_standard_fields_t){
        .present_mask = LXMF_STANDARD_RENDERER,
        .renderer = LXMF_RENDERER_MICRON};
    assert(lxmf_standard_fields_merge(unknown, sizeof unknown, &fields,
               LXMF_STANDARD_RENDERER, 0u, output, sizeof output,
               &output_length) == LXMF_OK);
    static const uint8_t expected_unknown[] = {
        0x82u, 0xccu, 0xfeu, 0x81u, 0xa1u, 'x', 0x93u, 0x01u, 0xc3u,
        0xc0u, LXMF_FIELD_RENDERER, LXMF_RENDERER_MICRON};
    assert(output_length == sizeof expected_unknown &&
           memcmp(output, expected_unknown, output_length) == 0);
    static const uint8_t unknown_extension[] = {
        0x81u, 0xccu, 0xfeu, 0xc7u, 0x01u, 0x02u, 0xaau};
    assert(lxmf_standard_fields_parse(unknown_extension,
               sizeof unknown_extension, &fields) == LXMF_OK);
    assert(fields.present_mask == 0u);
    assert(lxmf_standard_fields_merge(unknown_extension,
               sizeof unknown_extension, &fields, 0u, 0u, output,
               sizeof output, &output_length) == LXMF_OK);
    assert(output_length == sizeof unknown_extension &&
           memcmp(output, unknown_extension, output_length) == 0);

    /* Malformed known shapes, duplicates, truncations and tight outputs fail. */
    static const uint8_t duplicate[] = {0x82u, 0x0fu, 0x00u, 0x0fu, 0x01u};
    static const uint8_t short_reply[] = {0x81u, 0x30u, 0xc4u, 0x01u, 0x00u};
    static const uint8_t bad_quote[] = {0x81u, 0x31u, 0xc4u, 0x01u, 0xffu};
    static const uint8_t bad_reaction[] = {
        0x81u, 0x40u, 0x81u, 0x00u, 0xc4u, 0x20u,
        0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u,
        0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u,
        0u, 0u, 0u, 0u, 0u, 0u};
    static const uint8_t bad_media[] = {
        0x81u, 0x06u, 0x92u, 0xc0u, 0xc4u, 0x00u};
    assert(lxmf_standard_fields_parse(duplicate, sizeof duplicate, &fields) ==
           LXMF_ERR_FORMAT);
    assert(lxmf_standard_fields_parse(short_reply, sizeof short_reply,
                                      &fields) == LXMF_ERR_FORMAT);
    assert(lxmf_standard_fields_parse(bad_quote, sizeof bad_quote, &fields) ==
           LXMF_ERR_FORMAT);
    assert(lxmf_standard_fields_parse(bad_reaction, sizeof bad_reaction,
                                      &fields) == LXMF_ERR_FORMAT);
    assert(lxmf_standard_fields_parse(bad_media, sizeof bad_media, &fields) ==
           LXMF_ERR_FORMAT);
    for (size_t i = 1u; i < sizeof lxmf_standard_fields_fixture; ++i)
        assert(lxmf_standard_fields_parse(lxmf_standard_fields_fixture, i,
                                          &fields) != LXMF_OK);
    fields = (lxmf_standard_fields_t){
        .present_mask = LXMF_STANDARD_RENDERER,
        .renderer = LXMF_RENDERER_PLAIN};
    assert(lxmf_standard_fields_merge(NULL, 0u, &fields,
               LXMF_STANDARD_RENDERER, 0u, output, 1u, &output_length) ==
           LXMF_ERR_BOUNDS);
    fields = (lxmf_standard_fields_t){
        .present_mask = LXMF_STANDARD_ATTACHMENTS,
        .attachment_count = LXMF_STANDARD_MAX_ATTACHMENTS + 1u};
    assert(lxmf_standard_fields_merge(NULL, 0u, &fields,
               LXMF_STANDARD_ATTACHMENTS, 0u, output, sizeof output,
               &output_length) == LXMF_ERR_ARGUMENT);

    /* Ticket composition preserves opaque extensions, replaces rather than
     * duplicates a ticket, and can remove it without rewriting other values. */
    static const uint8_t opaque[] = {0x81u, 0x7fu, 0xd4u, 0x01u, 0x42u};
    lxmf_ticket_field_t ticket = {.present = true, .expires_at = 1000u};
    for (size_t i = 0u; i < LXMF_TICKET_LENGTH; ++i) ticket.ticket[i] = (uint8_t)i;
    uint8_t with_ticket[128], replaced[128], removed[128];
    size_t with_length = 0u, replaced_length = 0u, removed_length = 0u;
    assert(lxmf_fields_merge_ticket(opaque, sizeof opaque, &ticket,
        with_ticket, sizeof with_ticket, &with_length) == LXMF_OK);
    assert(with_ticket[0] == 0x82u && memcmp(with_ticket + 1u, opaque + 1u, sizeof opaque - 1u) == 0);
    lxmf_ticket_field_t parsed_ticket;
    assert(lxmf_fields_parse_ticket(with_ticket, with_length, &parsed_ticket) == LXMF_OK);
    assert(parsed_ticket.present && parsed_ticket.expires_at == 1000u &&
           memcmp(parsed_ticket.ticket, ticket.ticket, LXMF_TICKET_LENGTH) == 0);
    uint8_t duplicate_ticket[128];
    memcpy(duplicate_ticket, with_ticket, with_length);
    duplicate_ticket[0] = 0x83u;
    size_t ticket_pair_length = with_length - sizeof opaque;
    memcpy(duplicate_ticket + with_length, with_ticket + sizeof opaque, ticket_pair_length);
    assert(lxmf_fields_merge_ticket(duplicate_ticket, with_length + ticket_pair_length, &ticket,
        output, sizeof output, &output_length) == LXMF_ERR_FORMAT);
    ticket.expires_at = UINT64_C(0x100000000);
    ticket.ticket[0] = 0xa5u;
    assert(lxmf_fields_merge_ticket(with_ticket, with_length, &ticket,
        replaced, sizeof replaced, &replaced_length) == LXMF_OK);
    assert(replaced[0] == 0x82u);
    assert(lxmf_fields_parse_ticket(replaced, replaced_length, &parsed_ticket) == LXMF_OK);
    assert(parsed_ticket.expires_at == ticket.expires_at && parsed_ticket.ticket[0] == 0xa5u);
    assert(lxmf_fields_merge_ticket(replaced, replaced_length, NULL,
        removed, sizeof removed, &removed_length) == LXMF_OK);
    assert(removed_length == sizeof opaque && memcmp(removed, opaque, sizeof opaque) == 0);
    assert(lxmf_fields_merge_ticket(NULL, 0u, &ticket, output, sizeof output, &output_length) == LXMF_OK);
    assert(lxmf_fields_merge_ticket(with_ticket, with_length, &ticket,
        with_ticket, sizeof with_ticket, &output_length) == LXMF_ERR_ARGUMENT);
    for (size_t cap = 0u; cap < with_length; ++cap) {
        ticket.expires_at = 1000u;
        assert(lxmf_fields_merge_ticket(opaque, sizeof opaque, &ticket,
            output, cap, &output_length) == LXMF_ERR_BOUNDS && output_length == 0u);
    }
    static const uint8_t malformed_ticket[] = {0x81u, LXMF_FIELD_TICKET, 0xc0u};
    assert(lxmf_fields_merge_ticket(malformed_ticket, sizeof malformed_ticket, &ticket,
        output, sizeof output, &output_length) == LXMF_ERR_FORMAT);
    ticket.present = false;
    assert(lxmf_fields_merge_ticket(NULL, 0u, &ticket, output, sizeof output, &output_length) == LXMF_ERR_ARGUMENT);

    uint8_t safe[32];
    size_t safe_length = 0u;
    static const uint8_t unsafe[] = "../../C:\\secret.txt";
    assert(lxmf_attachment_safe_name(
               (lxmf_slice_t){unsafe, sizeof unsafe - 1u}, safe, sizeof safe,
               &safe_length) == LXMF_OK);
    assert(safe_length == 10u && memcmp(safe, "secret.txt", 10u) == 0);
    static const uint8_t dots[] = "../..";
    assert(lxmf_attachment_safe_name(
               (lxmf_slice_t){dots, sizeof dots - 1u}, safe, sizeof safe,
               &safe_length) == LXMF_OK);
    assert(safe_length == 10u && memcmp(safe, "attachment", 10u) == 0);
    return 0;
}
