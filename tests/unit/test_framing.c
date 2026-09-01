#include "reticulum/framing.h"

#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

typedef struct capture {
    uint8_t bytes[64];
    size_t length;
    size_t count;
} capture_t;

static rns_status_t capture_frame(const uint8_t *frame, size_t length, void *context) {
    capture_t *capture = context;
    assert(length <= sizeof(capture->bytes));
    memcpy(capture->bytes, frame, length);
    capture->length = length;
    capture->count++;
    return RNS_OK;
}

static void test_hdlc_round_trip_streamed(void) {
    static const uint8_t plain[] = {0x01U, RNS_HDLC_FLAG, RNS_HDLC_ESCAPE, 0x02U};
    uint8_t encoded[16];
    uint8_t storage[8];
    size_t encoded_length = 0U;
    rns_hdlc_decoder_t decoder;
    capture_t capture = {{0U}, 0U, 0U};

    assert(rns_hdlc_encode(plain, sizeof(plain), encoded, sizeof(encoded),
                           &encoded_length) == RNS_OK);
    rns_hdlc_decoder_init(&decoder, storage, sizeof(storage));
    assert(rns_hdlc_decoder_feed(&decoder, encoded, 3U, capture_frame, &capture) == RNS_OK);
    assert(rns_hdlc_decoder_feed(&decoder, encoded + 3U, encoded_length - 3U,
                                 capture_frame, &capture) == RNS_OK);
    assert(capture.count == 1U && capture.length == sizeof(plain));
    assert(memcmp(capture.bytes, plain, sizeof(plain)) == 0);
}

static void test_hdlc_recovery(void) {
    static const uint8_t stream[] = {
        RNS_HDLC_FLAG, 1U, 2U, 3U, 4U, RNS_HDLC_FLAG,
        RNS_HDLC_FLAG, 9U, RNS_HDLC_ESCAPE, RNS_HDLC_FLAG,
        RNS_HDLC_FLAG, 7U, RNS_HDLC_FLAG
    };
    uint8_t storage[3];
    rns_hdlc_decoder_t decoder;
    capture_t capture = {{0U}, 0U, 0U};

    rns_hdlc_decoder_init(&decoder, storage, sizeof(storage));
    assert(rns_hdlc_decoder_feed(&decoder, stream, sizeof(stream),
                                 capture_frame, &capture) == RNS_OK);
    assert(decoder.oversized_frames == 1U);
    assert(decoder.malformed_frames == 1U);
    assert(capture.count == 1U && capture.length == 1U && capture.bytes[0] == 7U);
}

static void test_kiss_round_trip_and_recovery(void) {
    static const uint8_t plain[] = {0x01U, RNS_KISS_FEND, RNS_KISS_FESC, 0x02U};
    static const uint8_t bad[] = {
        RNS_KISS_FEND, 0U, 1U, RNS_KISS_FESC, 0x01U, RNS_KISS_FEND
    };
    uint8_t encoded[16];
    uint8_t storage[8];
    size_t encoded_length = 0U;
    rns_kiss_decoder_t decoder;
    capture_t capture = {{0U}, 0U, 0U};

    assert(rns_kiss_encode(3U, plain, sizeof(plain), encoded, sizeof(encoded),
                           &encoded_length) == RNS_OK);
    rns_kiss_decoder_init(&decoder, storage, sizeof(storage));
    assert(rns_kiss_decoder_feed(&decoder, bad, sizeof(bad), capture_frame, &capture) == RNS_OK);
    assert(decoder.malformed_frames == 1U && capture.count == 0U);
    assert(rns_kiss_decoder_feed(&decoder, encoded, 2U, capture_frame, &capture) == RNS_OK);
    assert(rns_kiss_decoder_feed(&decoder, encoded + 2U, encoded_length - 2U,
                                 capture_frame, &capture) == RNS_OK);
    assert(capture.count == 1U && capture.length == sizeof(plain));
    assert(memcmp(capture.bytes, plain, sizeof(plain)) == 0);
}

static void test_encoder_bounds(void) {
    static const uint8_t byte = RNS_HDLC_FLAG;
    uint8_t output[3];
    size_t length = 99U;

    assert(rns_hdlc_encode(&byte, 1U, output, sizeof(output), &length) == RNS_ERROR_OVERFLOW);
    assert(length == 0U);
    assert(rns_kiss_encode(16U, &byte, 1U, output, sizeof(output), &length) ==
           RNS_ERROR_INVALID_ARGUMENT);
}

int main(void) {
    test_hdlc_round_trip_streamed();
    test_hdlc_recovery();
    test_kiss_round_trip_and_recovery();
    test_encoder_bounds();
    return 0;
}

