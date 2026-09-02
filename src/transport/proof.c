#include "reticulum/proof.h"

#include <string.h>

static int equal_ct(const uint8_t *a, const uint8_t *b, size_t n) {
    uint8_t different = 0; size_t i;
    for (i = 0; i < n; ++i) different |= (uint8_t)(a[i] ^ b[i]);
    return different == 0;
}

int rns_proof_generate_explicit(const rns_identity *identity,
                                const uint8_t packet_hash[32],
                                uint8_t proof[RNS_PROOF_EXPLICIT_SIZE]) {
    if (!identity || !packet_hash || !proof) return 0;
    memcpy(proof, packet_hash, 32);
    if (!rns_identity_sign(identity, packet_hash, 32, proof + 32)) { memset(proof, 0, 96); return 0; }
    return 1;
}

int rns_proof_generate_implicit(const rns_identity *identity,
                                const uint8_t packet_hash[32],
                                uint8_t proof[RNS_PROOF_IMPLICIT_SIZE]) {
    if (!identity || !packet_hash || !proof) return 0;
    return rns_identity_sign(identity, packet_hash, 32, proof);
}

int rns_proof_validate(const rns_identity *identity,
                       const uint8_t expected_packet_hash[32],
                       const uint8_t *proof, size_t proof_length) {
    const uint8_t *signature;
    if (!identity || !expected_packet_hash || !proof) return 0;
    if (proof_length == RNS_PROOF_EXPLICIT_SIZE) {
        if (!equal_ct(proof, expected_packet_hash, 32)) return 0;
        signature = proof + 32;
    } else if (proof_length == RNS_PROOF_IMPLICIT_SIZE) signature = proof;
    else return 0;
    return rns_identity_verify(identity, expected_packet_hash, 32, signature);
}
