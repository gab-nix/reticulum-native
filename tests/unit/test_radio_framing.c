#include "reticulum/radio_framing.h"

#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

typedef struct packet_capture {
    uint8_t packet[RNS_RADIO_PACKET_MTU];
    size_t length;
    size_t count;
    rns_status_t result;
} packet_capture_t;

static rns_status_t capture_packet(const uint8_t *packet, size_t length,
                                   void *context) {
    packet_capture_t *capture = context;
    assert(length <= sizeof(capture->packet));
    memcpy(capture->packet, packet, length);
    capture->length = length;
    capture->count++;
    return capture->result;
}

static void fill_pattern(uint8_t *data, size_t length, uint8_t salt) {
    size_t index;
    for (index = 0U; index < length; ++index) {
        data[index] = (uint8_t)(index * 31U + salt);
    }
}

static void assert_encoding(const uint8_t *packet, size_t packet_length,
                            uint8_t sequence, size_t frame_count,
                            size_t first_length, size_t second_length) {
    rns_radio_encoded_packet_t encoded;
    uint8_t header = (uint8_t)(sequence << 4U);

    memset(&encoded, 0xa5, sizeof(encoded));
    assert(rns_radio_frame_encode(packet, packet_length, sequence, &encoded) ==
           RNS_OK);
    assert(encoded.count == frame_count);
    assert(encoded.lengths[0] == first_length);
    assert(encoded.lengths[1] == second_length);
    if (frame_count == 2U) {
        header |= RNS_RADIO_SPLIT_FLAG;
    }
    assert(encoded.frames[0][0] == header);
    assert(memcmp(encoded.frames[0] + 1U, packet,
                  first_length - RNS_RADIO_FRAME_HEADER_SIZE) == 0);
    if (frame_count == 2U) {
        assert(encoded.frames[1][0] == header);
        assert(memcmp(encoded.frames[1] + 1U,
                      packet + RNS_RADIO_FRAME_PAYLOAD_MTU,
                      second_length - RNS_RADIO_FRAME_HEADER_SIZE) == 0);
    }
}

static void test_encode_boundaries_and_transaction(void) {
    uint8_t packet[RNS_RADIO_PACKET_MTU + 1U];
    rns_radio_encoded_packet_t encoded;
    rns_radio_encoded_packet_t before;

    fill_pattern(packet, sizeof(packet), 7U);
    assert_encoding(packet, 1U, 0U, 1U, 2U, 0U);
    assert_encoding(packet, 254U, 15U, 1U, 255U, 0U);
    assert_encoding(packet, 255U, 3U, 2U, 255U, 2U);
    assert_encoding(packet, 500U, 10U, 2U, 255U, 247U);

    memset(&encoded, 0x5a, sizeof(encoded));
    before = encoded;
    assert(rns_radio_frame_encode(packet, 0U, 0U, &encoded) ==
           RNS_ERROR_INVALID_ARGUMENT);
    assert(memcmp(&encoded, &before, sizeof(encoded)) == 0);
    assert(rns_radio_frame_encode(packet, sizeof(packet), 0U, &encoded) ==
           RNS_ERROR_INVALID_ARGUMENT);
    assert(memcmp(&encoded, &before, sizeof(encoded)) == 0);
    assert(rns_radio_frame_encode(packet, 1U, 16U, &encoded) ==
           RNS_ERROR_INVALID_ARGUMENT);
    assert(memcmp(&encoded, &before, sizeof(encoded)) == 0);
    assert(rns_radio_frame_encode(NULL, 1U, 0U, &encoded) ==
           RNS_ERROR_INVALID_ARGUMENT);
    assert(rns_radio_frame_encode(packet, 1U, 0U, NULL) ==
           RNS_ERROR_INVALID_ARGUMENT);
}

static void test_round_trip_boundaries(void) {
    static const size_t lengths[] = {1U, 254U, 255U, 500U};
    uint8_t packet[RNS_RADIO_PACKET_MTU];
    uint8_t storage[RNS_RADIO_PACKET_MTU];
    size_t case_index;

    fill_pattern(packet, sizeof(packet), 19U);
    for (case_index = 0U; case_index < sizeof(lengths) / sizeof(lengths[0]);
         ++case_index) {
        rns_radio_encoded_packet_t encoded;
        rns_radio_reassembler_t reassembler;
        packet_capture_t capture = {{0U}, 0U, 0U, RNS_OK};
        size_t frame_index;

        assert(rns_radio_frame_encode(packet, lengths[case_index],
                                      (uint8_t)case_index, &encoded) == RNS_OK);
        assert(rns_radio_reassembler_init(&reassembler, storage,
                                          sizeof(storage), 5000U,
                                          10000U) == RNS_OK);
        for (frame_index = 0U; frame_index < encoded.count; ++frame_index) {
            assert(rns_radio_reassembler_feed(
                       &reassembler, encoded.frames[frame_index],
                       encoded.lengths[frame_index], 100U + frame_index,
                       capture_packet, &capture) == RNS_OK);
        }
        assert(capture.count == 1U);
        assert(capture.length == lengths[case_index]);
        assert(memcmp(capture.packet, packet, lengths[case_index]) == 0);
        assert(reassembler.completed_packets == 1U);
        assert(!reassembler.pending);
    }
}

static void test_invalid_headers_and_short_first(void) {
    static const uint8_t header_only[] = {0x11U};
    static const uint8_t reserved[] = {0x12U, 0xaaU};
    static const uint8_t short_split[] = {0x51U, 0xaaU};
    uint8_t oversized[RNS_RADIO_PHY_MTU + 1U] = {0U};
    uint8_t storage[RNS_RADIO_PACKET_MTU];
    rns_radio_reassembler_t reassembler;
    packet_capture_t capture = {{0U}, 0U, 0U, RNS_OK};

    assert(rns_radio_reassembler_init(&reassembler, storage, sizeof(storage),
                                      1000U, 2000U) == RNS_OK);
    assert(rns_radio_reassembler_feed(&reassembler, header_only,
                                      sizeof(header_only), 1U, capture_packet,
                                      &capture) == RNS_ERROR_PROTOCOL);
    assert(rns_radio_reassembler_feed(&reassembler, reserved,
                                      sizeof(reserved), 2U, capture_packet,
                                      &capture) == RNS_ERROR_PROTOCOL);
    assert(rns_radio_reassembler_feed(&reassembler, oversized,
                                      sizeof(oversized), 3U, capture_packet,
                                      &capture) == RNS_ERROR_PROTOCOL);
    assert(rns_radio_reassembler_feed(&reassembler, short_split,
                                      sizeof(short_split), 4U, capture_packet,
                                      &capture) == RNS_ERROR_PROTOCOL);
    assert(reassembler.malformed_frames == 3U);
    assert(reassembler.out_of_order_fragments == 1U);
    assert(capture.count == 0U);
}

static void test_duplicate_and_reordered_fragments(void) {
    uint8_t packet[300U];
    uint8_t storage[RNS_RADIO_PACKET_MTU];
    rns_radio_encoded_packet_t encoded;
    rns_radio_reassembler_t reassembler;
    packet_capture_t capture = {{0U}, 0U, 0U, RNS_OK};

    fill_pattern(packet, sizeof(packet), 23U);
    assert(rns_radio_frame_encode(packet, sizeof(packet), 6U, &encoded) ==
           RNS_OK);
    assert(rns_radio_reassembler_init(&reassembler, storage, sizeof(storage),
                                      1000U, 2000U) == RNS_OK);

    /* A second fragment received first is rejected, never delivered. */
    assert(rns_radio_reassembler_feed(&reassembler, encoded.frames[1],
                                      encoded.lengths[1], 10U, capture_packet,
                                      &capture) == RNS_ERROR_PROTOCOL);
    assert(capture.count == 0U && !reassembler.pending);

    assert(rns_radio_reassembler_feed(&reassembler, encoded.frames[0],
                                      encoded.lengths[0], 11U, capture_packet,
                                      &capture) == RNS_OK);
    /* An exact repeated first fragment is ignored rather than appended. */
    assert(rns_radio_reassembler_feed(&reassembler, encoded.frames[0],
                                      encoded.lengths[0], 12U, capture_packet,
                                      &capture) == RNS_OK);
    assert(reassembler.duplicate_fragments == 1U);
    assert(rns_radio_reassembler_feed(&reassembler, encoded.frames[1],
                                      encoded.lengths[1], 13U, capture_packet,
                                      &capture) == RNS_OK);
    assert(capture.count == 1U && capture.length == sizeof(packet));
    assert(memcmp(capture.packet, packet, sizeof(packet)) == 0);

    /* A repeated second fragment has no pending first and cannot redeliver. */
    assert(rns_radio_reassembler_feed(&reassembler, encoded.frames[1],
                                      encoded.lengths[1], 14U, capture_packet,
                                      &capture) == RNS_ERROR_PROTOCOL);
    assert(capture.count == 1U);
}

static void test_collision_loss_timeout_and_reuse_guard(void) {
    uint8_t packet_a[300U];
    uint8_t packet_b[300U];
    uint8_t storage[RNS_RADIO_PACKET_MTU];
    rns_radio_encoded_packet_t encoded_a;
    rns_radio_encoded_packet_t encoded_b;
    rns_radio_reassembler_t reassembler;
    packet_capture_t capture = {{0U}, 0U, 0U, RNS_OK};

    fill_pattern(packet_a, sizeof(packet_a), 5U);
    fill_pattern(packet_b, sizeof(packet_b), 9U);
    assert(rns_radio_frame_encode(packet_a, sizeof(packet_a), 2U,
                                  &encoded_a) == RNS_OK);
    assert(rns_radio_frame_encode(packet_b, sizeof(packet_b), 2U,
                                  &encoded_b) == RNS_OK);
    assert(rns_radio_reassembler_init(&reassembler, storage, sizeof(storage),
                                      100U, 500U) == RNS_OK);

    assert(rns_radio_reassembler_feed(&reassembler, encoded_a.frames[0],
                                      encoded_a.lengths[0], 1000U,
                                      capture_packet, &capture) == RNS_OK);
    /* Different bytes under the same sequence are ambiguous: fail closed. */
    assert(rns_radio_reassembler_feed(&reassembler, encoded_b.frames[0],
                                      encoded_b.lengths[0], 1001U,
                                      capture_packet,
                                      &capture) == RNS_ERROR_PROTOCOL);
    assert(!reassembler.pending && reassembler.sequence_collisions == 1U);
    assert(rns_radio_reassembler_feed(&reassembler, encoded_a.frames[1],
                                      encoded_a.lengths[1], 1002U,
                                      capture_packet,
                                      &capture) == RNS_ERROR_PROTOCOL);
    assert(capture.count == 0U);

    /* The configured reuse guard rejects a fresh first with that sequence. */
    assert(rns_radio_reassembler_feed(&reassembler, encoded_b.frames[0],
                                      encoded_b.lengths[0], 1200U,
                                      capture_packet,
                                      &capture) == RNS_ERROR_INVALID_STATE);
    assert(!reassembler.pending);
    assert(rns_radio_reassembler_feed(&reassembler, encoded_b.frames[0],
                                      encoded_b.lengths[0], 1501U,
                                      capture_packet, &capture) == RNS_OK);
    assert(rns_radio_reassembler_expire(&reassembler, 1550U) == false);
    assert(rns_radio_reassembler_expire(&reassembler, 1601U) == true);
    assert(!reassembler.pending && reassembler.timed_out_packets == 1U);
    /* Clock rollback does not expire or unblock state early. */
    assert(rns_radio_reassembler_feed(&reassembler, encoded_b.frames[0],
                                      encoded_b.lengths[0], 1599U,
                                      capture_packet,
                                      &capture) == RNS_ERROR_INVALID_STATE);
    assert(capture.count == 0U);
}

static void test_new_sequence_replaces_pending_safely(void) {
    uint8_t packet_a[300U];
    uint8_t packet_b[300U];
    uint8_t storage[RNS_RADIO_PACKET_MTU];
    rns_radio_encoded_packet_t encoded_a;
    rns_radio_encoded_packet_t encoded_b;
    rns_radio_reassembler_t reassembler;
    packet_capture_t capture = {{0U}, 0U, 0U, RNS_OK};

    fill_pattern(packet_a, sizeof(packet_a), 13U);
    fill_pattern(packet_b, sizeof(packet_b), 17U);
    assert(rns_radio_frame_encode(packet_a, sizeof(packet_a), 1U,
                                  &encoded_a) == RNS_OK);
    assert(rns_radio_frame_encode(packet_b, sizeof(packet_b), 2U,
                                  &encoded_b) == RNS_OK);
    assert(rns_radio_reassembler_init(&reassembler, storage, sizeof(storage),
                                      100U, 500U) == RNS_OK);
    assert(rns_radio_reassembler_feed(&reassembler, encoded_a.frames[0],
                                      encoded_a.lengths[0], 10U,
                                      capture_packet, &capture) == RNS_OK);
    assert(rns_radio_reassembler_feed(&reassembler, encoded_b.frames[0],
                                      encoded_b.lengths[0], 11U,
                                      capture_packet, &capture) == RNS_OK);
    assert(reassembler.discarded_packets == 1U && reassembler.pending);
    assert(rns_radio_reassembler_feed(&reassembler, encoded_a.frames[1],
                                      encoded_a.lengths[1], 12U,
                                      capture_packet,
                                      &capture) == RNS_ERROR_PROTOCOL);
    assert(rns_radio_reassembler_feed(&reassembler, encoded_b.frames[1],
                                      encoded_b.lengths[1], 13U,
                                      capture_packet, &capture) == RNS_OK);
    assert(capture.count == 1U);
    assert(memcmp(capture.packet, packet_b, sizeof(packet_b)) == 0);
}

static void test_bounded_space_and_callback_failure(void) {
    uint8_t packet[255U];
    uint8_t small_storage[RNS_RADIO_FRAME_PAYLOAD_MTU];
    uint8_t storage[RNS_RADIO_PACKET_MTU];
    rns_radio_encoded_packet_t encoded;
    rns_radio_reassembler_t reassembler;
    packet_capture_t capture = {{0U}, 0U, 0U, RNS_OK};

    fill_pattern(packet, sizeof(packet), 31U);
    assert(rns_radio_frame_encode(packet, sizeof(packet), 8U, &encoded) ==
           RNS_OK);
    assert(rns_radio_reassembler_init(&reassembler, small_storage,
                                      sizeof(small_storage), 100U,
                                      200U) == RNS_OK);
    assert(rns_radio_reassembler_feed(&reassembler, encoded.frames[0],
                                      encoded.lengths[0], 1U, capture_packet,
                                      &capture) == RNS_OK);
    assert(rns_radio_reassembler_feed(&reassembler, encoded.frames[1],
                                      encoded.lengths[1], 2U, capture_packet,
                                      &capture) == RNS_ERROR_OVERFLOW);
    assert(capture.count == 0U && !reassembler.pending);

    assert(rns_radio_reassembler_init(&reassembler, storage, sizeof(storage),
                                      100U, 200U) == RNS_OK);
    capture.result = RNS_ERROR_IO;
    assert(rns_radio_reassembler_feed(&reassembler, encoded.frames[0],
                                      encoded.lengths[0], 10U, capture_packet,
                                      &capture) == RNS_OK);
    assert(rns_radio_reassembler_feed(&reassembler, encoded.frames[1],
                                      encoded.lengths[1], 11U, capture_packet,
                                      &capture) == RNS_ERROR_IO);
    assert(capture.count == 1U && !reassembler.pending);
    assert(reassembler.completed_packets == 0U);
}

static void test_init_validation_and_reset(void) {
    uint8_t storage[RNS_RADIO_PACKET_MTU + 1U];
    rns_radio_reassembler_t reassembler;

    assert(rns_radio_reassembler_init(NULL, storage, 1U, 1U, 1U) ==
           RNS_ERROR_INVALID_ARGUMENT);
    assert(rns_radio_reassembler_init(&reassembler, NULL, 1U, 1U, 1U) ==
           RNS_ERROR_INVALID_ARGUMENT);
    assert(rns_radio_reassembler_init(&reassembler, storage, 0U, 1U, 1U) ==
           RNS_ERROR_INVALID_ARGUMENT);
    assert(rns_radio_reassembler_init(&reassembler, storage, sizeof(storage),
                                      1U, 1U) == RNS_ERROR_INVALID_ARGUMENT);
    assert(rns_radio_reassembler_init(&reassembler, storage, 1U, 0U, 1U) ==
           RNS_ERROR_INVALID_ARGUMENT);
    assert(rns_radio_reassembler_init(&reassembler, storage, 1U, 1U, 0U) ==
           RNS_ERROR_INVALID_ARGUMENT);
    assert(rns_radio_reassembler_init(&reassembler, storage,
                                      RNS_RADIO_PACKET_MTU, 1U, 1U) == RNS_OK);
    rns_radio_reassembler_reset(&reassembler);
    rns_radio_reassembler_reset(NULL);
}

int main(void) {
    test_encode_boundaries_and_transaction();
    test_round_trip_boundaries();
    test_invalid_headers_and_short_first();
    test_duplicate_and_reordered_fragments();
    test_collision_loss_timeout_and_reuse_guard();
    test_new_sequence_replaces_pending_safely();
    test_bounded_space_and_callback_failure();
    test_init_validation_and_reset();
    return 0;
}
