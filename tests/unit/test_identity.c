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
    ciphertext[ciphertext_length - 1] ^= 1;
    assert(!rns_identity_decrypt(&a, ciphertext, ciphertext_length, plaintext,
                                 sizeof(plaintext), &plaintext_length));
    assert(!rns_identity_decrypt(&public_only, ciphertext, ciphertext_length, plaintext,
                                 sizeof(plaintext), &plaintext_length));
    return 0;
}
