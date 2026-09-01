#ifndef RETICULUM_IDENTITY_H
#define RETICULUM_IDENTITY_H

#include <stddef.h>
#include <stdint.h>

#define RNS_IDENTITY_PRIVATE_SIZE 64u
#define RNS_IDENTITY_PUBLIC_SIZE 64u
#define RNS_TRUNCATED_HASH_SIZE 16u

typedef struct {
    uint8_t encryption_private[32];
    uint8_t signing_private[32];
    uint8_t encryption_public[32];
    uint8_t signing_public[32];
    uint8_t hash[16];
    int has_private;
} rns_identity;

int rns_identity_generate(rns_identity *identity);
int rns_identity_from_private(rns_identity *identity, const uint8_t private_key[64]);
int rns_identity_from_public(rns_identity *identity, const uint8_t public_key[64]);
int rns_identity_export_private(const rns_identity *identity, uint8_t out[64]);
void rns_identity_export_public(const rns_identity *identity, uint8_t out[64]);
int rns_identity_sign(const rns_identity *identity, const uint8_t *message, size_t message_length,
                      uint8_t signature[64]);
int rns_identity_verify(const rns_identity *identity, const uint8_t *message, size_t message_length,
                        const uint8_t signature[64]);

#endif
