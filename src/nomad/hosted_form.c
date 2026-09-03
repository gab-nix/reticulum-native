#include "reticulum/hosted_form.h"

#include <string.h>

static bool take(const uint8_t **cursor, size_t *remaining, size_t amount,
                 const uint8_t **value) {
    if (amount > *remaining) return false;
    if (value != NULL) *value = *cursor;
    *cursor += amount;
    *remaining -= amount;
    return true;
}

static bool span(const uint8_t **cursor, size_t *remaining, uint8_t tag,
                 bool binary, const uint8_t **value, size_t *length) {
    uint32_t wide = 0U;
    if (!binary && (tag & 0xe0U) == 0xa0U) wide = tag & 0x1fU;
    else if ((!binary && tag == 0xd9U) || (binary && tag == 0xc4U)) {
        const uint8_t *size;
        if (!take(cursor, remaining, 1U, &size)) return false;
        wide = size[0];
    } else if ((!binary && tag == 0xdaU) || (binary && tag == 0xc5U)) {
        const uint8_t *size;
        if (!take(cursor, remaining, 2U, &size)) return false;
        wide = ((uint32_t)size[0] << 8U) | size[1];
    } else if ((!binary && tag == 0xdbU) || (binary && tag == 0xc6U)) {
        const uint8_t *size;
        if (!take(cursor, remaining, 4U, &size)) return false;
        wide = ((uint32_t)size[0] << 24U) | ((uint32_t)size[1] << 16U) |
               ((uint32_t)size[2] << 8U) | size[3];
    } else return false;
    *length = wide;
    return take(cursor, remaining, *length, value);
}

static bool skip(const uint8_t **cursor, size_t *remaining, unsigned depth) {
    if (depth > 8U || *remaining == 0U) return false;
    uint8_t tag = *(*cursor)++;
    --*remaining;
    const uint8_t *ignored;
    size_t length;
    if ((tag & 0x80U) == 0U || tag >= 0xe0U || tag == 0xc0U ||
        tag == 0xc2U || tag == 0xc3U) return true;
    if ((tag & 0xe0U) == 0xa0U || tag == 0xd9U || tag == 0xdaU || tag == 0xdbU)
        return span(cursor, remaining, tag, false, &ignored, &length);
    if (tag == 0xc4U || tag == 0xc5U || tag == 0xc6U)
        return span(cursor, remaining, tag, true, &ignored, &length);
    size_t scalar = tag == 0xcaU ? 4U : tag == 0xcbU ? 8U :
                    (tag == 0xccU || tag == 0xd0U) ? 1U :
                    (tag == 0xcdU || tag == 0xd1U) ? 2U :
                    (tag == 0xceU || tag == 0xd2U) ? 4U :
                    (tag == 0xcfU || tag == 0xd3U) ? 8U : 0U;
    if (scalar != 0U) return take(cursor, remaining, scalar, NULL);
    uint32_t items;
    bool map = false;
    if ((tag & 0xf0U) == 0x80U) { items = tag & 0x0fU; map = true; }
    else if ((tag & 0xf0U) == 0x90U) items = tag & 0x0fU;
    else if (tag == 0xdcU || tag == 0xdeU) {
        const uint8_t *size;
        if (!take(cursor, remaining, 2U, &size)) return false;
        items = ((uint32_t)size[0] << 8U) | size[1];
        map = tag == 0xdeU;
    } else if (tag == 0xddU || tag == 0xdfU) {
        const uint8_t *size;
        if (!take(cursor, remaining, 4U, &size)) return false;
        items = ((uint32_t)size[0] << 24U) | ((uint32_t)size[1] << 16U) |
                ((uint32_t)size[2] << 8U) | size[3];
        map = tag == 0xdfU;
    } else return false;
    if (items > 1024U || (map && items > 512U)) return false;
    if (map) items *= 2U;
    for (uint32_t i = 0U; i < items; ++i)
        if (!skip(cursor, remaining, depth + 1U)) return false;
    return true;
}

static uint64_t unsigned_be(const uint8_t *bytes, size_t length) {
    uint64_t value = 0U;
    for (size_t i = 0U; i < length; ++i) value = (value << 8U) | bytes[i];
    return value;
}

static rns_status_t decode_value(const uint8_t **cursor, size_t *remaining,
                                 rns_hosted_form_entry_t *entry) {
    if (*remaining == 0U) return RNS_ERROR_PROTOCOL;
    uint8_t tag = *(*cursor)++;
    --*remaining;
    if ((tag & 0x80U) == 0U) {
        entry->kind = RNS_HOSTED_FORM_UNSIGNED;
        entry->unsigned_value = tag;
        return RNS_OK;
    }
    if (tag >= 0xe0U) {
        entry->kind = RNS_HOSTED_FORM_SIGNED;
        entry->signed_value = (int8_t)tag;
        return RNS_OK;
    }
    if ((tag & 0xe0U) == 0xa0U || tag == 0xd9U || tag == 0xdaU || tag == 0xdbU) {
        entry->kind = RNS_HOSTED_FORM_STRING;
        if (!span(cursor, remaining, tag, false, &entry->bytes,
                  &entry->bytes_length)) return RNS_ERROR_PROTOCOL;
        return entry->bytes_length <= RNS_HOSTED_FORM_VALUE_MAX
                   ? RNS_OK : RNS_ERROR_OVERFLOW;
    }
    if (tag == 0xc4U || tag == 0xc5U || tag == 0xc6U) {
        entry->kind = RNS_HOSTED_FORM_BINARY;
        if (!span(cursor, remaining, tag, true, &entry->bytes,
                  &entry->bytes_length)) return RNS_ERROR_PROTOCOL;
        return entry->bytes_length <= RNS_HOSTED_FORM_VALUE_MAX
                   ? RNS_OK : RNS_ERROR_OVERFLOW;
    }
    if (tag == 0xc0U) { entry->kind = RNS_HOSTED_FORM_NIL; return RNS_OK; }
    if (tag == 0xc2U || tag == 0xc3U) {
        entry->kind = RNS_HOSTED_FORM_BOOL;
        entry->bool_value = tag == 0xc3U;
        return RNS_OK;
    }
    size_t width = tag == 0xccU || tag == 0xd0U ? 1U :
                   tag == 0xcdU || tag == 0xd1U ? 2U :
                   tag == 0xceU || tag == 0xd2U || tag == 0xcaU ? 4U :
                   tag == 0xcfU || tag == 0xd3U || tag == 0xcbU ? 8U : 0U;
    const uint8_t *bytes;
    if (width == 0U || !take(cursor, remaining, width, &bytes))
        return RNS_ERROR_PROTOCOL;
    uint64_t value = unsigned_be(bytes, width);
    if (tag == 0xcaU) {
        uint32_t bits = (uint32_t)value;
        float decoded;
        memcpy(&decoded, &bits, sizeof decoded);
        entry->kind = RNS_HOSTED_FORM_FLOAT;
        entry->float_value = decoded;
    } else if (tag == 0xcbU) {
        double decoded;
        memcpy(&decoded, &value, sizeof decoded);
        entry->kind = RNS_HOSTED_FORM_FLOAT;
        entry->float_value = decoded;
    } else if (tag >= 0xd0U && tag <= 0xd3U) {
        entry->kind = RNS_HOSTED_FORM_SIGNED;
        if (width < 8U && (value & (UINT64_C(1) << (width * 8U - 1U))) != 0U)
            value |= UINT64_MAX << (width * 8U);
        memcpy(&entry->signed_value, &value, sizeof value);
    } else {
        entry->kind = RNS_HOSTED_FORM_UNSIGNED;
        entry->unsigned_value = value;
    }
    return RNS_OK;
}

static bool retained(const uint8_t *key, size_t length) {
    return (length >= 6U && memcmp(key, "field_", 6U) == 0) ||
           (length >= 4U && memcmp(key, "var_", 4U) == 0);
}

rns_status_t rns_hosted_form_decode(const uint8_t *encoded, size_t length,
                                    rns_hosted_form_t *form) {
    if (encoded == NULL || form == NULL || length == 0U)
        return RNS_ERROR_INVALID_ARGUMENT;
    memset(form, 0, sizeof *form);
    if (length > RNS_HOSTED_FORM_MAX_ENCODED) return RNS_ERROR_OVERFLOW;
    if (length == 1U && encoded[0] == 0xc0U) return RNS_OK;
    const uint8_t *cursor = encoded;
    size_t remaining = length;
    uint8_t tag = *cursor++; --remaining;
    uint32_t count;
    if ((tag & 0xf0U) == 0x80U) count = tag & 0x0fU;
    else if (tag == 0xdeU || tag == 0xdfU) {
        size_t width = tag == 0xdeU ? 2U : 4U;
        const uint8_t *size;
        if (!take(&cursor, &remaining, width, &size)) return RNS_ERROR_PROTOCOL;
        count = (uint32_t)unsigned_be(size, width);
    } else return RNS_ERROR_PROTOCOL;
    if (count > RNS_HOSTED_FORM_MAX_ENTRIES) return RNS_ERROR_OVERFLOW;
    for (uint32_t i = 0U; i < count; ++i) {
        if (remaining == 0U) return RNS_ERROR_PROTOCOL;
        const uint8_t *key_start = cursor;
        size_t key_remaining = remaining;
        uint8_t key_tag = *cursor++; --remaining;
        const uint8_t *key;
        size_t key_length;
        if (!span(&cursor, &remaining, key_tag, false, &key, &key_length)) {
            cursor = key_start;
            remaining = key_remaining;
            if (!skip(&cursor, &remaining, 0U) || !skip(&cursor, &remaining, 0U))
                return RNS_ERROR_PROTOCOL;
            continue;
        }
        if (!retained(key, key_length)) {
            if (!skip(&cursor, &remaining, 0U)) return RNS_ERROR_PROTOCOL;
            continue;
        }
        if (key_length > RNS_HOSTED_FORM_KEY_MAX)
            return RNS_ERROR_OVERFLOW;
        size_t destination = form->count;
        for (size_t j = 0U; j < form->count; ++j)
            if (form->entries[j].key_length == key_length &&
                memcmp(form->entries[j].key, key, key_length) == 0) {
                destination = j;
                break;
            }
        if (destination == form->count) {
            if (form->count == RNS_HOSTED_FORM_MAX_ENTRIES)
                return RNS_ERROR_OVERFLOW;
            ++form->count;
        }
        rns_hosted_form_entry_t entry = {.key = key, .key_length = key_length};
        rns_status_t status = decode_value(&cursor, &remaining, &entry);
        if (status != RNS_OK) return status;
        form->entries[destination] = entry;
    }
    return remaining == 0U ? RNS_OK : RNS_ERROR_PROTOCOL;
}
