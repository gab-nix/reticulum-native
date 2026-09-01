#ifndef RETICULUM_ANNOUNCE_H
#define RETICULUM_ANNOUNCE_H

#include <stddef.h>
#include <stdint.h>

#include "reticulum/identity.h"

#define RNS_ANNOUNCE_RANDOM_SIZE 10u
#define RNS_ANNOUNCE_RATCHET_SIZE 32u
#define RNS_ANNOUNCE_SIGNATURE_SIZE 64u
#define RNS_ANNOUNCE_MIN_BODY_SIZE 148u
#define RNS_ANNOUNCE_RATCHET_BODY_SIZE 180u
#define RNS_ANNOUNCE_MAX_BODY_SIZE 465u
#define RNS_ANNOUNCE_MAX_TIMESTAMP UINT64_C(0xffffffffff)

/* Parsed fields point into the original body and remain valid for its lifetime. */
typedef struct {
    const uint8_t *public_key;
    const uint8_t *name_hash;
    const uint8_t *random_blob;
    const uint8_t *ratchet;
    const uint8_t *signature;
    const uint8_t *app_data;
    size_t app_data_length;
    uint64_t timestamp;
    int has_ratchet;
} rns_announce;

/*
 * Builds an announce body. random_prefix is the five random bytes preceding the
 * unsigned, big-endian 40-bit Unix timestamp. ratchet may be NULL.
 */
int rns_announce_build(const rns_identity *identity,
                       const uint8_t destination_hash[16],
                       const uint8_t name_hash[10],
                       const uint8_t random_prefix[5],
                       uint64_t timestamp,
                       const uint8_t ratchet[32],
                       const uint8_t *app_data, size_t app_data_length,
                       uint8_t *out, size_t out_capacity, size_t *out_length,
                       uint8_t *context_flag);

int rns_announce_parse(rns_announce *announce, const uint8_t *body,
                       size_t body_length, uint8_t context_flag);

/* Validates destination derivation and the Ed25519 announce signature. */
int rns_announce_verify(const uint8_t destination_hash[16],
                        const uint8_t *body, size_t body_length,
                        uint8_t context_flag);

#endif
