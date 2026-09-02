#ifndef RETICULUM_PROOF_H
#define RETICULUM_PROOF_H

#include <stddef.h>
#include <stdint.h>

#include "reticulum/identity.h"

#define RNS_PROOF_HASH_SIZE 32u
#define RNS_PROOF_SIGNATURE_SIZE 64u
#define RNS_PROOF_IMPLICIT_SIZE RNS_PROOF_SIGNATURE_SIZE
#define RNS_PROOF_EXPLICIT_SIZE (RNS_PROOF_HASH_SIZE + RNS_PROOF_SIGNATURE_SIZE)

int rns_proof_generate_explicit(const rns_identity *identity,
                                const uint8_t packet_hash[32],
                                uint8_t proof[RNS_PROOF_EXPLICIT_SIZE]);
int rns_proof_generate_implicit(const rns_identity *identity,
                                const uint8_t packet_hash[32],
                                uint8_t proof[RNS_PROOF_IMPLICIT_SIZE]);
int rns_proof_validate(const rns_identity *identity,
                       const uint8_t expected_packet_hash[32],
                       const uint8_t *proof, size_t proof_length);

#endif
