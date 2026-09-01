#include "reticulum/buffer.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

void rns_buffer_init(rns_buffer_t *buffer) {
    if (buffer != NULL) {
        buffer->data = NULL;
        buffer->len = 0U;
        buffer->capacity = 0U;
    }
}

void rns_buffer_clear(rns_buffer_t *buffer) {
    if (buffer != NULL) {
        free(buffer->data);
        rns_buffer_init(buffer);
    }
}

rns_status_t rns_buffer_reserve(rns_buffer_t *buffer, size_t capacity) {
    uint8_t *allocation;

    if (buffer == NULL) {
        return RNS_ERROR_INVALID_ARGUMENT;
    }
    if (capacity <= buffer->capacity) {
        return RNS_OK;
    }
    allocation = realloc(buffer->data, capacity);
    if (allocation == NULL) {
        return RNS_ERROR_NO_MEMORY;
    }
    buffer->data = allocation;
    buffer->capacity = capacity;
    return RNS_OK;
}

rns_status_t rns_buffer_resize(rns_buffer_t *buffer, size_t length) {
    rns_status_t status;

    if (buffer == NULL) {
        return RNS_ERROR_INVALID_ARGUMENT;
    }
    status = rns_buffer_reserve(buffer, length);
    if (status != RNS_OK) {
        return status;
    }
    if (length > buffer->len) {
        memset(buffer->data + buffer->len, 0, length - buffer->len);
    }
    buffer->len = length;
    return RNS_OK;
}

rns_status_t rns_buffer_append(rns_buffer_t *buffer, const void *data, size_t length) {
    size_t required;
    size_t capacity;
    rns_status_t status;

    if (buffer == NULL || (data == NULL && length != 0U)) {
        return RNS_ERROR_INVALID_ARGUMENT;
    }
    if (length > SIZE_MAX - buffer->len) {
        return RNS_ERROR_OVERFLOW;
    }
    required = buffer->len + length;
    capacity = buffer->capacity;
    if (required > capacity) {
        capacity = capacity == 0U ? 64U : capacity;
        while (capacity < required) {
            if (capacity > SIZE_MAX / 2U) {
                capacity = required;
                break;
            }
            capacity *= 2U;
        }
        status = rns_buffer_reserve(buffer, capacity);
        if (status != RNS_OK) {
            return status;
        }
    }
    if (length != 0U) {
        memcpy(buffer->data + buffer->len, data, length);
    }
    buffer->len = required;
    return RNS_OK;
}

