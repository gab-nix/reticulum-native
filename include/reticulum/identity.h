#ifndef RETICULUM_IDENTITY_H
#define RETICULUM_IDENTITY_H

#include <stddef.h>
#include <stdint.h>

#define RNS_IDENTITY_PRIVATE_SIZE 64u
#define RNS_IDENTITY_PUBLIC_SIZE 64u
#define RNS_TRUNCATED_HASH_SIZE 16u
#define RNS_RATCHET_PRIVATE_SIZE 32u
#define RNS_RATCHET_PUBLIC_SIZE 32u
#define RNS_RATCHET_ID_SIZE 16u

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
size_t rns_identity_encrypt_bound(size_t plaintext_length);
int rns_identity_encrypt(const rns_identity *identity, const uint8_t ratchet_public[32],
                         const uint8_t *plaintext, size_t plaintext_length,
                         uint8_t *out, size_t out_capacity, size_t *out_length);
int rns_identity_decrypt(const rns_identity *identity,
                         const uint8_t *ciphertext, size_t ciphertext_length,
                         uint8_t *out, size_t out_capacity, size_t *out_length);

/* Generates an X25519 ratchet and its protocol ID, which is the first 16 bytes
 * of SHA-256(public_key). The private key must be persisted securely by the
 * caller when announced ratchets are enabled. */
int rns_identity_ratchet_generate(
    uint8_t private_key[RNS_RATCHET_PRIVATE_SIZE],
    uint8_t public_key[RNS_RATCHET_PUBLIC_SIZE],
    uint8_t ratchet_id[RNS_RATCHET_ID_SIZE]);
void rns_identity_ratchet_id(
    const uint8_t public_key[RNS_RATCHET_PUBLIC_SIZE],
    uint8_t ratchet_id[RNS_RATCHET_ID_SIZE]);

/* Tries caller-supplied private ratchets in array order before the identity
 * key. When enforce_ratchets is non-zero, identity-key fallback is disabled.
 * ratchet_private_keys is a contiguous ratchet_count * 32-byte array. */
int rns_identity_decrypt_with_ratchets(
    const rns_identity *identity, const uint8_t *ratchet_private_keys,
    size_t ratchet_count, int enforce_ratchets,
    const uint8_t *ciphertext, size_t ciphertext_length,
    uint8_t *out, size_t out_capacity, size_t *out_length,
    uint8_t ratchet_id[RNS_RATCHET_ID_SIZE], int *used_ratchet);

#endif
