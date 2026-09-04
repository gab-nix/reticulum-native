#ifndef RETICULUM_RADIO_FRAMING_H
#define RETICULUM_RADIO_FRAMING_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "reticulum/framing.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Stock RNode split framing reserves one byte in every 255-byte LoRa frame.
 * Reticulum packets are bounded at 500 bytes even though other RNode-internal
 * buffers can be larger.
 */
#define RNS_RADIO_PHY_MTU 255U
#define RNS_RADIO_FRAME_HEADER_SIZE 1U
#define RNS_RADIO_FRAME_PAYLOAD_MTU \
    (RNS_RADIO_PHY_MTU - RNS_RADIO_FRAME_HEADER_SIZE)
#define RNS_RADIO_PACKET_MTU 500U
#define RNS_RADIO_MAX_FRAMES 2U
#define RNS_RADIO_SEQUENCE_COUNT 16U
#define RNS_RADIO_SEQUENCE_MASK 0xf0U
#define RNS_RADIO_SPLIT_FLAG 0x01U
#define RNS_RADIO_RESERVED_MASK 0x0eU

typedef struct rns_radio_encoded_packet {
    uint8_t frames[RNS_RADIO_MAX_FRAMES][RNS_RADIO_PHY_MTU];
    size_t lengths[RNS_RADIO_MAX_FRAMES];
    size_t count;
} rns_radio_encoded_packet_t;

typedef struct rns_radio_reassembler {
    uint8_t *buffer;
    size_t capacity;
    size_t length;
    uint64_t fragment_timeout_ms;
    uint64_t sequence_reuse_guard_ms;
    uint64_t pending_since_ms;
    uint64_t sequence_blocked_until_ms[RNS_RADIO_SEQUENCE_COUNT];
    bool sequence_blocked[RNS_RADIO_SEQUENCE_COUNT];
    size_t completed_packets;
    size_t malformed_frames;
    size_t oversized_packets;
    size_t timed_out_packets;
    size_t duplicate_fragments;
    size_t out_of_order_fragments;
    size_t sequence_collisions;
    size_t discarded_packets;
    uint8_t sequence;
    bool pending;
} rns_radio_reassembler_t;

/*
 * Produces the complete one- or two-frame result before returning. The caller
 * chooses the 4-bit sequence, normally from the installed platform entropy
 * provider. No callback can observe a half-committed split packet.
 */
rns_status_t rns_radio_frame_encode(const uint8_t *packet,
                                    size_t packet_length,
                                    uint8_t sequence,
                                    rns_radio_encoded_packet_t *encoded);

/*
 * Timeouts are caller supplied because safe values depend on modem airtime,
 * queueing and the configured duty-cycle scheduler. sequence_reuse_guard_ms
 * must cover the maximum lifetime of a delayed fragment on that interface.
 */
rns_status_t rns_radio_reassembler_init(
    rns_radio_reassembler_t *reassembler, uint8_t *storage,
    size_t storage_capacity, uint64_t fragment_timeout_ms,
    uint64_t sequence_reuse_guard_ms);
void rns_radio_reassembler_reset(rns_radio_reassembler_t *reassembler);

/* Expires a partial split packet. Returns true only when state was expired. */
bool rns_radio_reassembler_expire(rns_radio_reassembler_t *reassembler,
                                  uint64_t now_ms);

/*
 * Consumes one complete LoRa PHY frame. A split first frame is always exactly
 * 255 bytes; a split second frame is always shorter. This invariant lets the
 * decoder reject reordered, duplicated and ambiguous fragments fail-closed.
 */
rns_status_t rns_radio_reassembler_feed(rns_radio_reassembler_t *reassembler,
                                        const uint8_t *frame,
                                        size_t frame_length,
                                        uint64_t now_ms,
                                        rns_frame_callback_t callback,
                                        void *context);

#ifdef __cplusplus
}
#endif

#endif
