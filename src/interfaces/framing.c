#include "reticulum/framing.h"

#include <string.h>

static rns_status_t append_escaped(uint8_t byte,
                                   uint8_t escape_byte,
                                   uint8_t escaped_first,
                                   uint8_t escaped_second,
                                   uint8_t *output,
                                   size_t output_capacity,
                                   size_t *position) {
    if (byte == escaped_first || byte == escaped_second) {
        if (*position > output_capacity || output_capacity - *position < 2U) {
            return RNS_ERROR_OVERFLOW;
        }
        output[(*position)++] = escape_byte;
        output[(*position)++] = byte == escaped_first ?
                                (uint8_t)(escaped_first ^ 0x20U) :
                                (uint8_t)(escaped_second ^ 0x20U);
    } else {
        if (*position >= output_capacity) {
            return RNS_ERROR_OVERFLOW;
        }
        output[(*position)++] = byte;
    }
    return RNS_OK;
}

void rns_hdlc_decoder_init(rns_hdlc_decoder_t *decoder,
                           uint8_t *storage,
                           size_t storage_capacity) {
    if (decoder != NULL) {
        memset(decoder, 0, sizeof(*decoder));
        decoder->buffer = storage;
        decoder->capacity = storage_capacity;
    }
}

void rns_hdlc_decoder_reset(rns_hdlc_decoder_t *decoder) {
    if (decoder != NULL) {
        decoder->length = 0U;
        decoder->synchronized = false;
        decoder->escaped = false;
        decoder->discarding = false;
    }
}

static rns_status_t hdlc_flag(rns_hdlc_decoder_t *decoder,
                              rns_frame_callback_t callback,
                              void *context) {
    rns_status_t status = RNS_OK;

    if (decoder->escaped) {
        decoder->malformed_frames++;
        decoder->discarding = true;
    }
    if (decoder->synchronized && !decoder->discarding && decoder->length != 0U) {
        status = callback(decoder->buffer, decoder->length, context);
    }
    decoder->length = 0U;
    decoder->synchronized = true;
    decoder->escaped = false;
    decoder->discarding = false;
    return status;
}

rns_status_t rns_hdlc_decoder_feed(rns_hdlc_decoder_t *decoder,
                                   const uint8_t *input,
                                   size_t input_length,
                                   rns_frame_callback_t callback,
                                   void *context) {
    size_t index;

    if (decoder == NULL || callback == NULL ||
        decoder->buffer == NULL || decoder->capacity == 0U ||
        (input == NULL && input_length != 0U)) {
        return RNS_ERROR_INVALID_ARGUMENT;
    }
    for (index = 0U; index < input_length; ++index) {
        uint8_t byte = input[index];
        if (byte == RNS_HDLC_FLAG) {
            rns_status_t status = hdlc_flag(decoder, callback, context);
            if (status != RNS_OK) {
                return status;
            }
            continue;
        }
        if (!decoder->synchronized || decoder->discarding) {
            continue;
        }
        if (decoder->escaped) {
            byte ^= 0x20U;
            decoder->escaped = false;
        } else if (byte == RNS_HDLC_ESCAPE) {
            decoder->escaped = true;
            continue;
        }
        if (decoder->length == decoder->capacity) {
            decoder->oversized_frames++;
            decoder->discarding = true;
            decoder->length = 0U;
            decoder->escaped = false;
            continue;
        }
        decoder->buffer[decoder->length++] = byte;
    }
    return RNS_OK;
}

rns_status_t rns_hdlc_encode(const uint8_t *frame,
                             size_t frame_length,
                             uint8_t *output,
                             size_t output_capacity,
                             size_t *output_length) {
    size_t index;
    size_t position = 0U;

    if ((frame == NULL && frame_length != 0U) || output == NULL || output_length == NULL) {
        return RNS_ERROR_INVALID_ARGUMENT;
    }
    *output_length = 0U;
    if (output_capacity < 2U) {
        return RNS_ERROR_OVERFLOW;
    }
    output[position++] = RNS_HDLC_FLAG;
    for (index = 0U; index < frame_length; ++index) {
        rns_status_t status = append_escaped(frame[index], RNS_HDLC_ESCAPE,
                                             RNS_HDLC_FLAG, RNS_HDLC_ESCAPE,
                                             output, output_capacity, &position);
        if (status != RNS_OK) {
            return status;
        }
    }
    if (position >= output_capacity) {
        return RNS_ERROR_OVERFLOW;
    }
    output[position++] = RNS_HDLC_FLAG;
    *output_length = position;
    return RNS_OK;
}

void rns_kiss_decoder_init(rns_kiss_decoder_t *decoder,
                           uint8_t *storage,
                           size_t storage_capacity) {
    if (decoder != NULL) {
        memset(decoder, 0, sizeof(*decoder));
        decoder->buffer = storage;
        decoder->capacity = storage_capacity;
    }
}

void rns_kiss_decoder_reset(rns_kiss_decoder_t *decoder) {
    if (decoder != NULL) {
        decoder->length = 0U;
        decoder->synchronized = false;
        decoder->have_command = false;
        decoder->escaped = false;
        decoder->discarding = false;
    }
}

static rns_status_t kiss_fend(rns_kiss_decoder_t *decoder,
                              rns_frame_callback_t callback,
                              void *context) {
    rns_status_t status = RNS_OK;

    if (decoder->escaped) {
        decoder->malformed_frames++;
        decoder->discarding = true;
    }
    if (decoder->synchronized && decoder->have_command && !decoder->discarding &&
        (decoder->command & 0x0fU) == RNS_KISS_DATA_COMMAND) {
        status = callback(decoder->buffer, decoder->length, context);
    }
    decoder->length = 0U;
    decoder->synchronized = true;
    decoder->have_command = false;
    decoder->escaped = false;
    decoder->discarding = false;
    return status;
}

rns_status_t rns_kiss_decoder_feed(rns_kiss_decoder_t *decoder,
                                   const uint8_t *input,
                                   size_t input_length,
                                   rns_frame_callback_t callback,
                                   void *context) {
    size_t index;

    if (decoder == NULL || callback == NULL || decoder->buffer == NULL ||
        decoder->capacity == 0U || (input == NULL && input_length != 0U)) {
        return RNS_ERROR_INVALID_ARGUMENT;
    }
    for (index = 0U; index < input_length; ++index) {
        uint8_t byte = input[index];
        if (byte == RNS_KISS_FEND) {
            rns_status_t status = kiss_fend(decoder, callback, context);
            if (status != RNS_OK) {
                return status;
            }
            continue;
        }
        if (!decoder->synchronized || decoder->discarding) {
            continue;
        }
        if (!decoder->have_command) {
            decoder->command = byte;
            decoder->have_command = true;
            continue;
        }
        if (decoder->escaped) {
            if (byte == RNS_KISS_TFEND) {
                byte = RNS_KISS_FEND;
            } else if (byte == RNS_KISS_TFESC) {
                byte = RNS_KISS_FESC;
            } else {
                decoder->malformed_frames++;
                decoder->discarding = true;
                decoder->escaped = false;
                decoder->length = 0U;
                continue;
            }
            decoder->escaped = false;
        } else if (byte == RNS_KISS_FESC) {
            decoder->escaped = true;
            continue;
        }
        if (decoder->length == decoder->capacity) {
            decoder->oversized_frames++;
            decoder->discarding = true;
            decoder->length = 0U;
            decoder->escaped = false;
            continue;
        }
        decoder->buffer[decoder->length++] = byte;
    }
    return RNS_OK;
}

rns_status_t rns_kiss_encode(uint8_t port,
                             const uint8_t *frame,
                             size_t frame_length,
                             uint8_t *output,
                             size_t output_capacity,
                             size_t *output_length) {
    size_t index;
    size_t position = 0U;

    if (port > 15U || (frame == NULL && frame_length != 0U) ||
        output == NULL || output_length == NULL) {
        return RNS_ERROR_INVALID_ARGUMENT;
    }
    *output_length = 0U;
    if (output_capacity < 3U) {
        return RNS_ERROR_OVERFLOW;
    }
    output[position++] = RNS_KISS_FEND;
    output[position++] = (uint8_t)(port << 4U);
    for (index = 0U; index < frame_length; ++index) {
        uint8_t byte = frame[index];
        if (byte == RNS_KISS_FEND || byte == RNS_KISS_FESC) {
            if (position > output_capacity || output_capacity - position < 2U) {
                return RNS_ERROR_OVERFLOW;
            }
            output[position++] = RNS_KISS_FESC;
            output[position++] = byte == RNS_KISS_FEND ? RNS_KISS_TFEND : RNS_KISS_TFESC;
        } else {
            if (position >= output_capacity) {
                return RNS_ERROR_OVERFLOW;
            }
            output[position++] = byte;
        }
    }
    if (position >= output_capacity) {
        return RNS_ERROR_OVERFLOW;
    }
    output[position++] = RNS_KISS_FEND;
    *output_length = position;
    return RNS_OK;
}

