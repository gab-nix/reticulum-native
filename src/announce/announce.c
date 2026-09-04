#include "reticulum/announce.h"

#include "reticulum/crypto.h"

#include <stdlib.h>
#include <string.h>

static uint64_t decode_timestamp(const uint8_t blob[10]) {
    uint64_t value = 0;
    for (size_t i = 5; i < 10; ++i) value = (value << 8) | blob[i];
    return value;
}

static void encode_timestamp(uint64_t value, uint8_t out[5]) {
    for (size_t i = 5; i > 0; --i) { out[i - 1] = (uint8_t)value; value >>= 8; }
}

int rns_announce_parse(rns_announce *announce, const uint8_t *body,
                       size_t body_length, uint8_t context_flag) {
    size_t signature_offset;
    if (!announce || !body || context_flag > 1) return 0;
    if (body_length > RNS_ANNOUNCE_MAX_BODY_SIZE ||
        (!context_flag && body_length < RNS_ANNOUNCE_MIN_BODY_SIZE) ||
        (context_flag && body_length < RNS_ANNOUNCE_RATCHET_BODY_SIZE)) return 0;
    memset(announce, 0, sizeof(*announce));
    announce->public_key = body;
    announce->name_hash = body + 64;
    announce->random_blob = body + 74;
    announce->has_ratchet = context_flag != 0;
    signature_offset = 84;
    if (announce->has_ratchet) { announce->ratchet = body + signature_offset; signature_offset += 32; }
    announce->signature = body + signature_offset;
    announce->app_data = body + signature_offset + 64;
    announce->app_data_length = body_length - signature_offset - 64;
    announce->timestamp = decode_timestamp(announce->random_blob);
    return 1;
}

int rns_announce_build(const rns_identity *identity, const uint8_t destination_hash[16],
                       const uint8_t name_hash[10], const uint8_t random_prefix[5],
                       uint64_t timestamp, const uint8_t ratchet[32],
                       const uint8_t *app_data, size_t app_data_length,
                       uint8_t *out, size_t out_capacity, size_t *out_length,
                       uint8_t *context_flag) {
    uint8_t public_key[64], *signed_data = NULL, signature[64];
    size_t prefix_length, body_length, offset = 0, signed_length;
    int ok = 0;
    if (!identity || !identity->has_private || !destination_hash || !name_hash || !random_prefix ||
        !out || !out_length || !context_flag || timestamp > RNS_ANNOUNCE_MAX_TIMESTAMP ||
        (app_data_length && !app_data)) return 0;
    prefix_length = ratchet ? 116u : 84u;
    if (app_data_length > SIZE_MAX - prefix_length - 64u) return 0;
    body_length = prefix_length + 64u + app_data_length;
    if (body_length > RNS_ANNOUNCE_MAX_BODY_SIZE || out_capacity < body_length) return 0;
    rns_identity_export_public(identity, public_key);
    memcpy(out + offset, public_key, 64); offset += 64;
    memcpy(out + offset, name_hash, 10); offset += 10;
    memcpy(out + offset, random_prefix, 5); offset += 5;
    encode_timestamp(timestamp, out + offset); offset += 5;
    if (ratchet) { memcpy(out + offset, ratchet, 32); offset += 32; }

    /* Signature coverage is destination_hash || body-prefix || app_data. */
    if (app_data_length > SIZE_MAX - 16u - offset) return 0;
    signed_length = 16u + offset + app_data_length;
    signed_data = malloc(signed_length);
    if (!signed_data) return 0;
    memcpy(signed_data, destination_hash, 16);
    memcpy(signed_data + 16, out, offset);
    if (app_data_length) memcpy(signed_data + 16 + offset, app_data, app_data_length);
    if (!rns_identity_sign(identity, signed_data, signed_length, signature)) goto done;
    memcpy(out + offset, signature, 64); offset += 64;
    if (app_data_length) memcpy(out + offset, app_data, app_data_length);
    *out_length = body_length; *context_flag = ratchet ? 1u : 0u; ok = 1;
done:
    free(signed_data); return ok;
}

int rns_announce_verify(const uint8_t destination_hash[16], const uint8_t *body,
                        size_t body_length, uint8_t context_flag) {
    rns_announce announce; rns_identity identity; uint8_t material[26], digest[32];
    uint8_t *signed_data = NULL; size_t prefix_length, signed_length; int ok = 0;
    if (!destination_hash || !rns_announce_parse(&announce, body, body_length, context_flag)) return 0;
    if (!rns_identity_from_public(&identity, announce.public_key)) return 0;
    memcpy(material, announce.name_hash, 10); memcpy(material + 10, identity.hash, 16);
    if (!rns_sha256(material, sizeof(material), digest) || memcmp(digest, destination_hash, 16) != 0) return 0;
    prefix_length = announce.has_ratchet ? 116u : 84u;
    if (announce.app_data_length > SIZE_MAX - 16u - prefix_length) return 0;
    signed_length = 16u + prefix_length + announce.app_data_length;
    signed_data = malloc(signed_length);
    if (!signed_data) return 0;
    memcpy(signed_data, destination_hash, 16);
    memcpy(signed_data + 16, body, prefix_length);
    if (announce.app_data_length)
        memcpy(signed_data + 16 + prefix_length, announce.app_data, announce.app_data_length);
    ok = rns_identity_verify(&identity, signed_data, signed_length, announce.signature);
    free(signed_data); return ok;
}
