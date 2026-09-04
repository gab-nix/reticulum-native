/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "reticulum/esp_idf.h"

#include <limits.h>
#include <stdint.h>
#include <string.h>

#include <sodium.h>

#include "reticulum/crypto.h"
#include "reticulum/hal.h"

#ifdef ESP_PLATFORM
#include "mbedtls/aes.h"
#include "mbedtls/hkdf.h"
#include "mbedtls/md.h"
#include "mbedtls/sha256.h"
#else
/* This shim lets desktop tests exercise the complete provider contract and
 * libsodium key semantics. Firmware always selects the mbedTLS branch. */
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/sha.h>
#endif

#define RNS_AES_BLOCK_SIZE 16U
#define RNS_TOKEN_MAC_SIZE 32U
#define RNS_ED25519_SECRET_SIZE 64U

static int valid_buffer(const uint8_t *data, size_t length) {
    return length == 0U || data != NULL;
}

static int backend_sha256(const uint8_t *data, size_t length, uint8_t out[32]) {
    static const uint8_t empty = 0U;
    if (out == NULL || !valid_buffer(data, length)) return 0;
    if (data == NULL) data = &empty;
#ifdef ESP_PLATFORM
    return mbedtls_sha256(data, length, out, 0) == 0;
#else
    return SHA256(data, length, out) != NULL;
#endif
}

static int backend_hmac(const uint8_t *key, size_t key_length,
                        const uint8_t *data, size_t data_length,
                        uint8_t out[32]) {
    static const uint8_t empty = 0U;
    if (out == NULL || !valid_buffer(key, key_length) ||
        !valid_buffer(data, data_length)) return 0;
    if (key == NULL) key = &empty;
    if (data == NULL) data = &empty;
#ifdef ESP_PLATFORM
    {
        const mbedtls_md_info_t *info =
            mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
        return info != NULL &&
               mbedtls_md_hmac(info, key, key_length, data, data_length, out) == 0;
    }
#else
    {
        unsigned int output_length = 0U;
        if (key_length > (size_t)INT_MAX) return 0;
        return HMAC(EVP_sha256(), key, (int)key_length, data, data_length,
                    out, &output_length) != NULL && output_length == 32U;
    }
#endif
}

static int backend_hkdf(const uint8_t *input, size_t input_length,
                        const uint8_t *salt, size_t salt_length,
                        const uint8_t *info, size_t info_length,
                        uint8_t *out, size_t out_length) {
    static const uint8_t empty = 0U;
    uint8_t zero_salt[32] = {0};
    if (out == NULL || out_length == 0U || out_length > 255U * 32U ||
        input_length == 0U || !valid_buffer(input, input_length) ||
        !valid_buffer(salt, salt_length) || !valid_buffer(info, info_length) ||
        info_length > 255U) return 0;
    if (salt == NULL || salt_length == 0U) {
        salt = zero_salt;
        salt_length = sizeof(zero_salt);
    }
    if (info == NULL) info = &empty;
#ifdef ESP_PLATFORM
    {
        const mbedtls_md_info_t *md =
            mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
        return md != NULL && mbedtls_hkdf(md, salt, salt_length, input,
                                          input_length, info, info_length,
                                          out, out_length) == 0;
    }
#else
    {
        uint8_t prk[32];
        uint8_t block[32];
        uint8_t scratch[32 + 255 + 1];
        size_t produced = 0U;
        size_t previous = 0U;
        uint8_t counter = 1U;
        int ok = 0;
        if (!backend_hmac(salt, salt_length, input, input_length, prk)) goto done;
        while (produced < out_length) {
            size_t message_length = 0U;
            size_t remaining;
            size_t take;
            if (previous != 0U) {
                memcpy(scratch, block, sizeof(block));
                message_length = sizeof(block);
            }
            if (info_length != 0U) {
                memcpy(scratch + message_length, info, info_length);
                message_length += info_length;
            }
            scratch[message_length++] = counter++;
            if (!backend_hmac(prk, sizeof(prk), scratch, message_length, block))
                goto done;
            remaining = out_length - produced;
            take = remaining < sizeof(block) ? remaining : sizeof(block);
            memcpy(out + produced, block, take);
            produced += take;
            previous = sizeof(block);
        }
        ok = 1;
done:
        sodium_memzero(prk, sizeof(prk));
        sodium_memzero(block, sizeof(block));
        sodium_memzero(scratch, sizeof(scratch));
        return ok;
    }
#endif
}

static int aes_cbc_encrypt(const uint8_t key[32], uint8_t iv[16],
                           uint8_t *data, size_t length) {
#ifdef ESP_PLATFORM
    mbedtls_aes_context context;
    int result;
    mbedtls_aes_init(&context);
    result = mbedtls_aes_setkey_enc(&context, key, 256U);
    if (result == 0)
        result = mbedtls_aes_crypt_cbc(&context, MBEDTLS_AES_ENCRYPT, length,
                                       iv, data, data);
    mbedtls_aes_free(&context);
    return result == 0;
#else
    EVP_CIPHER_CTX *context = EVP_CIPHER_CTX_new();
    int written = 0;
    int final_written = 0;
    int ok = context != NULL && length <= (size_t)INT_MAX &&
             EVP_EncryptInit_ex(context, EVP_aes_256_cbc(), NULL, key, iv) == 1 &&
             EVP_CIPHER_CTX_set_padding(context, 0) == 1 &&
             EVP_EncryptUpdate(context, data, &written, data, (int)length) == 1 &&
             EVP_EncryptFinal_ex(context, data + written, &final_written) == 1 &&
             (size_t)(written + final_written) == length;
    EVP_CIPHER_CTX_free(context);
    return ok;
#endif
}

static int aes_cbc_decrypt(const uint8_t key[32], uint8_t iv[16],
                           uint8_t *data, size_t length) {
#ifdef ESP_PLATFORM
    mbedtls_aes_context context;
    int result;
    mbedtls_aes_init(&context);
    result = mbedtls_aes_setkey_dec(&context, key, 256U);
    if (result == 0)
        result = mbedtls_aes_crypt_cbc(&context, MBEDTLS_AES_DECRYPT, length,
                                       iv, data, data);
    mbedtls_aes_free(&context);
    return result == 0;
#else
    EVP_CIPHER_CTX *context = EVP_CIPHER_CTX_new();
    int written = 0;
    int final_written = 0;
    int ok = context != NULL && length <= (size_t)INT_MAX &&
             EVP_DecryptInit_ex(context, EVP_aes_256_cbc(), NULL, key, iv) == 1 &&
             EVP_CIPHER_CTX_set_padding(context, 0) == 1 &&
             EVP_DecryptUpdate(context, data, &written, data, (int)length) == 1 &&
             EVP_DecryptFinal_ex(context, data + written, &final_written) == 1 &&
             (size_t)(written + final_written) == length;
    EVP_CIPHER_CTX_free(context);
    return ok;
#endif
}

static int esp_sha256(void *context, const uint8_t *data, size_t length,
                      uint8_t out[32]) {
    (void)context;
    return backend_sha256(data, length, out);
}

static int esp_hmac(void *context, const uint8_t *key, size_t key_length,
                    const uint8_t *data, size_t data_length, uint8_t out[32]) {
    (void)context;
    return backend_hmac(key, key_length, data, data_length, out);
}

static int esp_hkdf(void *context, const uint8_t *input, size_t input_length,
                    const uint8_t *salt, size_t salt_length,
                    const uint8_t *info, size_t info_length,
                    uint8_t *out, size_t out_length) {
    (void)context;
    return backend_hkdf(input, input_length, salt, salt_length, info,
                        info_length, out, out_length);
}

static int esp_random(void *context, uint8_t *out, size_t length) {
    (void)context;
    if (out == NULL) return 0;
    return rns_hal_random_bytes(out, length) == RNS_OK;
}

static int esp_x_public(void *context, const uint8_t private_key[32],
                        uint8_t public_key[32]) {
    (void)context;
    if (private_key == NULL || public_key == NULL) return 0;
    return crypto_scalarmult_curve25519_base(public_key, private_key) == 0;
}

static int esp_x_generate(void *context, uint8_t private_key[32],
                          uint8_t public_key[32]) {
    if (private_key == NULL || public_key == NULL) return 0;
    if (!esp_random(context, private_key, 32U)) {
        sodium_memzero(private_key, 32U);
        sodium_memzero(public_key, 32U);
        return 0;
    }
    if (esp_x_public(context, private_key, public_key)) return 1;
    sodium_memzero(private_key, 32U);
    sodium_memzero(public_key, 32U);
    return 0;
}

static int esp_x_exchange(void *context, const uint8_t private_key[32],
                          const uint8_t peer_public[32], uint8_t shared[32]) {
    int result;
    (void)context;
    if (private_key == NULL || peer_public == NULL || shared == NULL) return 0;
    result = crypto_scalarmult_curve25519(shared, private_key, peer_public);
    if (result != 0) sodium_memzero(shared, 32U);
    return result == 0;
}

static int esp_ed_public(void *context, const uint8_t private_key[32],
                         uint8_t public_key[32]) {
    uint8_t secret_key[RNS_ED25519_SECRET_SIZE];
    int result;
    (void)context;
    if (private_key == NULL || public_key == NULL) return 0;
    result = crypto_sign_seed_keypair(public_key, secret_key, private_key);
    sodium_memzero(secret_key, sizeof(secret_key));
    return result == 0;
}

static int esp_ed_generate(void *context, uint8_t private_key[32],
                           uint8_t public_key[32]) {
    if (private_key == NULL || public_key == NULL) return 0;
    if (!esp_random(context, private_key, 32U)) {
        sodium_memzero(private_key, 32U);
        sodium_memzero(public_key, 32U);
        return 0;
    }
    if (esp_ed_public(context, private_key, public_key)) return 1;
    sodium_memzero(private_key, 32U);
    sodium_memzero(public_key, 32U);
    return 0;
}

static int esp_ed_sign(void *context, const uint8_t private_key[32],
                       const uint8_t *message, size_t message_length,
                       uint8_t signature[64]) {
    uint8_t public_key[32];
    uint8_t secret_key[RNS_ED25519_SECRET_SIZE];
    unsigned long long signature_length = 0U;
    int result;
    (void)context;
    if (private_key == NULL || signature == NULL ||
        !valid_buffer(message, message_length)) return 0;
    if (crypto_sign_seed_keypair(public_key, secret_key, private_key) != 0) return 0;
    result = crypto_sign_detached(signature, &signature_length, message,
                                  (unsigned long long)message_length, secret_key);
    sodium_memzero(public_key, sizeof(public_key));
    sodium_memzero(secret_key, sizeof(secret_key));
    return result == 0 && signature_length == 64U;
}

static int esp_ed_verify(void *context, const uint8_t public_key[32],
                         const uint8_t *message, size_t message_length,
                         const uint8_t signature[64]) {
    (void)context;
    if (public_key == NULL || signature == NULL ||
        !valid_buffer(message, message_length)) return 0;
    return crypto_sign_verify_detached(signature, message,
                                       (unsigned long long)message_length,
                                       public_key) == 0;
}

static int esp_constant_time_equal(void *context, const uint8_t *left,
                                   const uint8_t *right, size_t length) {
    (void)context;
    if (length == 0U) return 1;
    if (left == NULL || right == NULL) return 0;
    return sodium_memcmp(left, right, length) == 0;
}

static int esp_token_encrypt(void *context, const uint8_t key[64],
                             const uint8_t *plaintext, size_t plaintext_length,
                             uint8_t *out, size_t out_capacity,
                             size_t *out_length) {
    uint8_t mac[32];
    uint8_t iv[16];
    size_t padded_length;
    size_t needed = 0U;
    uint8_t padding;
    int ok = 0;
    (void)context;
    if (out_length != NULL) *out_length = 0U;
    if (key == NULL || out == NULL || out_length == NULL ||
        !valid_buffer(plaintext, plaintext_length) ||
        plaintext_length > SIZE_MAX - RNS_AES_BLOCK_SIZE) return 0;
    padded_length = ((plaintext_length / RNS_AES_BLOCK_SIZE) + 1U) *
                    RNS_AES_BLOCK_SIZE;
    if (padded_length > SIZE_MAX - RNS_AES_BLOCK_SIZE - RNS_TOKEN_MAC_SIZE)
        return 0;
    needed = RNS_AES_BLOCK_SIZE + padded_length + RNS_TOKEN_MAC_SIZE;
    if (out_capacity < needed) return 0;
    if (!esp_random(NULL, out, RNS_AES_BLOCK_SIZE)) goto done;
    memcpy(iv, out, sizeof(iv));
    if (plaintext_length != 0U)
        memcpy(out + RNS_AES_BLOCK_SIZE, plaintext, plaintext_length);
    padding = (uint8_t)(padded_length - plaintext_length);
    memset(out + RNS_AES_BLOCK_SIZE + plaintext_length, padding, padding);
    if (!aes_cbc_encrypt(key + 32U, iv, out + RNS_AES_BLOCK_SIZE,
                         padded_length) ||
        !backend_hmac(key, 32U, out, RNS_AES_BLOCK_SIZE + padded_length, mac))
        goto done;
    memcpy(out + RNS_AES_BLOCK_SIZE + padded_length, mac, sizeof(mac));
    *out_length = needed;
    ok = 1;
done:
    sodium_memzero(mac, sizeof(mac));
    sodium_memzero(iv, sizeof(iv));
    if (!ok && needed != 0U) sodium_memzero(out, needed);
    return ok;
}

static int esp_token_decrypt(void *context, const uint8_t key[64],
                             const uint8_t *token, size_t token_length,
                             uint8_t *out, size_t out_capacity,
                             size_t *out_length) {
    uint8_t mac[32];
    uint8_t iv[16];
    size_t cipher_length;
    size_t index;
    uint8_t padding;
    uint8_t padding_error = 0U;
    int ok = 0;
    (void)context;
    if (out_length != NULL) *out_length = 0U;
    if (key == NULL || token == NULL || out == NULL || out_length == NULL ||
        token_length < 64U || (token_length - 48U) % RNS_AES_BLOCK_SIZE != 0U)
        return 0;
    cipher_length = token_length - 48U;
    if (out_capacity < cipher_length ||
        !backend_hmac(key, 32U, token, token_length - RNS_TOKEN_MAC_SIZE, mac) ||
        !esp_constant_time_equal(NULL, mac,
                                 token + token_length - RNS_TOKEN_MAC_SIZE,
                                 RNS_TOKEN_MAC_SIZE)) goto done;
    memcpy(iv, token, sizeof(iv));
    memcpy(out, token + RNS_AES_BLOCK_SIZE, cipher_length);
    if (!aes_cbc_decrypt(key + 32U, iv, out, cipher_length)) goto done;
    padding = out[cipher_length - 1U];
    if (padding == 0U || padding > RNS_AES_BLOCK_SIZE || padding > cipher_length)
        goto done;
    for (index = 0U; index < RNS_AES_BLOCK_SIZE; index++) {
        uint8_t check = out[cipher_length - 1U - index];
        uint8_t mask = (uint8_t)(index < padding ? 0xffU : 0U);
        padding_error |= (uint8_t)((check ^ padding) & mask);
    }
    if (padding_error != 0U) goto done;
    *out_length = cipher_length - padding;
    ok = 1;
done:
    sodium_memzero(mac, sizeof(mac));
    sodium_memzero(iv, sizeof(iv));
    if (!ok && out_capacity != 0U)
        sodium_memzero(out, out_capacity < cipher_length ? out_capacity : cipher_length);
    return ok;
}

static const rns_crypto_provider_t provider = {
    NULL,
    esp_sha256,
    esp_hmac,
    esp_hkdf,
    esp_random,
    esp_x_generate,
    esp_x_public,
    esp_x_exchange,
    esp_ed_generate,
    esp_ed_public,
    esp_ed_sign,
    esp_ed_verify,
    esp_constant_time_equal,
    esp_token_encrypt,
    esp_token_decrypt
};

rns_status_t rns_esp_crypto_install(void) {
    if (sodium_init() < 0) return RNS_ERROR_CRYPTO;
    return rns_crypto_provider_install(&provider);
}
