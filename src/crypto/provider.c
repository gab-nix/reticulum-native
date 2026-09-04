#include "reticulum/crypto.h"

#ifdef RNS_CRYPTO_OPENSSL_DEFAULT
#include "provider_internal.h"
#endif

static const rns_crypto_provider_t *installed_provider;

static const rns_crypto_provider_t *crypto_provider(void) {
#ifdef RNS_CRYPTO_OPENSSL_DEFAULT
    return installed_provider != NULL ? installed_provider
                                      : rns_openssl_crypto_provider();
#else
    return installed_provider;
#endif
}

rns_status_t rns_crypto_provider_install(const rns_crypto_provider_t *provider) {
    if (provider == NULL || provider->sha256 == NULL ||
        provider->hmac_sha256 == NULL || provider->hkdf_sha256 == NULL ||
        provider->random_bytes == NULL || provider->x25519_generate == NULL ||
        provider->x25519_public_from_private == NULL ||
        provider->x25519_exchange == NULL || provider->ed25519_generate == NULL ||
        provider->ed25519_public_from_private == NULL ||
        provider->ed25519_sign == NULL || provider->ed25519_verify == NULL ||
        provider->constant_time_equal == NULL ||
        provider->token_encrypt == NULL || provider->token_decrypt == NULL) {
        return RNS_ERROR_INVALID_ARGUMENT;
    }
    if (installed_provider != NULL && installed_provider != provider)
        return RNS_ERROR_INVALID_STATE;
    installed_provider = provider;
    return RNS_OK;
}

void rns_crypto_provider_restore_default(void) { installed_provider = NULL; }

const rns_crypto_provider_t *rns_crypto_provider_current(void) {
    return crypto_provider();
}

int rns_sha256(const uint8_t *data, size_t length, uint8_t out[32]) {
    const rns_crypto_provider_t *provider = crypto_provider();
    return provider != NULL && provider->sha256(provider->context, data, length, out);
}

int rns_hmac_sha256(const uint8_t *key, size_t key_length, const uint8_t *data,
                    size_t data_length, uint8_t out[32]) {
    const rns_crypto_provider_t *provider = crypto_provider();
    return provider != NULL &&
           provider->hmac_sha256(provider->context, key, key_length,
                                 data, data_length, out);
}

int rns_hkdf_sha256(const uint8_t *input, size_t input_length, const uint8_t *salt,
                    size_t salt_length, const uint8_t *context, size_t context_length,
                    uint8_t *out, size_t out_length) {
    const rns_crypto_provider_t *provider = crypto_provider();
    return provider != NULL &&
           provider->hkdf_sha256(provider->context, input, input_length,
                                 salt, salt_length, context, context_length,
                                 out, out_length);
}

int rns_random_bytes(uint8_t *out, size_t length) {
    const rns_crypto_provider_t *provider = crypto_provider();
    return provider != NULL && provider->random_bytes(provider->context, out, length);
}

int rns_x25519_generate(uint8_t private_key[32], uint8_t public_key[32]) {
    const rns_crypto_provider_t *provider = crypto_provider();
    return provider != NULL &&
           provider->x25519_generate(provider->context, private_key, public_key);
}

int rns_x25519_public_from_private(const uint8_t private_key[32], uint8_t public_key[32]) {
    const rns_crypto_provider_t *provider = crypto_provider();
    return provider != NULL &&
           provider->x25519_public_from_private(provider->context, private_key, public_key);
}

int rns_x25519_exchange(const uint8_t private_key[32], const uint8_t peer_public[32],
                        uint8_t shared[32]) {
    const rns_crypto_provider_t *provider = crypto_provider();
    return provider != NULL &&
           provider->x25519_exchange(provider->context, private_key, peer_public, shared);
}

int rns_ed25519_generate(uint8_t private_key[32], uint8_t public_key[32]) {
    const rns_crypto_provider_t *provider = crypto_provider();
    return provider != NULL &&
           provider->ed25519_generate(provider->context, private_key, public_key);
}

int rns_ed25519_public_from_private(const uint8_t private_key[32], uint8_t public_key[32]) {
    const rns_crypto_provider_t *provider = crypto_provider();
    return provider != NULL &&
           provider->ed25519_public_from_private(provider->context, private_key, public_key);
}

int rns_ed25519_sign(const uint8_t private_key[32], const uint8_t *message,
                     size_t message_length, uint8_t signature[64]) {
    const rns_crypto_provider_t *provider = crypto_provider();
    return provider != NULL &&
           provider->ed25519_sign(provider->context, private_key, message,
                                  message_length, signature);
}

int rns_ed25519_verify(const uint8_t public_key[32], const uint8_t *message,
                       size_t message_length, const uint8_t signature[64]) {
    const rns_crypto_provider_t *provider = crypto_provider();
    return provider != NULL &&
           provider->ed25519_verify(provider->context, public_key, message,
                                    message_length, signature);
}

int rns_constant_time_equal(const uint8_t *left, const uint8_t *right,
                            size_t length) {
    const rns_crypto_provider_t *provider = crypto_provider();
    return provider != NULL &&
           provider->constant_time_equal(provider->context, left, right, length);
}

int rns_token_encrypt(const uint8_t key[64], const uint8_t *plaintext,
                      size_t plaintext_length, uint8_t *out, size_t out_capacity,
                      size_t *out_length) {
    const rns_crypto_provider_t *provider = crypto_provider();
    return provider != NULL &&
           provider->token_encrypt(provider->context, key, plaintext,
                                   plaintext_length, out, out_capacity, out_length);
}

int rns_token_decrypt(const uint8_t key[64], const uint8_t *token,
                      size_t token_length, uint8_t *out, size_t out_capacity,
                      size_t *out_length) {
    const rns_crypto_provider_t *provider = crypto_provider();
    return provider != NULL &&
           provider->token_decrypt(provider->context, key, token, token_length,
                                   out, out_capacity, out_length);
}
