#include "reticulum/lxmf_delivery.h"

#include "reticulum/destination.h"
#include "reticulum/packet.h"

#include <string.h>

static int delivery_hash(const rns_identity *identity, uint8_t out[16]) {
    static const char *aspects[] = {"delivery"};
    return rns_destination_hash(identity, "lxmf", aspects, 1, out);
}

lxmf_status_t lxmf_opportunistic_packet_pack(
    lxmf_message_t *message,
    const rns_identity *source_identity,
    const rns_identity *destination_identity,
    uint8_t *packet, size_t packet_capacity, size_t *packet_length) {
    uint8_t packed_lxmf[RNS_MTU];
    uint8_t encrypted[RNS_MTU];
    uint8_t expected_source[16];
    uint8_t expected_destination[16];
    size_t packed_length = 0;
    size_t encrypted_length = 0;
    rns_packet outer = {0};
    lxmf_status_t status;

    if (!message || !source_identity || !source_identity->has_private ||
        !destination_identity || !packet || !packet_length) return LXMF_ERR_ARGUMENT;
    if (!delivery_hash(source_identity, expected_source) ||
        !delivery_hash(destination_identity, expected_destination)) return LXMF_ERR_CRYPTO;
    if (memcmp(message->source, expected_source, 16) != 0 ||
        memcmp(message->destination, expected_destination, 16) != 0) return LXMF_ERR_ARGUMENT;

    status = lxmf_pack(message, lxmf_identity_signer, (void *)source_identity,
                       packed_lxmf, sizeof(packed_lxmf), &packed_length);
    if (status != LXMF_OK) return status;
    /* In opportunistic LXMF the Reticulum destination already carries the
     * first 16 bytes of the packed message. Python LXMF therefore encrypts
     * only source || signature || payload and reconstructs the destination
     * prefix at the receiver. */
    if (packed_length < LXMF_DESTINATION_LENGTH ||
        !rns_identity_encrypt(destination_identity, NULL,
                              packed_lxmf + LXMF_DESTINATION_LENGTH,
                              packed_length - LXMF_DESTINATION_LENGTH,
                              encrypted, sizeof(encrypted), &encrypted_length)) {
        return LXMF_ERR_BOUNDS;
    }

    memcpy(outer.destination_hash, expected_destination, 16);
    outer.destination_type = 0;
    outer.packet_type = 0;
    outer.context = 0;
    outer.data = encrypted;
    outer.data_length = encrypted_length;
    if (!rns_packet_encode(&outer, packet, packet_capacity, packet_length)) {
        return LXMF_ERR_BOUNDS;
    }
    return LXMF_OK;
}

lxmf_status_t lxmf_opportunistic_packet_unpack(
    const uint8_t *packet, size_t packet_length,
    const rns_identity *local_identity,
    lxmf_verify_fn verifier, void *verify_context,
    uint8_t *plaintext, size_t plaintext_capacity, size_t *plaintext_length,
    lxmf_message_t *message) {
    uint8_t expected_destination[16];
    rns_packet outer;

    if (!packet || !local_identity || !local_identity->has_private || !plaintext ||
        !plaintext_length || !message) return LXMF_ERR_ARGUMENT;
    *plaintext_length = 0u;
    if (plaintext_capacity < LXMF_DESTINATION_LENGTH) return LXMF_ERR_BOUNDS;
    if (!rns_packet_decode(&outer, packet, packet_length) || outer.header_type != 0 ||
        outer.destination_type != 0 || outer.packet_type != 0 || outer.context != 0) {
        return LXMF_ERR_FORMAT;
    }
    if (!delivery_hash(local_identity, expected_destination)) return LXMF_ERR_CRYPTO;
    if (memcmp(outer.destination_hash, expected_destination, 16) != 0) {
        return LXMF_ERR_FORMAT;
    }
    size_t decrypted_length = 0u;
    memcpy(plaintext, outer.destination_hash, LXMF_DESTINATION_LENGTH);
    if (!rns_identity_decrypt(local_identity, outer.data, outer.data_length,
                              plaintext + LXMF_DESTINATION_LENGTH,
                              plaintext_capacity - LXMF_DESTINATION_LENGTH,
                              &decrypted_length)) {
        return LXMF_ERR_CRYPTO;
    }
    if (decrypted_length > plaintext_capacity - LXMF_DESTINATION_LENGTH)
        return LXMF_ERR_BOUNDS;
    *plaintext_length = LXMF_DESTINATION_LENGTH + decrypted_length;
    return lxmf_unpack(plaintext, *plaintext_length, verifier, verify_context, message);
}
