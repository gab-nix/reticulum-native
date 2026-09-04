#include "reticulum/lxmf_fields.h"

#include <limits.h>
#include <string.h>

#define FIELDS_MAX_DEPTH 32u
#define FIELDS_MAX_ITEMS 256u

typedef struct reader {
    const uint8_t *p;
    const uint8_t *end;
    size_t items;
} reader_t;

typedef struct writer {
    uint8_t *p;
    size_t left;
} writer_t;

static uint32_t load32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24u) | ((uint32_t)p[1] << 16u) |
           ((uint32_t)p[2] << 8u) | p[3];
}

static bool skip(reader_t *r, unsigned depth) {
    if (depth > FIELDS_MAX_DEPTH || r->p == r->end ||
        r->items++ >= FIELDS_MAX_ITEMS) return false;
    uint8_t code = *r->p++;
    uint64_t bytes = 0u;
    size_t children = 0u;
    if (code <= 0x7fu || code >= 0xe0u || code == 0xc0u ||
        code == 0xc2u || code == 0xc3u) return true;
    if ((code & 0xe0u) == 0xa0u) bytes = code & 31u;
    else if ((code & 0xf0u) == 0x90u) children = code & 15u;
    else if ((code & 0xf0u) == 0x80u) children = 2u * (code & 15u);
    else switch (code) {
        case 0xc4u: case 0xd9u:
            if ((size_t)(r->end - r->p) < 1u) return false;
            bytes = *r->p++; break;
        case 0xc5u: case 0xdau:
            if ((size_t)(r->end - r->p) < 2u) return false;
            bytes = ((uint64_t)r->p[0] << 8u) | r->p[1]; r->p += 2u; break;
        case 0xc6u: case 0xdbu:
            if ((size_t)(r->end - r->p) < 4u) return false;
            bytes = load32(r->p); r->p += 4u; break;
        case 0xc7u:
            if ((size_t)(r->end - r->p) < 1u) return false;
            bytes = (uint64_t)*r->p++ + 1u; break;
        case 0xc8u:
            if ((size_t)(r->end - r->p) < 2u) return false;
            bytes = (((uint64_t)r->p[0] << 8u) | r->p[1]) + 1u;
            r->p += 2u; break;
        case 0xc9u:
            if ((size_t)(r->end - r->p) < 4u) return false;
            bytes = (uint64_t)load32(r->p) + 1u; r->p += 4u; break;
        case 0xcau: bytes = 4u; break;
        case 0xcbu: bytes = 8u; break;
        case 0xccu: case 0xd0u: bytes = 1u; break;
        case 0xcdu: case 0xd1u: bytes = 2u; break;
        case 0xceu: case 0xd2u: bytes = 4u; break;
        case 0xcfu: case 0xd3u: bytes = 8u; break;
        case 0xd4u: bytes = 2u; break;
        case 0xd5u: bytes = 3u; break;
        case 0xd6u: bytes = 5u; break;
        case 0xd7u: bytes = 9u; break;
        case 0xd8u: bytes = 17u; break;
        case 0xdcu:
            if ((size_t)(r->end - r->p) < 2u) return false;
            children = ((size_t)r->p[0] << 8u) | r->p[1]; r->p += 2u; break;
        case 0xddu:
            if ((size_t)(r->end - r->p) < 4u) return false;
            children = load32(r->p); r->p += 4u; break;
        case 0xdeu:
            if ((size_t)(r->end - r->p) < 2u) return false;
            children = 2u * (((size_t)r->p[0] << 8u) | r->p[1]);
            r->p += 2u; break;
        case 0xdfu: {
            if ((size_t)(r->end - r->p) < 4u) return false;
            uint32_t count = load32(r->p); r->p += 4u;
            if (count > FIELDS_MAX_ITEMS / 2u) return false;
            children = 2u * (size_t)count; break;
        }
        default: return false;
    }
    if (bytes > (uint64_t)(r->end - r->p)) return false;
    r->p += (size_t)bytes;
    if (children > FIELDS_MAX_ITEMS) return false;
    for (size_t i = 0u; i < children; ++i)
        if (!skip(r, depth + 1u)) return false;
    return true;
}

static bool container(reader_t *r, uint8_t wanted, size_t *count) {
    if (r->p == r->end || r->items++ >= FIELDS_MAX_ITEMS) return false;
    uint8_t code = *r->p++;
    bool map = wanted == 5u;
    if ((!map && (code & 0xf0u) == 0x90u) ||
        (map && (code & 0xf0u) == 0x80u)) {
        *count = code & 15u;
        return true;
    }
    uint8_t code16 = map ? 0xdeu : 0xdcu;
    uint8_t code32 = map ? 0xdfu : 0xddu;
    if (code == code16) {
        if ((size_t)(r->end - r->p) < 2u) return false;
        *count = ((size_t)r->p[0] << 8u) | r->p[1]; r->p += 2u;
    } else if (code == code32) {
        if ((size_t)(r->end - r->p) < 4u) return false;
        uint32_t value = load32(r->p); r->p += 4u;
        *count = (size_t)value;
        if ((uint32_t)*count != value) return false;
    } else return false;
    return *count <= FIELDS_MAX_ITEMS;
}

static bool unsigned_value(reader_t *r, uint64_t *value) {
    if (r->p == r->end || r->items++ >= FIELDS_MAX_ITEMS) return false;
    uint8_t code = *r->p++;
    if (code <= 0x7fu) { *value = code; return true; }
    size_t width = 0u;
    if (code == 0xccu) width = 1u;
    else if (code == 0xcdu) width = 2u;
    else if (code == 0xceu) width = 4u;
    else if (code == 0xcfu) width = 8u;
    else return false;
    if ((size_t)(r->end - r->p) < width) return false;
    uint64_t result = 0u;
    for (size_t i = 0u; i < width; ++i) result = (result << 8u) | *r->p++;
    *value = result;
    return true;
}

static bool bytes_value(reader_t *r, uint8_t wanted, lxmf_slice_t *value) {
    if (r->p == r->end || r->items++ >= FIELDS_MAX_ITEMS) return false;
    uint8_t code = *r->p++;
    uint64_t length = 0u;
    if (wanted == 3u && code >= 0xa0u && code <= 0xbfu)
        length = code & 31u;
    else if ((wanted == 2u && code == 0xc4u) ||
             (wanted == 3u && code == 0xd9u)) {
        if ((size_t)(r->end - r->p) < 1u) return false;
        length = *r->p++;
    } else if ((wanted == 2u && code == 0xc5u) ||
               (wanted == 3u && code == 0xdau)) {
        if ((size_t)(r->end - r->p) < 2u) return false;
        length = ((uint64_t)r->p[0] << 8u) | r->p[1]; r->p += 2u;
    } else if ((wanted == 2u && code == 0xc6u) ||
               (wanted == 3u && code == 0xdbu)) {
        if ((size_t)(r->end - r->p) < 4u) return false;
        length = load32(r->p); r->p += 4u;
    } else return false;
    if (length > (uint64_t)(r->end - r->p)) return false;
    value->data = r->p;
    value->len = (size_t)length;
    r->p += value->len;
    return true;
}

static bool valid_utf8(const uint8_t *s, size_t length) {
    size_t i = 0u;
    while (i < length) {
        uint8_t first = s[i++];
        if (first < 0x80u) continue;
        size_t continuation;
        uint32_t codepoint, minimum;
        if (first >= 0xc2u && first <= 0xdfu) {
            continuation = 1u; codepoint = first & 31u; minimum = 0x80u;
        } else if (first >= 0xe0u && first <= 0xefu) {
            continuation = 2u; codepoint = first & 15u; minimum = 0x800u;
        } else if (first >= 0xf0u && first <= 0xf4u) {
            continuation = 3u; codepoint = first & 7u; minimum = 0x10000u;
        } else return false;
        if (continuation > length - i) return false;
        while (continuation-- != 0u) {
            uint8_t next = s[i++];
            if ((next & 0xc0u) != 0x80u) return false;
            codepoint = (codepoint << 6u) | (next & 0x3fu);
        }
        if (codepoint < minimum || codepoint > 0x10ffffu ||
            (codepoint >= 0xd800u && codepoint <= 0xdfffu)) return false;
    }
    return true;
}

static uint32_t mask_for_key(uint64_t key) {
    switch (key) {
        case LXMF_FIELD_RENDERER: return LXMF_STANDARD_RENDERER;
        case LXMF_FIELD_REPLY_TO: return LXMF_STANDARD_REPLY_TO;
        case LXMF_FIELD_REPLY_QUOTE: return LXMF_STANDARD_REPLY_QUOTE;
        case LXMF_FIELD_REACTION: return LXMF_STANDARD_REACTION;
        case LXMF_FIELD_THREAD: return LXMF_STANDARD_THREAD;
        case LXMF_FIELD_FILE_ATTACHMENTS: return LXMF_STANDARD_ATTACHMENTS;
        case LXMF_FIELD_IMAGE: return LXMF_STANDARD_IMAGE;
        case LXMF_FIELD_AUDIO: return LXMF_STANDARD_AUDIO;
        default: return 0u;
    }
}

static bool parse_hash(reader_t *r, uint8_t output[LXMF_MESSAGE_ID_LENGTH]) {
    lxmf_slice_t value;
    if (!bytes_value(r, 2u, &value) || value.len != LXMF_MESSAGE_ID_LENGTH)
        return false;
    memcpy(output, value.data, value.len);
    return true;
}

static bool parse_text_bin(reader_t *r, size_t maximum, lxmf_slice_t *value) {
    return bytes_value(r, 2u, value) && value->len <= maximum &&
           valid_utf8(value->data, value->len);
}

static bool parse_reaction(reader_t *r, lxmf_standard_fields_t *fields) {
    size_t count = 0u;
    if (!container(r, 5u, &count) || count != 2u) return false;
    bool target = false, content = false;
    for (size_t i = 0u; i < count; ++i) {
        uint64_t key = 0u;
        if (!unsigned_value(r, &key)) return false;
        if (key == LXMF_REACTION_TO && !target) {
            target = parse_hash(r, fields->reaction_to);
            if (!target) return false;
        } else if (key == LXMF_REACTION_CONTENT && !content) {
            content = parse_text_bin(r, LXMF_STANDARD_MAX_REACTION_BYTES,
                                     &fields->reaction_content);
            if (!content) return false;
        } else return false;
    }
    return target && content;
}

static bool parse_attachments(reader_t *r, lxmf_standard_fields_t *fields) {
    size_t count = 0u;
    if (!container(r, 4u, &count) || count > LXMF_STANDARD_MAX_ATTACHMENTS)
        return false;
    size_t total = 0u;
    for (size_t i = 0u; i < count; ++i) {
        size_t pair = 0u;
        lxmf_attachment_view_t *attachment = &fields->attachments[i];
        if (!container(r, 4u, &pair) || pair != 2u ||
            !bytes_value(r, 3u, &attachment->name) ||
            attachment->name.len == 0u ||
            attachment->name.len > LXMF_STANDARD_MAX_NAME_BYTES ||
            !valid_utf8(attachment->name.data, attachment->name.len) ||
            !bytes_value(r, 2u, &attachment->data) ||
            attachment->data.len > LXMF_STANDARD_MAX_MEDIA_BYTES ||
            attachment->data.len > LXMF_MAX_MESSAGE_SIZE - total)
            return false;
        total += attachment->data.len;
    }
    fields->attachment_count = count;
    return true;
}

static bool parse_media(reader_t *r, lxmf_media_view_t *media) {
    reader_t raw = *r;
    if (bytes_value(&raw, 2u, &media->data)) {
        if (media->data.len > LXMF_STANDARD_MAX_MEDIA_BYTES) return false;
        media->format_kind = LXMF_MEDIA_FORMAT_NONE;
        *r = raw;
        return true;
    }
    size_t count = 0u;
    if (!container(r, 4u, &count) || count != 2u) return false;
    reader_t format = *r;
    uint64_t integer = 0u;
    if (unsigned_value(&format, &integer)) {
        if (integer > UINT8_MAX) return false;
        media->format_kind = LXMF_MEDIA_FORMAT_INTEGER;
        media->integer_format = (uint8_t)integer;
        *r = format;
    } else {
        media->format_kind = LXMF_MEDIA_FORMAT_TEXT;
        if (!bytes_value(r, 3u, &media->text_format) ||
            media->text_format.len == 0u ||
            media->text_format.len > LXMF_STANDARD_MAX_FORMAT_BYTES ||
            !valid_utf8(media->text_format.data, media->text_format.len))
            return false;
    }
    return bytes_value(r, 2u, &media->data) &&
           media->data.len <= LXMF_STANDARD_MAX_MEDIA_BYTES;
}

static lxmf_status_t parse_value(reader_t *r, uint32_t mask,
                                 lxmf_standard_fields_t *fields) {
    uint64_t value = 0u;
    switch (mask) {
        case LXMF_STANDARD_RENDERER:
            if (!unsigned_value(r, &value) || value > LXMF_RENDERER_BBCODE)
                return LXMF_ERR_FORMAT;
            fields->renderer = (uint8_t)value;
            break;
        case LXMF_STANDARD_REPLY_TO:
            if (!parse_hash(r, fields->reply_to)) return LXMF_ERR_FORMAT;
            break;
        case LXMF_STANDARD_REPLY_QUOTE:
            if (!parse_text_bin(r, LXMF_STANDARD_MAX_QUOTE_BYTES,
                                &fields->reply_quote)) return LXMF_ERR_FORMAT;
            break;
        case LXMF_STANDARD_REACTION:
            if (!parse_reaction(r, fields)) return LXMF_ERR_FORMAT;
            break;
        case LXMF_STANDARD_THREAD:
            if (!parse_hash(r, fields->thread)) return LXMF_ERR_FORMAT;
            break;
        case LXMF_STANDARD_ATTACHMENTS:
            if (!parse_attachments(r, fields)) return LXMF_ERR_FORMAT;
            break;
        case LXMF_STANDARD_IMAGE:
            if (!parse_media(r, &fields->image)) return LXMF_ERR_FORMAT;
            break;
        case LXMF_STANDARD_AUDIO:
            if (!parse_media(r, &fields->audio)) return LXMF_ERR_FORMAT;
            break;
        default: return LXMF_ERR_ARGUMENT;
    }
    fields->present_mask |= mask;
    return LXMF_OK;
}

lxmf_status_t lxmf_standard_fields_parse(const uint8_t *fields,
                                         size_t fields_length,
                                         lxmf_standard_fields_t *standard) {
    if (fields == NULL || fields_length == 0u || standard == NULL)
        return LXMF_ERR_ARGUMENT;
    if (fields_length > LXMF_MAX_MESSAGE_SIZE) return LXMF_ERR_BOUNDS;
    memset(standard, 0, sizeof *standard);
    reader_t reader = {fields, fields + fields_length, 0u};
    size_t count = 0u;
    if (!container(&reader, 5u, &count)) return LXMF_ERR_FORMAT;
    for (size_t i = 0u; i < count; ++i) {
        reader_t key_reader = reader;
        uint64_t key = UINT64_MAX;
        bool integer_key = unsigned_value(&key_reader, &key);
        if (!skip(&reader, 0u)) return LXMF_ERR_FORMAT;
        uint32_t mask = integer_key ? mask_for_key(key) : 0u;
        if (mask == 0u) {
            if (!skip(&reader, 0u)) return LXMF_ERR_FORMAT;
        } else {
            if ((standard->present_mask & mask) != 0u)
                return LXMF_ERR_FORMAT;
            lxmf_status_t status = parse_value(&reader, mask, standard);
            if (status != LXMF_OK) return status;
        }
    }
    return reader.p == reader.end ? LXMF_OK : LXMF_ERR_FORMAT;
}

static bool put(writer_t *w, const void *data, size_t length) {
    if (length > w->left) return false;
    if (length != 0u) memcpy(w->p, data, length);
    w->p += length;
    w->left -= length;
    return true;
}

static bool put_head(writer_t *w, uint8_t major, uint64_t value) {
    uint8_t bytes[9];
    size_t length = 1u;
    if (major == 0u) {
        if (value <= 0x7fu) bytes[0] = (uint8_t)value;
        else if (value <= UINT8_MAX) {
            bytes[0] = 0xccu; bytes[1] = (uint8_t)value; length = 2u;
        } else if (value <= UINT16_MAX) {
            bytes[0] = 0xcdu; length = 3u;
        } else if (value <= UINT32_MAX) {
            bytes[0] = 0xceu; length = 5u;
        } else { bytes[0] = 0xcfu; length = 9u; }
    } else if (major == 2u || major == 3u) {
        if (major == 3u && value <= 31u) bytes[0] = 0xa0u | (uint8_t)value;
        else if (value <= UINT8_MAX) {
            bytes[0] = major == 2u ? 0xc4u : 0xd9u; length = 2u;
        } else if (value <= UINT16_MAX) {
            bytes[0] = major == 2u ? 0xc5u : 0xdau; length = 3u;
        } else if (value <= UINT32_MAX) {
            bytes[0] = major == 2u ? 0xc6u : 0xdbu; length = 5u;
        } else return false;
    } else if (major == 4u || major == 5u) {
        if (value <= 15u)
            bytes[0] = (major == 4u ? 0x90u : 0x80u) | (uint8_t)value;
        else if (value <= UINT16_MAX) {
            bytes[0] = major == 4u ? 0xdcu : 0xdeu; length = 3u;
        } else if (value <= UINT32_MAX) {
            bytes[0] = major == 4u ? 0xddu : 0xdfu; length = 5u;
        } else return false;
    } else return false;
    if (length == 2u && major != 0u) {
        bytes[1] = (uint8_t)value;
    } else if (length == 3u) {
        bytes[1] = (uint8_t)(value >> 8u); bytes[2] = (uint8_t)value;
    } else if (length == 5u) {
        for (size_t i = 0u; i < 4u; ++i)
            bytes[1u + i] = (uint8_t)(value >> (24u - 8u * i));
    } else if (length == 9u) {
        for (size_t i = 0u; i < 8u; ++i)
            bytes[1u + i] = (uint8_t)(value >> (56u - 8u * i));
    }
    return put(w, bytes, length);
}

static bool put_slice(writer_t *w, uint8_t major, lxmf_slice_t value) {
    return (value.len == 0u || value.data != NULL) &&
           put_head(w, major, value.len) && put(w, value.data, value.len);
}

static bool put_hash(writer_t *w,
                     const uint8_t value[LXMF_MESSAGE_ID_LENGTH]) {
    return put_head(w, 2u, LXMF_MESSAGE_ID_LENGTH) &&
           put(w, value, LXMF_MESSAGE_ID_LENGTH);
}

static bool put_media(writer_t *w, const lxmf_media_view_t *media) {
    if (media->data.len > LXMF_STANDARD_MAX_MEDIA_BYTES ||
        (media->data.len != 0u && media->data.data == NULL)) return false;
    if (media->format_kind == LXMF_MEDIA_FORMAT_NONE)
        return put_slice(w, 2u, media->data);
    if (!put_head(w, 4u, 2u)) return false;
    if (media->format_kind == LXMF_MEDIA_FORMAT_INTEGER) {
        if (!put_head(w, 0u, media->integer_format)) return false;
    } else if (media->format_kind == LXMF_MEDIA_FORMAT_TEXT) {
        if (media->text_format.len == 0u ||
            media->text_format.len > LXMF_STANDARD_MAX_FORMAT_BYTES ||
            media->text_format.data == NULL ||
            !valid_utf8(media->text_format.data, media->text_format.len) ||
            !put_slice(w, 3u, media->text_format)) return false;
    } else return false;
    return put_slice(w, 2u, media->data);
}

static bool put_value(writer_t *w, uint32_t mask,
                      const lxmf_standard_fields_t *fields) {
    switch (mask) {
        case LXMF_STANDARD_RENDERER:
            return fields->renderer <= LXMF_RENDERER_BBCODE &&
                   put_head(w, 0u, fields->renderer);
        case LXMF_STANDARD_REPLY_TO: return put_hash(w, fields->reply_to);
        case LXMF_STANDARD_REPLY_QUOTE:
            return fields->reply_quote.len <= LXMF_STANDARD_MAX_QUOTE_BYTES &&
                   (fields->reply_quote.len == 0u ||
                    fields->reply_quote.data != NULL) &&
                   valid_utf8(fields->reply_quote.data,
                              fields->reply_quote.len) &&
                   put_slice(w, 2u, fields->reply_quote);
        case LXMF_STANDARD_REACTION:
            return fields->reaction_content.len <=
                       LXMF_STANDARD_MAX_REACTION_BYTES &&
                   (fields->reaction_content.len == 0u ||
                    fields->reaction_content.data != NULL) &&
                   valid_utf8(fields->reaction_content.data,
                              fields->reaction_content.len) &&
                   put_head(w, 5u, 2u) &&
                   put_head(w, 0u, LXMF_REACTION_TO) &&
                   put_hash(w, fields->reaction_to) &&
                   put_head(w, 0u, LXMF_REACTION_CONTENT) &&
                   put_slice(w, 2u, fields->reaction_content);
        case LXMF_STANDARD_THREAD: return put_hash(w, fields->thread);
        case LXMF_STANDARD_ATTACHMENTS:
            if (fields->attachment_count > LXMF_STANDARD_MAX_ATTACHMENTS ||
                !put_head(w, 4u, fields->attachment_count)) return false;
            for (size_t i = 0u; i < fields->attachment_count; ++i) {
                const lxmf_attachment_view_t *attachment =
                    &fields->attachments[i];
                if (attachment->name.len == 0u ||
                    attachment->name.len > LXMF_STANDARD_MAX_NAME_BYTES ||
                    attachment->name.data == NULL ||
                    attachment->data.len > LXMF_STANDARD_MAX_MEDIA_BYTES ||
                    (attachment->data.len != 0u &&
                     attachment->data.data == NULL) ||
                    !valid_utf8(attachment->name.data, attachment->name.len) ||
                    !put_head(w, 4u, 2u) ||
                    !put_slice(w, 3u, attachment->name) ||
                    !put_slice(w, 2u, attachment->data)) return false;
            }
            return true;
        case LXMF_STANDARD_IMAGE: return put_media(w, &fields->image);
        case LXMF_STANDARD_AUDIO: return put_media(w, &fields->audio);
        default: return false;
    }
}

static size_t bit_count(uint32_t value) {
    size_t count = 0u;
    while (value != 0u) { count += value & 1u; value >>= 1u; }
    return count;
}

static bool valid_media_value(const lxmf_media_view_t *media) {
    if (media->data.len > LXMF_STANDARD_MAX_MEDIA_BYTES ||
        (media->data.len != 0u && media->data.data == NULL)) return false;
    if (media->format_kind == LXMF_MEDIA_FORMAT_NONE ||
        media->format_kind == LXMF_MEDIA_FORMAT_INTEGER) return true;
    return media->format_kind == LXMF_MEDIA_FORMAT_TEXT &&
           media->text_format.data != NULL &&
           media->text_format.len != 0u &&
           media->text_format.len <= LXMF_STANDARD_MAX_FORMAT_BYTES &&
           valid_utf8(media->text_format.data, media->text_format.len);
}

static bool valid_replacement(const lxmf_standard_fields_t *fields,
                              uint32_t mask) {
    if (mask == LXMF_STANDARD_RENDERER)
        return fields->renderer <= LXMF_RENDERER_BBCODE;
    if (mask == LXMF_STANDARD_REPLY_TO || mask == LXMF_STANDARD_THREAD)
        return true;
    if (mask == LXMF_STANDARD_REPLY_QUOTE)
        return fields->reply_quote.len <= LXMF_STANDARD_MAX_QUOTE_BYTES &&
               (fields->reply_quote.len == 0u ||
                fields->reply_quote.data != NULL) &&
               valid_utf8(fields->reply_quote.data, fields->reply_quote.len);
    if (mask == LXMF_STANDARD_REACTION)
        return fields->reaction_content.len <=
                   LXMF_STANDARD_MAX_REACTION_BYTES &&
               (fields->reaction_content.len == 0u ||
                fields->reaction_content.data != NULL) &&
               valid_utf8(fields->reaction_content.data,
                          fields->reaction_content.len);
    if (mask == LXMF_STANDARD_IMAGE) return valid_media_value(&fields->image);
    if (mask == LXMF_STANDARD_AUDIO) return valid_media_value(&fields->audio);
    if (mask != LXMF_STANDARD_ATTACHMENTS ||
        fields->attachment_count > LXMF_STANDARD_MAX_ATTACHMENTS) return false;
    size_t total = 0u;
    for (size_t i = 0u; i < fields->attachment_count; ++i) {
        const lxmf_attachment_view_t *attachment = &fields->attachments[i];
        if (attachment->name.data == NULL || attachment->name.len == 0u ||
            attachment->name.len > LXMF_STANDARD_MAX_NAME_BYTES ||
            !valid_utf8(attachment->name.data, attachment->name.len) ||
            attachment->data.len > LXMF_STANDARD_MAX_MEDIA_BYTES ||
            (attachment->data.len != 0u && attachment->data.data == NULL) ||
            attachment->data.len > LXMF_MAX_MESSAGE_SIZE - total) return false;
        total += attachment->data.len;
    }
    return true;
}

lxmf_status_t lxmf_standard_fields_merge(
    const uint8_t *existing_fields, size_t existing_length,
    const lxmf_standard_fields_t *standard, uint32_t replace_mask,
    uint32_t remove_mask, uint8_t *output, size_t output_capacity,
    size_t *output_length) {
    if ((existing_length != 0u && existing_fields == NULL) ||
        standard == NULL || output == NULL || output_length == NULL ||
        existing_length > LXMF_MAX_MESSAGE_SIZE ||
        (replace_mask & remove_mask) != 0u ||
        ((replace_mask | remove_mask) & ~(uint32_t)LXMF_STANDARD_ALL) != 0u ||
        (standard->present_mask & replace_mask) != replace_mask)
        return LXMF_ERR_ARGUMENT;
    *output_length = 0u;
    for (size_t bit = 0u; bit < 8u; ++bit) {
        uint32_t mask = UINT32_C(1) << bit;
        if ((replace_mask & mask) != 0u &&
            !valid_replacement(standard, mask)) return LXMF_ERR_ARGUMENT;
    }
    static const uint8_t empty[] = {0x80u};
    if (existing_length == 0u) {
        existing_fields = empty;
        existing_length = sizeof empty;
    }
    lxmf_standard_fields_t parsed;
    lxmf_status_t status = lxmf_standard_fields_parse(
        existing_fields, existing_length, &parsed);
    if (status != LXMF_OK) return status;
    reader_t reader = {existing_fields, existing_fields + existing_length, 0u};
    size_t old_count = 0u, skipped = 0u;
    if (!container(&reader, 5u, &old_count)) return LXMF_ERR_FORMAT;
    uint32_t changed = replace_mask | remove_mask;
    for (size_t i = 0u; i < old_count; ++i) {
        reader_t key_reader = reader;
        uint64_t key = UINT64_MAX;
        bool integer = unsigned_value(&key_reader, &key);
        if (!skip(&reader, 0u) || !skip(&reader, 0u))
            return LXMF_ERR_FORMAT;
        if (integer && (mask_for_key(key) & changed) != 0u) skipped++;
    }
    size_t new_count = old_count - skipped + bit_count(replace_mask);
    if (new_count > FIELDS_MAX_ITEMS) return LXMF_ERR_BOUNDS;
    size_t bounded_capacity = output_capacity < LXMF_MAX_MESSAGE_SIZE
                                  ? output_capacity
                                  : LXMF_MAX_MESSAGE_SIZE;
    writer_t writer = {output, bounded_capacity};
    if (!put_head(&writer, 5u, new_count)) return LXMF_ERR_BOUNDS;
    reader = (reader_t){existing_fields, existing_fields + existing_length, 0u};
    if (!container(&reader, 5u, &old_count)) return LXMF_ERR_FORMAT;
    for (size_t i = 0u; i < old_count; ++i) {
        const uint8_t *start = reader.p;
        reader_t key_reader = reader;
        uint64_t key = UINT64_MAX;
        bool integer = unsigned_value(&key_reader, &key);
        if (!skip(&reader, 0u) || !skip(&reader, 0u))
            return LXMF_ERR_FORMAT;
        if ((!integer || (mask_for_key(key) & changed) == 0u) &&
            !put(&writer, start, (size_t)(reader.p - start)))
            return LXMF_ERR_BOUNDS;
    }
    static const uint8_t keys[] = {
        LXMF_FIELD_RENDERER, LXMF_FIELD_REPLY_TO, LXMF_FIELD_REPLY_QUOTE,
        LXMF_FIELD_REACTION, LXMF_FIELD_THREAD, LXMF_FIELD_FILE_ATTACHMENTS,
        LXMF_FIELD_IMAGE, LXMF_FIELD_AUDIO};
    for (size_t bit = 0u; bit < sizeof keys; ++bit) {
        uint32_t mask = UINT32_C(1) << bit;
        if ((replace_mask & mask) != 0u &&
            (!put_head(&writer, 0u, keys[bit]) ||
             !put_value(&writer, mask, standard)))
            return LXMF_ERR_BOUNDS;
    }
    *output_length = bounded_capacity - writer.left;
    return LXMF_OK;
}

lxmf_status_t lxmf_fields_merge_ticket(
    const uint8_t *existing_fields, size_t existing_length,
    const lxmf_ticket_field_t *ticket, uint8_t *output, size_t output_capacity,
    size_t *output_length) {
    if (output_length != NULL) *output_length = 0u;
    if ((existing_length != 0u && existing_fields == NULL) || output == NULL ||
        output_length == NULL || existing_length > LXMF_MAX_MESSAGE_SIZE ||
        (ticket != NULL && !ticket->present)) return LXMF_ERR_ARGUMENT;
    size_t capacity = output_capacity < LXMF_MAX_MESSAGE_SIZE ? output_capacity : LXMF_MAX_MESSAGE_SIZE;
    uintptr_t in = (uintptr_t)existing_fields, out = (uintptr_t)output;
    if (existing_length != 0u && capacity != 0u &&
        (out >= in ? out - in < existing_length : in - out < capacity))
        return LXMF_ERR_ARGUMENT;
    lxmf_ticket_field_t replacement = {0}, previous;
    if (ticket != NULL) replacement = *ticket;
    static const uint8_t empty[] = {0x80u};
    if (existing_length == 0u) { existing_fields = empty; existing_length = sizeof empty; }
    lxmf_status_t status = lxmf_fields_parse_ticket(existing_fields, existing_length, &previous);
    if (status != LXMF_OK) return status;
    reader_t reader = {existing_fields, existing_fields + existing_length, 0u};
    size_t count = 0u;
    if (!container(&reader, 5u, &count)) return LXMF_ERR_FORMAT;
    size_t new_count = count - (previous.present ? 1u : 0u) + (ticket != NULL ? 1u : 0u);
    if (new_count > FIELDS_MAX_ITEMS) return LXMF_ERR_BOUNDS;
    writer_t writer = {output, capacity};
    if (!put_head(&writer, 5u, new_count)) return LXMF_ERR_BOUNDS;
    for (size_t i = 0u; i < count; ++i) {
        const uint8_t *start = reader.p;
        reader_t key_reader = reader;
        uint64_t key = UINT64_MAX;
        bool integer = unsigned_value(&key_reader, &key);
        if (!skip(&reader, 0u) || !skip(&reader, 0u)) return LXMF_ERR_FORMAT;
        if ((!integer || key != LXMF_FIELD_TICKET) &&
            !put(&writer, start, (size_t)(reader.p - start))) return LXMF_ERR_BOUNDS;
    }
    if (ticket != NULL &&
        (!put_head(&writer, 0u, LXMF_FIELD_TICKET) ||
         !put_head(&writer, 4u, 2u) || !put_head(&writer, 0u, replacement.expires_at) ||
         !put_slice(&writer, 2u, (lxmf_slice_t){replacement.ticket, LXMF_TICKET_LENGTH})))
        return LXMF_ERR_BOUNDS;
    *output_length = capacity - writer.left;
    return LXMF_OK;
}

lxmf_status_t lxmf_attachment_safe_name(lxmf_slice_t name, uint8_t *output,
                                        size_t output_capacity,
                                        size_t *output_length) {
    if ((name.len != 0u && name.data == NULL) || output == NULL ||
        output_length == NULL || name.len > LXMF_STANDARD_MAX_NAME_BYTES ||
        !valid_utf8(name.data, name.len)) return LXMF_ERR_ARGUMENT;
    size_t start = 0u;
    for (size_t i = 0u; i < name.len; ++i)
        if (name.data[i] == '/' || name.data[i] == '\\' || name.data[i] == ':')
            start = i + 1u;
    while (start < name.len && name.data[start] == '.') start++;
    size_t written = 0u;
    for (size_t i = start; i < name.len; ++i) {
        uint8_t byte = name.data[i];
        if (byte < 0x20u || byte == 0x7fu) continue;
        if (written >= output_capacity) return LXMF_ERR_BOUNDS;
        output[written++] = byte;
    }
    static const uint8_t fallback[] = "attachment";
    if (written == 0u) {
        if (sizeof fallback - 1u > output_capacity) return LXMF_ERR_BOUNDS;
        memcpy(output, fallback, sizeof fallback - 1u);
        written = sizeof fallback - 1u;
    }
    *output_length = written;
    return LXMF_OK;
}
