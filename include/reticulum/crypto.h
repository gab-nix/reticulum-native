#ifndef RETICULUM_CRYPTO_H
#define RETICULUM_CRYPTO_H

#include <stddef.h>
#include <stdint.h>

#include "reticulum/status.h"

#define RNS_SHA256_SIZE 32u
#define RNS_X25519_KEY_SIZE 32u
#define RNS_ED25519_KEY_SIZE 32u
#define RNS_ED25519_SIGNATURE_SIZE 64u
#define RNS_TOKEN_OVERHEAD 48u

typedef struct rns_crypto_provider {
    void *context;
    int (*sha256)(void *context, const uint8_t *data, size_t length,
                  uint8_t out[RNS_SHA256_SIZE]);
    int (*hmac_sha256)(void *context, const uint8_t *key, size_t key_length,
                       const uint8_t *data, size_t data_length,
                       uint8_t out[RNS_SHA256_SIZE]);
    int (*hkdf_sha256)(void *context, const uint8_t *input, size_t input_length,
                       const uint8_t *salt, size_t salt_length,
                       const uint8_t *info, size_t info_length,
                       uint8_t *out, size_t out_length);
    int (*random_bytes)(void *context, uint8_t *out, size_t length);
    int (*x25519_generate)(void *context, uint8_t private_key[32],
                           uint8_t public_key[32]);
    int (*x25519_public_from_private)(void *context,
                                      const uint8_t private_key[32],
                                      uint8_t public_key[32]);
    int (*x25519_exchange)(void *context, const uint8_t private_key[32],
                           const uint8_t peer_public[32], uint8_t shared[32]);
    int (*ed25519_generate)(void *context, uint8_t private_key[32],
                            uint8_t public_key[32]);
    int (*ed25519_public_from_private)(void *context,
                                       const uint8_t private_key[32],
                                       uint8_t public_key[32]);
    int (*ed25519_sign)(void *context, const uint8_t private_key[32],
                        const uint8_t *message, size_t message_length,
                        uint8_t signature[64]);
    int (*ed25519_verify)(void *context, const uint8_t public_key[32],
                          const uint8_t *message, size_t message_length,
                          const uint8_t signature[64]);
    int (*token_encrypt)(void *context, const uint8_t key[64],
                         const uint8_t *plaintext, size_t plaintext_length,
                         uint8_t *out, size_t out_capacity, size_t *out_length);
    int (*token_decrypt)(void *context, const uint8_t key[64],
                         const uint8_t *token, size_t token_length,
                         uint8_t *out, size_t out_capacity, size_t *out_length);
} rns_crypto_provider_t;

/* The provider is borrowed and must remain valid until reset. Install it
 * before protocol objects are created. */
rns_status_t rns_crypto_provider_install(const rns_crypto_provider_t *provider);
void rns_crypto_provider_restore_default(void);
const rns_crypto_provider_t *rns_crypto_provider_current(void);

int rns_sha256(const uint8_t *data, size_t length, uint8_t out[RNS_SHA256_SIZE]);
int rns_hmac_sha256(const uint8_t *key, size_t key_length,
                    const uint8_t *data, size_t data_length,
                    uint8_t out[RNS_SHA256_SIZE]);
int rns_hkdf_sha256(const uint8_t *input, size_t input_length,
                    const uint8_t *salt, size_t salt_length,
                    const uint8_t *context, size_t context_length,
                    uint8_t *out, size_t out_length);
int rns_random_bytes(uint8_t *out, size_t length);

int rns_x25519_generate(uint8_t private_key[32], uint8_t public_key[32]);
int rns_x25519_public_from_private(const uint8_t private_key[32], uint8_t public_key[32]);
int rns_x25519_exchange(const uint8_t private_key[32], const uint8_t peer_public[32], uint8_t shared[32]);
int rns_ed25519_generate(uint8_t private_key[32], uint8_t public_key[32]);
int rns_ed25519_public_from_private(const uint8_t private_key[32], uint8_t public_key[32]);
int rns_ed25519_sign(const uint8_t private_key[32], const uint8_t *message, size_t message_length,
                     uint8_t signature[64]);
int rns_ed25519_verify(const uint8_t public_key[32], const uint8_t *message, size_t message_length,
                       const uint8_t signature[64]);

/* Reticulum's modified Fernet token: IV || AES-256-CBC(PKCS7) || HMAC-SHA256. */
int rns_token_encrypt(const uint8_t key[64], const uint8_t *plaintext, size_t plaintext_length,
                      uint8_t *out, size_t out_capacity, size_t *out_length);
int rns_token_decrypt(const uint8_t key[64], const uint8_t *token, size_t token_length,
                      uint8_t *out, size_t out_capacity, size_t *out_length);

#endif
