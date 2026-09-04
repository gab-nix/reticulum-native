#include "reticulum/radio_framing.h"

#include <limits.h>
#include <string.h>

static uint64_t deadline_after(uint64_t now_ms, uint64_t interval_ms) {
    if (interval_ms > UINT64_MAX - now_ms) {
        return UINT64_MAX;
    }
    return now_ms + interval_ms;
}

static void clear_pending(rns_radio_reassembler_t *reassembler) {
    reassembler->length = 0U;
    reassembler->pending_since_ms = 0U;
    reassembler->sequence = 0U;
    reassembler->pending = false;
}

static void block_sequence(rns_radio_reassembler_t *reassembler,
                           uint8_t sequence, uint64_t now_ms) {
    reassembler->sequence_blocked[sequence] = true;
    reassembler->sequence_blocked_until_ms[sequence] = deadline_after(
        now_ms, reassembler->sequence_reuse_guard_ms);
}

static bool sequence_is_blocked(rns_radio_reassembler_t *reassembler,
                                uint8_t sequence, uint64_t now_ms) {
    if (!reassembler->sequence_blocked[sequence]) {
        return false;
    }
    if (now_ms >= reassembler->sequence_blocked_until_ms[sequence]) {
        reassembler->sequence_blocked[sequence] = false;
        reassembler->sequence_blocked_until_ms[sequence] = 0U;
        return false;
    }
    return true;
}

rns_status_t rns_radio_frame_encode(const uint8_t *packet,
                                    size_t packet_length,
                                    uint8_t sequence,
                                    rns_radio_encoded_packet_t *encoded) {
    rns_radio_encoded_packet_t result;
    size_t second_length;
    uint8_t header;

    if (packet == NULL || packet_length == 0U ||
        packet_length > RNS_RADIO_PACKET_MTU || sequence >= 16U ||
        encoded == NULL) {
        return RNS_ERROR_INVALID_ARGUMENT;
    }

    memset(&result, 0, sizeof(result));
    header = (uint8_t)(sequence << 4U);
    if (packet_length <= RNS_RADIO_FRAME_PAYLOAD_MTU) {
        result.frames[0][0] = header;
        memcpy(result.frames[0] + RNS_RADIO_FRAME_HEADER_SIZE, packet,
               packet_length);
        result.lengths[0] = packet_length + RNS_RADIO_FRAME_HEADER_SIZE;
        result.count = 1U;
    } else {
        header |= RNS_RADIO_SPLIT_FLAG;
        result.frames[0][0] = header;
        memcpy(result.frames[0] + RNS_RADIO_FRAME_HEADER_SIZE, packet,
               RNS_RADIO_FRAME_PAYLOAD_MTU);
        result.lengths[0] = RNS_RADIO_PHY_MTU;

        second_length = packet_length - RNS_RADIO_FRAME_PAYLOAD_MTU;
        result.frames[1][0] = header;
        memcpy(result.frames[1] + RNS_RADIO_FRAME_HEADER_SIZE,
               packet + RNS_RADIO_FRAME_PAYLOAD_MTU, second_length);
        result.lengths[1] = second_length + RNS_RADIO_FRAME_HEADER_SIZE;
        result.count = 2U;
    }

    *encoded = result;
    return RNS_OK;
}

rns_status_t rns_radio_reassembler_init(
    rns_radio_reassembler_t *reassembler, uint8_t *storage,
    size_t storage_capacity, uint64_t fragment_timeout_ms,
    uint64_t sequence_reuse_guard_ms) {
    if (reassembler == NULL || storage == NULL || storage_capacity == 0U ||
        storage_capacity > RNS_RADIO_PACKET_MTU || fragment_timeout_ms == 0U ||
        sequence_reuse_guard_ms == 0U) {
        return RNS_ERROR_INVALID_ARGUMENT;
    }
    memset(reassembler, 0, sizeof(*reassembler));
    reassembler->buffer = storage;
    reassembler->capacity = storage_capacity;
    reassembler->fragment_timeout_ms = fragment_timeout_ms;
    reassembler->sequence_reuse_guard_ms = sequence_reuse_guard_ms;
    return RNS_OK;
}

void rns_radio_reassembler_reset(rns_radio_reassembler_t *reassembler) {
    if (reassembler != NULL) {
        clear_pending(reassembler);
        memset(reassembler->sequence_blocked, 0,
               sizeof(reassembler->sequence_blocked));
        memset(reassembler->sequence_blocked_until_ms, 0,
               sizeof(reassembler->sequence_blocked_until_ms));
    }
}

bool rns_radio_reassembler_expire(rns_radio_reassembler_t *reassembler,
                                  uint64_t now_ms) {
    uint8_t sequence;

    if (reassembler == NULL || !reassembler->pending ||
        now_ms < reassembler->pending_since_ms ||
        now_ms - reassembler->pending_since_ms <
            reassembler->fragment_timeout_ms) {
        return false;
    }
    sequence = reassembler->sequence;
    clear_pending(reassembler);
    block_sequence(reassembler, sequence, now_ms);
    reassembler->timed_out_packets++;
    return true;
}

static rns_status_t buffer_first(rns_radio_reassembler_t *reassembler,
                                 const uint8_t *payload, uint8_t sequence,
                                 uint64_t now_ms) {
    if (RNS_RADIO_FRAME_PAYLOAD_MTU > reassembler->capacity) {
        reassembler->oversized_packets++;
        return RNS_ERROR_OVERFLOW;
    }
    memcpy(reassembler->buffer, payload, RNS_RADIO_FRAME_PAYLOAD_MTU);
    reassembler->length = RNS_RADIO_FRAME_PAYLOAD_MTU;
    reassembler->pending_since_ms = now_ms;
    reassembler->sequence = sequence;
    reassembler->pending = true;
    return RNS_OK;
}

static void discard_pending(rns_radio_reassembler_t *reassembler,
                            uint64_t now_ms) {
    uint8_t sequence = reassembler->sequence;
    clear_pending(reassembler);
    block_sequence(reassembler, sequence, now_ms);
    reassembler->discarded_packets++;
}

rns_status_t rns_radio_reassembler_feed(rns_radio_reassembler_t *reassembler,
                                        const uint8_t *frame,
                                        size_t frame_length,
                                        uint64_t now_ms,
                                        rns_frame_callback_t callback,
                                        void *context) {
    const uint8_t *payload;
    size_t payload_length;
    uint8_t header;
    uint8_t sequence;
    bool split;
    bool first_fragment;
    rns_status_t status;

    if (reassembler == NULL || reassembler->buffer == NULL ||
        reassembler->capacity == 0U || frame == NULL || callback == NULL) {
        return RNS_ERROR_INVALID_ARGUMENT;
    }
    if (frame_length <= RNS_RADIO_FRAME_HEADER_SIZE ||
        frame_length > RNS_RADIO_PHY_MTU) {
        reassembler->malformed_frames++;
        return RNS_ERROR_PROTOCOL;
    }

    header = frame[0];
    if ((header & RNS_RADIO_RESERVED_MASK) != 0U) {
        reassembler->malformed_frames++;
        return RNS_ERROR_PROTOCOL;
    }
    (void)rns_radio_reassembler_expire(reassembler, now_ms);

    sequence = (uint8_t)((header & RNS_RADIO_SEQUENCE_MASK) >> 4U);
    split = (header & RNS_RADIO_SPLIT_FLAG) != 0U;
    payload = frame + RNS_RADIO_FRAME_HEADER_SIZE;
    payload_length = frame_length - RNS_RADIO_FRAME_HEADER_SIZE;

    if (!split) {
        if (reassembler->pending) {
            discard_pending(reassembler, now_ms);
        }
        if (payload_length > reassembler->capacity) {
            reassembler->oversized_packets++;
            return RNS_ERROR_OVERFLOW;
        }
        status = callback(payload, payload_length, context);
        if (status == RNS_OK) {
            reassembler->completed_packets++;
        }
        return status;
    }

    first_fragment = frame_length == RNS_RADIO_PHY_MTU;
    if (first_fragment) {
        if (sequence_is_blocked(reassembler, sequence, now_ms)) {
            reassembler->sequence_collisions++;
            return RNS_ERROR_INVALID_STATE;
        }
        if (reassembler->pending) {
            if (reassembler->sequence == sequence) {
                if (memcmp(reassembler->buffer, payload,
                           RNS_RADIO_FRAME_PAYLOAD_MTU) == 0) {
                    reassembler->duplicate_fragments++;
                    return RNS_OK;
                }
                clear_pending(reassembler);
                block_sequence(reassembler, sequence, now_ms);
                reassembler->sequence_collisions++;
                return RNS_ERROR_PROTOCOL;
            }
            discard_pending(reassembler, now_ms);
        }
        return buffer_first(reassembler, payload, sequence, now_ms);
    }

    if (payload_length >
        RNS_RADIO_PACKET_MTU - RNS_RADIO_FRAME_PAYLOAD_MTU) {
        reassembler->malformed_frames++;
        return RNS_ERROR_PROTOCOL;
    }
    if (!reassembler->pending || reassembler->sequence != sequence) {
        reassembler->out_of_order_fragments++;
        return RNS_ERROR_PROTOCOL;
    }
    if (payload_length > reassembler->capacity - reassembler->length) {
        uint8_t pending_sequence = reassembler->sequence;
        clear_pending(reassembler);
        block_sequence(reassembler, pending_sequence, now_ms);
        reassembler->oversized_packets++;
        return RNS_ERROR_OVERFLOW;
    }

    memcpy(reassembler->buffer + reassembler->length, payload, payload_length);
    reassembler->length += payload_length;
    payload_length = reassembler->length;
    clear_pending(reassembler);
    block_sequence(reassembler, sequence, now_ms);
    status = callback(reassembler->buffer, payload_length, context);
    if (status == RNS_OK) {
        reassembler->completed_packets++;
    }
    return status;
}
