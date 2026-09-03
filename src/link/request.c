#include "reticulum/request.h"

#include "reticulum/crypto.h"

#include <math.h>
#include <stdbool.h>
#include <string.h>

static uint32_t read32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | p[3];
}

static bool skip_object(const uint8_t **cursor, const uint8_t *end,
                        unsigned depth) {
    if (cursor == NULL || *cursor >= end || depth > 32U) return false;
    uint8_t code = *(*cursor)++;
    uint64_t bytes = 0U;
    size_t children = 0U;
    if (code <= 0x7fU || code >= 0xe0U || code == 0xc0U || code == 0xc2U ||
        code == 0xc3U) return true;
    if ((code & 0xe0U) == 0xa0U) bytes = code & 0x1fU;
    else if ((code & 0xf0U) == 0x90U) children = code & 0x0fU;
    else if ((code & 0xf0U) == 0x80U) children = 2U * (code & 0x0fU);
    else switch (code) {
        case 0xc4: case 0xd9:
            if ((size_t)(end - *cursor) < 1U) return false;
            bytes = *(*cursor)++; break;
        case 0xc5: case 0xda:
            if ((size_t)(end - *cursor) < 2U) return false;
            bytes = ((uint64_t)(*cursor)[0] << 8) | (*cursor)[1];
            *cursor += 2; break;
        case 0xc6: case 0xdb:
            if ((size_t)(end - *cursor) < 4U) return false;
            bytes = read32(*cursor); *cursor += 4; break;
        case 0xca: bytes = 4U; break;
        case 0xcb: bytes = 8U; break;
        case 0xcc: case 0xd0: bytes = 1U; break;
        case 0xcd: case 0xd1: bytes = 2U; break;
        case 0xce: case 0xd2: bytes = 4U; break;
        case 0xcf: case 0xd3: bytes = 8U; break;
        case 0xd4: bytes = 2U; break;
        case 0xd5: bytes = 3U; break;
        case 0xd6: bytes = 5U; break;
        case 0xd7: bytes = 9U; break;
        case 0xd8: bytes = 17U; break;
        case 0xdc:
            if ((size_t)(end - *cursor) < 2U) return false;
            children = ((size_t)(*cursor)[0] << 8) | (*cursor)[1];
            *cursor += 2; break;
        case 0xdd:
            if ((size_t)(end - *cursor) < 4U) return false;
            children = read32(*cursor); *cursor += 4; break;
        case 0xde:
            if ((size_t)(end - *cursor) < 2U) return false;
            children = 2U * (((size_t)(*cursor)[0] << 8) | (*cursor)[1]);
            *cursor += 2; break;
        case 0xdf: {
            if ((size_t)(end - *cursor) < 4U) return false;
            uint32_t pairs = read32(*cursor); *cursor += 4;
#if SIZE_MAX < UINT32_MAX
            if ((size_t)pairs > SIZE_MAX / 2U) return false;
#endif
            children = 2U * (size_t)pairs; break;
        }
        default: return false;
    }
    if (bytes > (uint64_t)(end - *cursor)) return false;
    *cursor += (size_t)bytes;
    for (size_t i = 0U; i < children; i++)
        if (!skip_object(cursor, end, depth + 1U)) return false;
    return true;
}

static void write64(uint8_t *out, uint64_t value) {
    for (size_t i = 0U; i < 8U; i++) out[7U - i] = (uint8_t)(value >> (8U * i));
}

rns_status_t rns_request_encode(const char *path, double requested_at,
                                const uint8_t *data, size_t data_length,
                                uint8_t *out, size_t capacity, size_t *length) {
    if (path == NULL || out == NULL || length == NULL || !isfinite(requested_at) ||
        requested_at < 0.0 || (data == NULL && data_length != 0U))
        return RNS_ERROR_INVALID_ARGUMENT;
    size_t path_length = strlen(path);
    if (path_length == 0U || path_length > RNS_REQUEST_PATH_MAX)
        return RNS_ERROR_INVALID_ARGUMENT;
    if (data_length != 0U) {
        const uint8_t *cursor = data;
        if (!skip_object(&cursor, data + data_length, 0U) ||
            cursor != data + data_length) return RNS_ERROR_PROTOCOL;
    }
    size_t needed = 1U + 9U + 2U + RNS_REQUEST_ID_LENGTH +
                    (data_length ? data_length : 1U);
    if (needed > capacity) return RNS_ERROR_OVERFLOW;
    uint8_t digest[32];
    if (!rns_sha256((const uint8_t *)path, path_length, digest))
        return RNS_ERROR_CRYPTO;
    size_t offset = 0U;
    out[offset++] = 0x93U;
    out[offset++] = 0xcbU;
    uint64_t bits;
    memcpy(&bits, &requested_at, sizeof bits);
    write64(out + offset, bits); offset += 8U;
    out[offset++] = 0xc4U;
    out[offset++] = RNS_REQUEST_ID_LENGTH;
    memcpy(out + offset, digest, RNS_REQUEST_ID_LENGTH);
    offset += RNS_REQUEST_ID_LENGTH;
    if (data_length != 0U) {
        memcpy(out + offset, data, data_length); offset += data_length;
    } else out[offset++] = 0xc0U;
    *length = offset;
    return RNS_OK;
}

rns_status_t rns_request_decode(const uint8_t *input, size_t length,
                                rns_request_view_t *request) {
    if (input == NULL || request == NULL) return RNS_ERROR_INVALID_ARGUMENT;
    if (length < 29U || input[0] != 0x93U || input[1] != 0xcbU ||
        input[10] != 0xc4U || input[11] != RNS_REQUEST_ID_LENGTH)
        return RNS_ERROR_PROTOCOL;
    uint64_t bits = 0U;
    for (size_t i = 0U; i < 8U; i++) bits = (bits << 8) | input[2U + i];
    double timestamp;
    memcpy(&timestamp, &bits, sizeof timestamp);
    if (!isfinite(timestamp) || timestamp < 0.0) return RNS_ERROR_PROTOCOL;
    const uint8_t *data = input + 28U;
    const uint8_t *cursor = data;
    if (!skip_object(&cursor, input + length, 0U) || cursor != input + length)
        return RNS_ERROR_PROTOCOL;
    memset(request, 0, sizeof *request);
    request->requested_at = timestamp;
    memcpy(request->path_hash, input + 12U, RNS_REQUEST_ID_LENGTH);
    request->data_msgpack = data;
    request->data_msgpack_length = (size_t)(cursor - data);
    return RNS_OK;
}

static bool read_bytes(const uint8_t **cursor, const uint8_t *end,
                       const uint8_t **bytes, size_t *length) {
    if (*cursor >= end) return false;
    uint8_t code = *(*cursor)++;
    uint64_t size;
    if ((code & 0xe0U) == 0xa0U) size = code & 0x1fU;
    else if (code == 0xc4U || code == 0xd9U) {
        if (*cursor >= end) return false; size = *(*cursor)++;
    } else if (code == 0xc5U || code == 0xdaU) {
        if ((size_t)(end - *cursor) < 2U) return false;
        size = ((uint64_t)(*cursor)[0] << 8) | (*cursor)[1]; *cursor += 2;
    } else if (code == 0xc6U || code == 0xdbU) {
        if ((size_t)(end - *cursor) < 4U) return false;
        size = read32(*cursor); *cursor += 4;
    } else return false;
    if (size > (uint64_t)(end - *cursor)) return false;
    *bytes = *cursor; *length = (size_t)size; *cursor += (size_t)size;
    return true;
}

rns_status_t rns_response_decode(const uint8_t *input, size_t length,
                                 rns_response_view_t *response) {
    if (input == NULL || response == NULL) return RNS_ERROR_INVALID_ARGUMENT;
    if (length < 1U || input[0] != 0x92U) return RNS_ERROR_PROTOCOL;
    const uint8_t *cursor = input + 1U, *id, *body = NULL;
    size_t id_length, body_length = 0U;
    if (!read_bytes(&cursor, input + length, &id, &id_length) ||
        id_length != RNS_REQUEST_ID_LENGTH) return RNS_ERROR_PROTOCOL;
    const uint8_t *object = cursor;
    if (!skip_object(&cursor, input + length, 0U) ||
        cursor != input + length) return RNS_ERROR_PROTOCOL;
    const uint8_t *bytes_cursor = object;
    bool byte_response = read_bytes(&bytes_cursor, input + length, &body,
                                    &body_length) && bytes_cursor == cursor;
    if (!byte_response) {
        body = object;
        body_length = (size_t)(cursor - object);
    }
    memset(response, 0, sizeof *response);
    memcpy(response->request_id, id, RNS_REQUEST_ID_LENGTH);
    response->response_msgpack = object;
    response->response_msgpack_length = (size_t)(cursor - object);
    response->response = body;
    response->response_length = body_length;
    return RNS_OK;
}

rns_status_t rns_response_encode(
    const uint8_t request_id[RNS_REQUEST_ID_LENGTH],
    const uint8_t *response_msgpack, size_t response_msgpack_length,
    uint8_t *output, size_t output_capacity, size_t *output_length) {
    if (request_id == NULL || response_msgpack == NULL ||
        response_msgpack_length == 0U || output == NULL ||
        output_length == NULL) return RNS_ERROR_INVALID_ARGUMENT;
    const uint8_t *cursor = response_msgpack;
    if (!skip_object(&cursor, response_msgpack + response_msgpack_length, 0U) ||
        cursor != response_msgpack + response_msgpack_length)
        return RNS_ERROR_PROTOCOL;
    if (response_msgpack_length > SIZE_MAX - 19U ||
        output_capacity < 19U + response_msgpack_length)
        return RNS_ERROR_OVERFLOW;
    output[0] = 0x92U;
    output[1] = 0xc4U;
    output[2] = RNS_REQUEST_ID_LENGTH;
    memcpy(output + 3U, request_id, RNS_REQUEST_ID_LENGTH);
    memcpy(output + 19U, response_msgpack, response_msgpack_length);
    *output_length = 19U + response_msgpack_length;
    return RNS_OK;
}
