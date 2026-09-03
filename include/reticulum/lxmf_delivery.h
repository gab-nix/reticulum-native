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

#endif
