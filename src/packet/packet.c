#include "reticulum/packet.h"
#include "reticulum/crypto.h"

#include <string.h>

static int layout(uint8_t flags, size_t raw_length, size_t *context_offset, size_t *data_offset) {
    uint8_t ht = (flags >> 6) & 1u; size_t co = ht ? 34u : 18u;
    if (raw_length <= co || raw_length > RNS_MTU) return 0;
    *context_offset = co; *data_offset = co + 1; return 1;
}

int rns_packet_encode(const rns_packet *packet, uint8_t *out, size_t capacity, size_t *out_length) {
    size_t address_length, total, offset;
    if (!packet || !out || !out_length || packet->header_type > 1 || packet->context_flag > 1 ||
        packet->transport_type > 1 || packet->destination_type > 3 || packet->packet_type > 3 ||
        (packet->data_length && !packet->data)) return 0;
    address_length = packet->header_type == RNS_PACKET_HEADER_2 ? 32 : 16;
    total = 2 + address_length + 1 + packet->data_length;
    if (total > RNS_MTU || capacity < total) return 0;
    out[0] = (uint8_t)((packet->header_type << 6) | (packet->context_flag << 5) |
                       (packet->transport_type << 4) | (packet->destination_type << 2) | packet->packet_type);
    out[1] = packet->hops; offset = 2;
    if (packet->header_type == RNS_PACKET_HEADER_2) { memcpy(out + offset, packet->transport_id, 16); offset += 16; }
    memcpy(out + offset, packet->destination_hash, 16); offset += 16; out[offset++] = packet->context;
    if (packet->data_length) memcpy(out + offset, packet->data, packet->data_length);
    *out_length = total; return 1;
}

int rns_packet_decode(rns_packet *packet, const uint8_t *raw, size_t raw_length) {
    size_t context_offset, data_offset, offset = 2;
    if (!packet || !raw || raw_length < 2 || !layout(raw[0], raw_length, &context_offset, &data_offset) || data_offset == raw_length) return 0;
    memset(packet, 0, sizeof(*packet));
    packet->header_type = (raw[0] >> 6) & 1; packet->context_flag = (raw[0] >> 5) & 1;
    packet->transport_type = (raw[0] >> 4) & 1; packet->destination_type = (raw[0] >> 2) & 3;
    packet->packet_type = raw[0] & 3; packet->hops = raw[1];
    if (packet->header_type == RNS_PACKET_HEADER_2) { memcpy(packet->transport_id, raw + offset, 16); offset += 16; }
    memcpy(packet->destination_hash, raw + offset, 16); packet->context = raw[context_offset];
    packet->data = raw + data_offset; packet->data_length = raw_length - data_offset; return 1;
}

int rns_packet_hash(const uint8_t *raw, size_t raw_length, uint8_t out[32]) {
    uint8_t hashable[RNS_MTU]; size_t context_offset, data_offset, source_offset, n;
    if (!raw || !out || raw_length < 2 || !layout(raw[0], raw_length, &context_offset, &data_offset)) return 0;
    (void)context_offset; (void)data_offset;
    source_offset = ((raw[0] >> 6) & 1u) ? 18u : 2u;
    n = raw_length - source_offset;
    hashable[0] = raw[0] & 0x0f; memcpy(hashable + 1, raw + source_offset, n);
    return rns_sha256(hashable, n + 1, out);
}

int rns_packet_truncated_hash(const uint8_t *raw, size_t raw_length, uint8_t out[16]) {
    uint8_t digest[32]; if (!out || !rns_packet_hash(raw, raw_length, digest)) return 0; memcpy(out, digest, 16); return 1;
}
