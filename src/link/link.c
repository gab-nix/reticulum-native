#include "reticulum/link.h"

#include "reticulum/crypto.h"
#include "reticulum/packet.h"

#include <math.h>
#include <string.h>

static int derive(rns_link *link) {
    uint8_t shared[32];
    if (!rns_x25519_exchange(link->private_key, link->peer_public, shared)) return 0;
    if (!rns_hkdf_sha256(shared, sizeof(shared), link->link_id, sizeof(link->link_id), NULL, 0,
                         link->derived_key, sizeof(link->derived_key))) return 0;
    return 1;
}

int rns_link_signalling_encode(uint32_t mtu, uint8_t mode, uint8_t out[3]) {
    uint32_t value;
    if (!out || mtu == 0 || mtu > RNS_LINK_MTU_MASK || mode != RNS_LINK_MODE_AES256_CBC) return 0;
    value = (mtu & RNS_LINK_MTU_MASK) | ((uint32_t)(mode << 5) << 16);
    out[0] = (uint8_t)(value >> 16); out[1] = (uint8_t)(value >> 8); out[2] = (uint8_t)value; return 1;
}

int rns_link_signalling_decode(const uint8_t in[3], uint32_t *mtu, uint8_t *mode) {
    uint32_t value; uint8_t decoded_mode;
    if (!in || !mtu || !mode) return 0;
    value = ((uint32_t)in[0] << 16) | ((uint32_t)in[1] << 8) | in[2];
    decoded_mode = (uint8_t)((in[0] & 0xe0u) >> 5); *mtu = value & RNS_LINK_MTU_MASK; *mode = decoded_mode;
    return *mtu != 0 && decoded_mode == RNS_LINK_MODE_AES256_CBC;
}

int rns_link_id_from_request_packet(const uint8_t *raw, size_t raw_length, uint8_t out[16]) {
    rns_packet packet; uint8_t hashable[500], digest[32]; size_t source_offset, retained_data, n;
    if (!raw || !out || !rns_packet_decode(&packet, raw, raw_length) || packet.packet_type != 2 ||
        (packet.data_length != 64 && packet.data_length != 67)) return 0;
    source_offset = packet.header_type == RNS_PACKET_HEADER_2 ? 18u : 2u;
    retained_data = 64; n = (raw_length - packet.data_length - source_offset) + retained_data;
    hashable[0] = raw[0] & 0x0f; memcpy(hashable + 1, raw + source_offset, n);
    if (!rns_sha256(hashable, n + 1, digest)) return 0; memcpy(out, digest, 16); return 1;
}

static int initiator_common(rns_link *link, const rns_identity *destination,
                            const uint8_t *x_private, const uint8_t *s_private,
                            uint32_t mtu, double timeout, rns_link_clock clock, void *context) {
    if (!link || !destination || !clock || timeout <= 0 || !rns_link_signalling_encode(mtu, RNS_LINK_MODE_AES256_CBC, (uint8_t[3]){0})) return 0;
    memset(link, 0, sizeof(*link)); link->role = RNS_LINK_INITIATOR; link->state = RNS_LINK_PENDING;
    link->clock = clock; link->clock_context = context; link->request_time = clock(context); link->deadline = link->request_time + timeout;
    link->mtu = mtu; link->mode = RNS_LINK_MODE_AES256_CBC; link->remote_identity = *destination;
    if (x_private) { memcpy(link->private_key, x_private, 32); if (!rns_x25519_public_from_private(x_private, link->public_key)) return 0; }
    else if (!rns_x25519_generate(link->private_key, link->public_key)) return 0;
    if (s_private) { memcpy(link->signing_private, s_private, 32); if (!rns_ed25519_public_from_private(s_private, link->signing_public)) return 0; }
    else if (!rns_ed25519_generate(link->signing_private, link->signing_public)) return 0;
    return 1;
}

int rns_link_initiator_init(rns_link *link, const rns_identity *destination, uint32_t mtu, double timeout,
                            rns_link_clock clock, void *context) {
    return initiator_common(link, destination, NULL, NULL, mtu, timeout, clock, context);
}
int rns_link_initiator_init_keys(rns_link *link, const rns_identity *destination,
                                 const uint8_t x_private[32], const uint8_t s_private[32],
                                 uint32_t mtu, double timeout, rns_link_clock clock, void *context) {
    if (!x_private || !s_private) return 0; return initiator_common(link, destination, x_private, s_private, mtu, timeout, clock, context);
}

int rns_link_build_request_payload(const rns_link *link, uint8_t out[67]) {
    if (!link || !out || link->role != RNS_LINK_INITIATOR || link->state != RNS_LINK_PENDING) return 0;
    memcpy(out, link->public_key, 32); memcpy(out + 32, link->signing_public, 32);
    return rns_link_signalling_encode(link->mtu, link->mode, out + 64);
}

int rns_link_initiator_set_request_packet(rns_link *link, const uint8_t *raw, size_t raw_length) {
    rns_packet packet; uint8_t expected[67];
    if (!link || link->role != RNS_LINK_INITIATOR || link->state != RNS_LINK_PENDING ||
        !rns_packet_decode(&packet, raw, raw_length) || packet.data_length != 67 ||
        !rns_link_build_request_payload(link, expected) || memcmp(packet.data, expected, 67) != 0) return 0;
    return rns_link_id_from_request_packet(raw, raw_length, link->link_id);
}

static int responder_common(rns_link *link, const rns_identity *identity, const uint8_t *private_key,
                            const uint8_t *raw, size_t raw_length, double timeout, rns_link_clock clock, void *context) {
    rns_packet packet; uint32_t mtu; uint8_t mode;
    if (!link || !identity || !identity->has_private || !raw || !clock || timeout <= 0 ||
        !rns_packet_decode(&packet, raw, raw_length) || packet.packet_type != 2 || packet.data_length != 67 ||
        !rns_link_signalling_decode(packet.data + 64, &mtu, &mode)) return 0;
    memset(link, 0, sizeof(*link)); link->role = RNS_LINK_RESPONDER; link->state = RNS_LINK_HANDSHAKE;
    link->clock = clock; link->clock_context = context; link->request_time = clock(context); link->deadline = link->request_time + timeout;
    link->mtu = mtu; link->mode = mode; memcpy(link->peer_public, packet.data, 32); memcpy(link->peer_signing_public, packet.data + 32, 32);
    memcpy(link->signing_private, identity->signing_private, 32); memcpy(link->signing_public, identity->signing_public, 32);
    if (private_key) { memcpy(link->private_key, private_key, 32); if (!rns_x25519_public_from_private(private_key, link->public_key)) return 0; }
    else if (!rns_x25519_generate(link->private_key, link->public_key)) return 0;
    if (!rns_link_id_from_request_packet(raw, raw_length, link->link_id) || !derive(link)) { link->state = RNS_LINK_CLOSED; return 0; }
    return 1;
}

int rns_link_responder_accept(rns_link *link, const rns_identity *identity, const uint8_t *raw, size_t raw_length,
                              double timeout, rns_link_clock clock, void *context) {
    return responder_common(link, identity, NULL, raw, raw_length, timeout, clock, context);
}
int rns_link_responder_accept_key(rns_link *link, const rns_identity *identity, const uint8_t private_key[32],
                                  const uint8_t *raw, size_t raw_length, double timeout, rns_link_clock clock, void *context) {
    if (!private_key) return 0; return responder_common(link, identity, private_key, raw, raw_length, timeout, clock, context);
}

int rns_link_build_proof(const rns_link *link, uint8_t out[99]) {
    uint8_t signed_data[83];
    if (!link || !out || link->role != RNS_LINK_RESPONDER || link->state != RNS_LINK_HANDSHAKE) return 0;
    memcpy(signed_data, link->link_id, 16); memcpy(signed_data + 16, link->public_key, 32);
    memcpy(signed_data + 48, link->signing_public, 32);
    if (!rns_link_signalling_encode(link->mtu, link->mode, signed_data + 80) ||
        !rns_ed25519_sign(link->signing_private, signed_data, sizeof(signed_data), out)) return 0;
    memcpy(out + 64, link->public_key, 32); memcpy(out + 96, signed_data + 80, 3); return 1;
}

int rns_link_initiator_accept_proof(rns_link *link, const uint8_t *proof, size_t proof_length) {
    uint8_t signed_data[83]; uint32_t mtu; uint8_t mode; double now;
    if (!link || !proof || proof_length != 99 || link->role != RNS_LINK_INITIATOR || link->state != RNS_LINK_PENDING ||
        !rns_link_signalling_decode(proof + 96, &mtu, &mode) || mode != link->mode) return 0;
    memcpy(link->peer_public, proof + 64, 32); memcpy(link->peer_signing_public, link->remote_identity.signing_public, 32);
    memcpy(signed_data, link->link_id, 16); memcpy(signed_data + 16, link->peer_public, 32);
    memcpy(signed_data + 48, link->peer_signing_public, 32); memcpy(signed_data + 80, proof + 96, 3);
    if (!rns_identity_verify(&link->remote_identity, signed_data, sizeof(signed_data), proof) || !derive(link)) return 0;
    now = link->clock(link->clock_context); link->rtt = now - link->request_time; link->mtu = mtu;
    link->state = RNS_LINK_ACTIVE; link->activated_at = now; return 1;
}

static void encode_double(double value, uint8_t out[9]) {
    uint64_t bits; memcpy(&bits, &value, 8); out[0] = 0xcb;
    for (size_t i = 9; i > 1; --i) { out[i - 1] = (uint8_t)bits; bits >>= 8; }
}
static int decode_double(const uint8_t in[9], double *value) {
    uint64_t bits = 0; if (in[0] != 0xcb) return 0;
    for (size_t i = 1; i < 9; ++i) bits = (bits << 8) | in[i]; memcpy(value, &bits, 8);
    return isfinite(*value) && *value >= 0;
}

int rns_link_build_rtt_confirm(rns_link *link, uint8_t *out, size_t capacity, size_t *out_length) {
    uint8_t packed[9]; if (!link || link->role != RNS_LINK_INITIATOR || link->state != RNS_LINK_ACTIVE) return 0;
    encode_double(link->rtt, packed); return rns_token_encrypt(link->derived_key, packed, sizeof(packed), out, capacity, out_length);
}

int rns_link_responder_accept_rtt(rns_link *link, const uint8_t *token, size_t token_length) {
    uint8_t packed[16]; size_t length; double reported, measured, now;
    if (!link || link->role != RNS_LINK_RESPONDER || link->state != RNS_LINK_HANDSHAKE ||
        !rns_token_decrypt(link->derived_key, token, token_length, packed, sizeof(packed), &length) ||
        length != 9 || !decode_double(packed, &reported)) { if (link) link->state = RNS_LINK_CLOSED; return 0; }
    now = link->clock(link->clock_context); measured = now - link->request_time; link->rtt = measured > reported ? measured : reported;
    link->state = RNS_LINK_ACTIVE; link->activated_at = now; return 1;
}

int rns_link_check_timeout(rns_link *link) {
    if (!link || link->state == RNS_LINK_CLOSED) return 0;
    if ((link->state == RNS_LINK_PENDING || link->state == RNS_LINK_HANDSHAKE) &&
        link->clock(link->clock_context) >= link->deadline) { link->state = RNS_LINK_CLOSED; return 1; }
    return 0;
}

int rns_link_encrypt(const rns_link *link, const uint8_t *plaintext, size_t length,
                     uint8_t *out, size_t capacity, size_t *out_length) {
    return link && link->state == RNS_LINK_ACTIVE && rns_token_encrypt(link->derived_key, plaintext, length, out, capacity, out_length);
}
int rns_link_decrypt(const rns_link *link, const uint8_t *token, size_t length,
                     uint8_t *out, size_t capacity, size_t *out_length) {
    return link && (link->state == RNS_LINK_ACTIVE || link->state == RNS_LINK_HANDSHAKE) &&
           rns_token_decrypt(link->derived_key, token, length, out, capacity, out_length);
}
