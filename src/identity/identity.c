#include "reticulum/identity.h"
#include "reticulum/crypto.h"

#include <string.h>
#include <openssl/crypto.h>

static int update_hash(rns_identity *identity) {
    uint8_t pub[64], digest[32];
    memcpy(pub, identity->encryption_public, 32); memcpy(pub + 32, identity->signing_public, 32);
    if (!rns_sha256(pub, sizeof(pub), digest)) return 0;
    memcpy(identity->hash, digest, 16); return 1;
}

int rns_identity_generate(rns_identity *identity) {
    if (!identity) return 0; memset(identity, 0, sizeof(*identity));
    if (!rns_x25519_generate(identity->encryption_private, identity->encryption_public) ||
        !rns_ed25519_generate(identity->signing_private, identity->signing_public)) {
        memset(identity, 0, sizeof(*identity)); return 0;
    }
    identity->has_private = 1; return update_hash(identity);
}

int rns_identity_from_private(rns_identity *identity, const uint8_t private_key[64]) {
    if (!identity || !private_key) return 0; memset(identity, 0, sizeof(*identity));
    memcpy(identity->encryption_private, private_key, 32); memcpy(identity->signing_private, private_key + 32, 32);
    if (!rns_x25519_public_from_private(identity->encryption_private, identity->encryption_public) ||
        !rns_ed25519_public_from_private(identity->signing_private, identity->signing_public)) {
        memset(identity, 0, sizeof(*identity)); return 0;
    }
    identity->has_private = 1; return update_hash(identity);
}

int rns_identity_from_public(rns_identity *identity, const uint8_t public_key[64]) {
    if (!identity || !public_key) return 0; memset(identity, 0, sizeof(*identity));
    memcpy(identity->encryption_public, public_key, 32); memcpy(identity->signing_public, public_key + 32, 32);
    return update_hash(identity);
}

int rns_identity_export_private(const rns_identity *identity, uint8_t out[64]) {
    if (!identity || !out || !identity->has_private) return 0;
    memcpy(out, identity->encryption_private, 32); memcpy(out + 32, identity->signing_private, 32); return 1;
}

void rns_identity_export_public(const rns_identity *identity, uint8_t out[64]) {
    if (!identity || !out) return;
    memcpy(out, identity->encryption_public, 32); memcpy(out + 32, identity->signing_public, 32);
}

int rns_identity_sign(const rns_identity *identity, const uint8_t *message, size_t message_length, uint8_t signature[64]) {
    return identity && identity->has_private && rns_ed25519_sign(identity->signing_private, message, message_length, signature);
}

int rns_identity_verify(const rns_identity *identity, const uint8_t *message, size_t message_length, const uint8_t signature[64]) {
    return identity && rns_ed25519_verify(identity->signing_public, message, message_length, signature);
}

size_t rns_identity_encrypt_bound(size_t plaintext_length) {
    size_t blocks;
    if (plaintext_length > SIZE_MAX - 16u) return 0;
    blocks = (plaintext_length / 16u) + 1u;
    if (blocks > (SIZE_MAX - 80u) / 16u) return 0;
    return 80u + blocks * 16u;
}

int rns_identity_encrypt(const rns_identity *identity, const uint8_t ratchet_public[32],
                         const uint8_t *plaintext, size_t plaintext_length,
                         uint8_t *out, size_t out_capacity, size_t *out_length) {
    uint8_t ephemeral_private[32], ephemeral_public[32], shared[32], derived[64];
    const uint8_t *target;
    size_t token_length = 0;
    size_t bound;
    int ok = 0;
    if (!identity || (!plaintext && plaintext_length) || !out || !out_length) return 0;
    bound = rns_identity_encrypt_bound(plaintext_length);
    if (bound == 0 || out_capacity < bound) return 0;
    target = ratchet_public ? ratchet_public : identity->encryption_public;
    if (!rns_x25519_generate(ephemeral_private, ephemeral_public) ||
        !rns_x25519_exchange(ephemeral_private, target, shared) ||
        !rns_hkdf_sha256(shared, sizeof(shared), identity->hash, sizeof(identity->hash),
                         NULL, 0, derived, sizeof(derived))) goto done;
    memcpy(out, ephemeral_public, sizeof(ephemeral_public));
    if (!rns_token_encrypt(derived, plaintext, plaintext_length, out + 32u,
                           out_capacity - 32u, &token_length)) goto done;
    *out_length = 32u + token_length;
    ok = 1;
done:
    OPENSSL_cleanse(ephemeral_private, sizeof(ephemeral_private));
    OPENSSL_cleanse(shared, sizeof(shared));
    OPENSSL_cleanse(derived, sizeof(derived));
    return ok;
}

int rns_identity_decrypt(const rns_identity *identity,
                         const uint8_t *ciphertext, size_t ciphertext_length,
                         uint8_t *out, size_t out_capacity, size_t *out_length) {
    return rns_identity_decrypt_with_ratchets(
        identity, NULL, 0u, 0, ciphertext, ciphertext_length, out,
        out_capacity, out_length, NULL, NULL);
}

void rns_identity_ratchet_id(const uint8_t public_key[32],
                             uint8_t ratchet_id[16]) {
    uint8_t digest[32];
    if (public_key == NULL || ratchet_id == NULL) return;
    memset(ratchet_id, 0, 16u);
    if (rns_sha256(public_key, 32u, digest)) memcpy(ratchet_id, digest, 16u);
    OPENSSL_cleanse(digest, sizeof digest);
}

int rns_identity_ratchet_generate(uint8_t private_key[32],
                                  uint8_t public_key[32],
                                  uint8_t ratchet_id[16]) {
    if (private_key == NULL || public_key == NULL || ratchet_id == NULL)
        return 0;
    if (!rns_x25519_generate(private_key, public_key)) return 0;
    rns_identity_ratchet_id(public_key, ratchet_id);
    return 1;
}

static int decrypt_with_private(const rns_identity *identity,
                                const uint8_t private_key[32],
                                const uint8_t *ciphertext,
                                size_t ciphertext_length, uint8_t *out,
                                size_t out_capacity, size_t *out_length) {
    uint8_t shared[32], derived[64];
    int ok = 0;
    if (!rns_x25519_exchange(private_key, ciphertext, shared) ||
        !rns_hkdf_sha256(shared, sizeof(shared), identity->hash, sizeof(identity->hash),
                         NULL, 0, derived, sizeof(derived))) goto done;
    ok = rns_token_decrypt(derived, ciphertext + 32u, ciphertext_length - 32u,
                           out, out_capacity, out_length);
done:
    OPENSSL_cleanse(shared, sizeof(shared));
    OPENSSL_cleanse(derived, sizeof(derived));
    return ok;
}

int rns_identity_decrypt_with_ratchets(
    const rns_identity *identity, const uint8_t *ratchet_private_keys,
    size_t ratchet_count, int enforce_ratchets, const uint8_t *ciphertext,
    size_t ciphertext_length, uint8_t *out, size_t out_capacity,
    size_t *out_length, uint8_t ratchet_id[16], int *used_ratchet) {
    if (out_length != NULL) *out_length = 0u;
    if (used_ratchet != NULL) *used_ratchet = 0;
    if (ratchet_id != NULL) memset(ratchet_id, 0, 16u);
    if (identity == NULL || !identity->has_private || ciphertext == NULL ||
        ciphertext_length <= 32u || out == NULL || out_length == NULL ||
        (ratchet_count != 0u && ratchet_private_keys == NULL) ||
        ratchet_count > SIZE_MAX / RNS_RATCHET_PRIVATE_SIZE)
        return 0;
    for (size_t i = 0u; i < ratchet_count; ++i) {
        const uint8_t *private_key =
            ratchet_private_keys + i * RNS_RATCHET_PRIVATE_SIZE;
        if (decrypt_with_private(identity, private_key, ciphertext,
                                 ciphertext_length, out, out_capacity,
                                 out_length)) {
            if (used_ratchet != NULL) *used_ratchet = 1;
            if (ratchet_id != NULL) {
                uint8_t public_key[32];
                if (!rns_x25519_public_from_private(private_key, public_key)) {
                    *out_length = 0u;
                    OPENSSL_cleanse(out, out_capacity);
                    return 0;
                }
                rns_identity_ratchet_id(public_key, ratchet_id);
                OPENSSL_cleanse(public_key, sizeof public_key);
            }
            return 1;
        }
        *out_length = 0u;
    }
    if (enforce_ratchets) return 0;
    return decrypt_with_private(identity, identity->encryption_private,
                                ciphertext, ciphertext_length, out,
                                out_capacity, out_length);
}
