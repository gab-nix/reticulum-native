#include "reticulum/identity.h"
#include <assert.h>
#include <string.h>

int main(void) {
    rns_identity a, b, public_only; uint8_t private_key[64], public_key[64], signature[64];
    uint8_t ciphertext[256], plaintext[64]; size_t ciphertext_length, plaintext_length;
    assert(rns_identity_generate(&a)); assert(rns_identity_export_private(&a, private_key)); rns_identity_export_public(&a, public_key);
    assert(rns_identity_from_private(&b, private_key)); assert(memcmp(a.hash, b.hash, 16) == 0);
    assert(rns_identity_from_public(&public_only, public_key)); assert(memcmp(a.hash, public_only.hash, 16) == 0);
    assert(!rns_identity_export_private(&public_only, private_key));
    assert(rns_identity_sign(&a, (const uint8_t *)"message", 7, signature));
    assert(rns_identity_verify(&public_only, (const uint8_t *)"message", 7, signature));
    assert(rns_identity_encrypt_bound(5) == 96);
    assert(rns_identity_encrypt(&public_only, NULL, (const uint8_t *)"hello", 5,
                                ciphertext, sizeof(ciphertext), &ciphertext_length));
    assert(ciphertext_length == 96);
    assert(rns_identity_decrypt(&a, ciphertext, ciphertext_length, plaintext,
                                sizeof(plaintext), &plaintext_length));
    assert(plaintext_length == 5 && memcmp(plaintext, "hello", 5) == 0);

    uint8_t ratchet_private[32], ratchet_public[32], ratchet_id[16];
    uint8_t used_id[16]; int used_ratchet = 0;
    assert(rns_identity_ratchet_generate(ratchet_private, ratchet_public,
                                         ratchet_id));
    uint8_t expected_id[16];
    rns_identity_ratchet_id(ratchet_public, expected_id);
    assert(memcmp(ratchet_id, expected_id, sizeof ratchet_id) == 0);
    assert(rns_identity_encrypt(&public_only, ratchet_public,
                                (const uint8_t *)"ratcheted", 9,
                                ciphertext, sizeof ciphertext,
                                &ciphertext_length));
    assert(!rns_identity_decrypt(&a, ciphertext, ciphertext_length, plaintext,
                                 sizeof plaintext, &plaintext_length));
    assert(rns_identity_decrypt_with_ratchets(
        &a, ratchet_private, 1u, 1, ciphertext, ciphertext_length, plaintext,
        sizeof plaintext, &plaintext_length, used_id, &used_ratchet));
    assert(used_ratchet && plaintext_length == 9u &&
           memcmp(plaintext, "ratcheted", 9u) == 0 &&
           memcmp(used_id, ratchet_id, sizeof used_id) == 0);

    uint8_t wrong_private[32], wrong_public[32], wrong_id[16];
    uint8_t ratchets[64];
    assert(rns_identity_ratchet_generate(wrong_private, wrong_public,
                                         wrong_id));
    memcpy(ratchets, wrong_private, 32u);
    memcpy(ratchets + 32u, ratchet_private, 32u);
    assert(rns_identity_decrypt_with_ratchets(
        &a, ratchets, 2u, 1, ciphertext, ciphertext_length, plaintext,
        sizeof plaintext, &plaintext_length, used_id, &used_ratchet));
    assert(used_ratchet && memcmp(used_id, ratchet_id, 16u) == 0);

    assert(rns_identity_encrypt(&public_only, NULL,
                                (const uint8_t *)"fallback", 8,
                                ciphertext, sizeof ciphertext,
                                &ciphertext_length));
    assert(!rns_identity_decrypt_with_ratchets(
        &a, ratchet_private, 1u, 1, ciphertext, ciphertext_length, plaintext,
        sizeof plaintext, &plaintext_length, used_id, &used_ratchet));
    assert(rns_identity_decrypt_with_ratchets(
        &a, ratchet_private, 1u, 0, ciphertext, ciphertext_length, plaintext,
        sizeof plaintext, &plaintext_length, used_id, &used_ratchet));
    assert(!used_ratchet && plaintext_length == 8u &&
           memcmp(plaintext, "fallback", 8u) == 0);

    ciphertext[ciphertext_length - 1] ^= 1;
    assert(!rns_identity_decrypt(&a, ciphertext, ciphertext_length, plaintext,
                                 sizeof(plaintext), &plaintext_length));
    assert(!rns_identity_decrypt(&public_only, ciphertext, ciphertext_length, plaintext,
                                 sizeof(plaintext), &plaintext_length));
    return 0;
}
