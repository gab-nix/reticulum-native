#ifndef RETICULUM_LXMF_FIELDS_H
#define RETICULUM_LXMF_FIELDS_H

#include "reticulum/lxmf.h"

#ifdef __cplusplus
extern "C" {
#endif

#define LXMF_FIELD_FILE_ATTACHMENTS 0x05u
#define LXMF_FIELD_IMAGE 0x06u
#define LXMF_FIELD_AUDIO 0x07u
#define LXMF_FIELD_THREAD 0x08u
#define LXMF_FIELD_RENDERER 0x0fu
#define LXMF_FIELD_REPLY_TO 0x30u
#define LXMF_FIELD_REPLY_QUOTE 0x31u
#define LXMF_FIELD_REACTION 0x40u

#define LXMF_REACTION_TO 0x00u
#define LXMF_REACTION_CONTENT 0x01u

#define LXMF_RENDERER_PLAIN 0x00u
#define LXMF_RENDERER_MICRON 0x01u
#define LXMF_RENDERER_MARKDOWN 0x02u
#define LXMF_RENDERER_BBCODE 0x03u

#define LXMF_AUDIO_CODEC2_450PWB 0x01u
#define LXMF_AUDIO_CODEC2_450 0x02u
#define LXMF_AUDIO_CODEC2_700C 0x03u
#define LXMF_AUDIO_CODEC2_1200 0x04u
#define LXMF_AUDIO_CODEC2_1300 0x05u
#define LXMF_AUDIO_CODEC2_1400 0x06u
#define LXMF_AUDIO_CODEC2_1600 0x07u
#define LXMF_AUDIO_CODEC2_2400 0x08u
#define LXMF_AUDIO_CODEC2_3200 0x09u
#define LXMF_AUDIO_OPUS_OGG 0x10u
#define LXMF_AUDIO_OPUS_LBW 0x11u
#define LXMF_AUDIO_OPUS_MBW 0x12u
#define LXMF_AUDIO_OPUS_PTT 0x13u
#define LXMF_AUDIO_OPUS_RT_HDX 0x14u
#define LXMF_AUDIO_OPUS_RT_FDX 0x15u
#define LXMF_AUDIO_OPUS_STANDARD 0x16u
#define LXMF_AUDIO_OPUS_HQ 0x17u
#define LXMF_AUDIO_OPUS_BROADCAST 0x18u
#define LXMF_AUDIO_OPUS_LOSSLESS 0x19u
#define LXMF_AUDIO_CUSTOM 0xffu

#define LXMF_STANDARD_MAX_ATTACHMENTS 16u
#define LXMF_STANDARD_MAX_NAME_BYTES 255u
#define LXMF_STANDARD_MAX_QUOTE_BYTES 4096u
#define LXMF_STANDARD_MAX_REACTION_BYTES 256u
#define LXMF_STANDARD_MAX_FORMAT_BYTES 31u
#define LXMF_STANDARD_MAX_MEDIA_BYTES (4u * 1024u * 1024u)

typedef enum lxmf_standard_field_mask {
    LXMF_STANDARD_RENDERER = 1u << 0,
    LXMF_STANDARD_REPLY_TO = 1u << 1,
    LXMF_STANDARD_REPLY_QUOTE = 1u << 2,
    LXMF_STANDARD_REACTION = 1u << 3,
    LXMF_STANDARD_THREAD = 1u << 4,
    LXMF_STANDARD_ATTACHMENTS = 1u << 5,
    LXMF_STANDARD_IMAGE = 1u << 6,
    LXMF_STANDARD_AUDIO = 1u << 7,
    LXMF_STANDARD_ALL = 0xffu
} lxmf_standard_field_mask_t;

typedef struct lxmf_attachment_view {
    lxmf_slice_t name;
    lxmf_slice_t data;
} lxmf_attachment_view_t;

typedef enum lxmf_media_format_kind {
    LXMF_MEDIA_FORMAT_NONE = 0,
    LXMF_MEDIA_FORMAT_INTEGER,
    LXMF_MEDIA_FORMAT_TEXT
} lxmf_media_format_kind_t;

typedef struct lxmf_media_view {
    lxmf_media_format_kind_t format_kind;
    uint8_t integer_format;
    lxmf_slice_t text_format;
    lxmf_slice_t data;
} lxmf_media_view_t;

/* All slices borrow the caller's fields buffer. Full original field bytes,
 * including unknown entries, remain owned by the caller. */
typedef struct lxmf_standard_fields {
    uint32_t present_mask;
    uint8_t renderer;
    uint8_t reply_to[LXMF_MESSAGE_ID_LENGTH];
    lxmf_slice_t reply_quote;
    uint8_t reaction_to[LXMF_MESSAGE_ID_LENGTH];
    lxmf_slice_t reaction_content;
    uint8_t thread[LXMF_MESSAGE_ID_LENGTH];
    lxmf_attachment_view_t attachments[LXMF_STANDARD_MAX_ATTACHMENTS];
    size_t attachment_count;
    lxmf_media_view_t image;
    lxmf_media_view_t audio;
} lxmf_standard_fields_t;

/* Parses pinned LXMF 1.1.0 standard field shapes. Duplicate standard fields,
 * malformed known values, excessive values and trailing bytes are rejected.
 * Structurally valid unknown entries are skipped without altering input. */
lxmf_status_t lxmf_standard_fields_parse(const uint8_t *fields,
                                         size_t fields_length,
                                         lxmf_standard_fields_t *standard);

/* Produces a complete MessagePack fields map. Existing entries not selected
 * by replace_mask or remove_mask are copied byte-for-byte, including unknown
 * keys and values. Replacement values come from standard and must have their
 * present_mask bits set. Output must not overlap existing_fields. */
lxmf_status_t lxmf_standard_fields_merge(
    const uint8_t *existing_fields, size_t existing_length,
    const lxmf_standard_fields_t *standard, uint32_t replace_mask,
    uint32_t remove_mask, uint8_t *output, size_t output_capacity,
    size_t *output_length);

/* Adds/replaces FIELD_TICKET, or removes it when ticket is NULL. A supplied
 * ticket must have present=true. Other entries are preserved byte-for-byte.
 * Empty input means an empty map. Output may not overlap existing_fields.
 * Compose fields before signing: changing a ticket changes the message ID. */
lxmf_status_t lxmf_fields_merge_ticket(
    const uint8_t *existing_fields, size_t existing_length,
    const lxmf_ticket_field_t *ticket, uint8_t *output, size_t output_capacity,
    size_t *output_length);

/* Derives a display/save candidate without path separators, control bytes,
 * drive/URI prefixes or leading dots. This does not write any file. */
lxmf_status_t lxmf_attachment_safe_name(lxmf_slice_t name, uint8_t *output,
                                        size_t output_capacity,
                                        size_t *output_length);

#ifdef __cplusplus
}
#endif
#endif
