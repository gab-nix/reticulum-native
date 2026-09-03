#ifndef RETICULUM_LXMF_DELIVERY_H
#define RETICULUM_LXMF_DELIVERY_H

#include <stddef.h>
#include <stdint.h>

#include "reticulum/lxmf.h"
#include "reticulum/identity.h"

lxmf_status_t lxmf_opportunistic_packet_pack(
    lxmf_message_t *message,
    const rns_identity *source_identity,
    const rns_identity *destination_identity,
    uint8_t *packet, size_t packet_capacity, size_t *packet_length);

/* As above, but encrypts to the verified ratchet public key advertised by the
 * destination. A NULL ratchet is equivalent to the compatibility wrapper. */
lxmf_status_t lxmf_opportunistic_packet_pack_ratchet(
    lxmf_message_t *message, const rns_identity *source_identity,
    const rns_identity *destination_identity,
    const uint8_t ratchet_public[RNS_RATCHET_PUBLIC_SIZE],
    uint8_t *packet, size_t packet_capacity, size_t *packet_length);

/* Decrypts and unpacks one opportunistic packet. `plaintext` receives the
 * complete packed LXMF representation, including the destination prefix that
 * is carried by the outer Reticulum packet instead of its ciphertext.
 * LXMF_ERR_UNKNOWN_SIGNER means the payload parsed and the message is
 * populated, but the verifier holds no identity for its source, so the
 * signature is neither confirmed nor refuted. */
lxmf_status_t lxmf_opportunistic_packet_unpack(
    const uint8_t *packet, size_t packet_length,
    const rns_identity *local_identity,
    lxmf_verify_fn verifier, void *verify_context,
    uint8_t *plaintext, size_t plaintext_capacity, size_t *plaintext_length,
    lxmf_message_t *message);

/* Tries newest-first private ratchets before identity-key fallback. Set
 * enforce_ratchets to reject identity-key ciphertext. */
lxmf_status_t lxmf_opportunistic_packet_unpack_ratchets(
    const uint8_t *packet, size_t packet_length,
    const rns_identity *local_identity, const uint8_t *ratchet_private_keys,
    size_t ratchet_count, int enforce_ratchets,
    lxmf_verify_fn verifier, void *verify_context,
    uint8_t *plaintext, size_t plaintext_capacity, size_t *plaintext_length,
    lxmf_message_t *message, uint8_t ratchet_id[RNS_RATCHET_ID_SIZE],
    int *used_ratchet);

#endif
