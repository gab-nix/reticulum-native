#include "reticulum/crypto.h"

#include <limits.h>
#include <string.h>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>
#include <openssl/sha.h>

static int valid_buffer(const uint8_t *p, size_t n) { return n == 0 || p != NULL; }

int rns_sha256(const uint8_t *data, size_t length, uint8_t out[32]) {
    if (!out || !valid_buffer(data, length)) return 0;
    return SHA256(data, length, out) != NULL;
}

int rns_hmac_sha256(const uint8_t *key, size_t key_length, const uint8_t *data,
                    size_t data_length, uint8_t out[32]) {
    unsigned int n = 0;
    if (!out || !valid_buffer(key, key_length) || !valid_buffer(data, data_length) || key_length > INT_MAX) return 0;
    return HMAC(EVP_sha256(), key, (int)key_length, data, data_length, out, &n) != NULL && n == 32;
}

int rns_hkdf_sha256(const uint8_t *input, size_t input_length, const uint8_t *salt,
                    size_t salt_length, const uint8_t *context, size_t context_length,
                    uint8_t *out, size_t out_length) {
    uint8_t zero_salt[32] = {0}, prk[32], block[32], scratch[32 + 255 + 1];
    size_t produced = 0, previous = 0;
    unsigned counter = 1;
    if (!out || out_length == 0 || out_length > 255u * 32u || !valid_buffer(input, input_length) ||
        input_length == 0 || !valid_buffer(salt, salt_length) || !valid_buffer(context, context_length) ||
        context_length > 255) return 0;
    if (!salt || salt_length == 0) { salt = zero_salt; salt_length = sizeof(zero_salt); }
    if (!rns_hmac_sha256(salt, salt_length, input, input_length, prk)) return 0;
    while (produced < out_length) {
        size_t take, message_length = 0;
        if (previous) { memcpy(scratch, block, 32); message_length = 32; }
        if (context_length) { memcpy(scratch + message_length, context, context_length); message_length += context_length; }
        scratch[message_length++] = (uint8_t)counter++;
        if (!rns_hmac_sha256(prk, sizeof(prk), scratch, message_length, block)) goto fail;
        take = out_length - produced < sizeof(block) ? out_length - produced : sizeof(block);
        memcpy(out + produced, block, take); produced += take; previous = sizeof(block);
    }
    OPENSSL_cleanse(prk, sizeof(prk)); OPENSSL_cleanse(block, sizeof(block));
    return 1;
fail:
    OPENSSL_cleanse(prk, sizeof(prk)); OPENSSL_cleanse(block, sizeof(block));
    return 0;
}

int rns_random_bytes(uint8_t *out, size_t length) {
    return out && length <= INT_MAX && RAND_bytes(out, (int)length) == 1;
}

static int raw_public_from_private(int type, const uint8_t private_key[32], uint8_t public_key[32]) {
    EVP_PKEY *key = NULL; size_t n = 32; int ok = 0;
    if (!private_key || !public_key) return 0;
    key = EVP_PKEY_new_raw_private_key(type, NULL, private_key, 32);
    if (key) ok = EVP_PKEY_get_raw_public_key(key, public_key, &n) == 1 && n == 32;
    EVP_PKEY_free(key); return ok;
}

static int generate_raw(int type, uint8_t private_key[32], uint8_t public_key[32]) {
    EVP_PKEY_CTX *ctx = NULL; EVP_PKEY *key = NULL; size_t a = 32, b = 32; int ok = 0;
    if (!private_key || !public_key) return 0;
    ctx = EVP_PKEY_CTX_new_id(type, NULL);
    if (ctx && EVP_PKEY_keygen_init(ctx) == 1 && EVP_PKEY_keygen(ctx, &key) == 1 &&
        EVP_PKEY_get_raw_private_key(key, private_key, &a) == 1 && a == 32 &&
        EVP_PKEY_get_raw_public_key(key, public_key, &b) == 1 && b == 32) ok = 1;
    EVP_PKEY_free(key); EVP_PKEY_CTX_free(ctx); return ok;
}

int rns_x25519_generate(uint8_t private_key[32], uint8_t public_key[32]) { return generate_raw(EVP_PKEY_X25519, private_key, public_key); }
int rns_x25519_public_from_private(const uint8_t private_key[32], uint8_t public_key[32]) { return raw_public_from_private(EVP_PKEY_X25519, private_key, public_key); }
int rns_ed25519_generate(uint8_t private_key[32], uint8_t public_key[32]) { return generate_raw(EVP_PKEY_ED25519, private_key, public_key); }
int rns_ed25519_public_from_private(const uint8_t private_key[32], uint8_t public_key[32]) { return raw_public_from_private(EVP_PKEY_ED25519, private_key, public_key); }

int rns_x25519_exchange(const uint8_t private_key[32], const uint8_t peer_public[32], uint8_t shared[32]) {
    EVP_PKEY *ours = NULL, *peer = NULL; EVP_PKEY_CTX *ctx = NULL; size_t n = 32; int ok = 0;
    if (!private_key || !peer_public || !shared) return 0;
    ours = EVP_PKEY_new_raw_private_key(EVP_PKEY_X25519, NULL, private_key, 32);
    peer = EVP_PKEY_new_raw_public_key(EVP_PKEY_X25519, NULL, peer_public, 32);
    if (ours && peer && (ctx = EVP_PKEY_CTX_new(ours, NULL)) && EVP_PKEY_derive_init(ctx) == 1 &&
        EVP_PKEY_derive_set_peer(ctx, peer) == 1 && EVP_PKEY_derive(ctx, shared, &n) == 1 && n == 32) ok = 1;
    EVP_PKEY_CTX_free(ctx); EVP_PKEY_free(peer); EVP_PKEY_free(ours); return ok;
}

int rns_ed25519_sign(const uint8_t private_key[32], const uint8_t *message, size_t message_length, uint8_t signature[64]) {
    EVP_PKEY *key = NULL; EVP_MD_CTX *ctx = NULL; size_t n = 64; int ok = 0;
    if (!private_key || !signature || !valid_buffer(message, message_length)) return 0;
    key = EVP_PKEY_new_raw_private_key(EVP_PKEY_ED25519, NULL, private_key, 32); ctx = EVP_MD_CTX_new();
    if (key && ctx && EVP_DigestSignInit(ctx, NULL, NULL, NULL, key) == 1 &&
        EVP_DigestSign(ctx, signature, &n, message, message_length) == 1 && n == 64) ok = 1;
    EVP_MD_CTX_free(ctx); EVP_PKEY_free(key); return ok;
}

int rns_ed25519_verify(const uint8_t public_key[32], const uint8_t *message, size_t message_length, const uint8_t signature[64]) {
    EVP_PKEY *key = NULL; EVP_MD_CTX *ctx = NULL; int ok = 0;
    if (!public_key || !signature || !valid_buffer(message, message_length)) return 0;
    key = EVP_PKEY_new_raw_public_key(EVP_PKEY_ED25519, NULL, public_key, 32); ctx = EVP_MD_CTX_new();
    if (key && ctx && EVP_DigestVerifyInit(ctx, NULL, NULL, NULL, key) == 1)
        ok = EVP_DigestVerify(ctx, signature, 64, message, message_length) == 1;
    EVP_MD_CTX_free(ctx); EVP_PKEY_free(key); return ok;
}

int rns_token_encrypt(const uint8_t key[64], const uint8_t *plaintext, size_t plaintext_length,
                      uint8_t *out, size_t out_capacity, size_t *out_length) {
    EVP_CIPHER_CTX *ctx = NULL; uint8_t mac[32]; int n = 0, final_n = 0; size_t needed; int ok = 0;
    if (!key || !out || !out_length || !valid_buffer(plaintext, plaintext_length) || plaintext_length > INT_MAX) return 0;
    needed = 16 + ((plaintext_length / 16) + 1) * 16 + 32;
    if (out_capacity < needed || needed > INT_MAX) return 0;
    if (!rns_random_bytes(out, 16)) return 0;
    ctx = EVP_CIPHER_CTX_new();
    if (ctx && EVP_EncryptInit_ex(ctx, EVP_aes_256_cbc(), NULL, key + 32, out) == 1 &&
        EVP_EncryptUpdate(ctx, out + 16, &n, plaintext, (int)plaintext_length) == 1 &&
        EVP_EncryptFinal_ex(ctx, out + 16 + n, &final_n) == 1 &&
        rns_hmac_sha256(key, 32, out, 16u + (size_t)n + (size_t)final_n, mac)) {
        memcpy(out + 16 + n + final_n, mac, 32); *out_length = 48u + (size_t)n + (size_t)final_n; ok = 1;
    }
    OPENSSL_cleanse(mac, sizeof(mac)); EVP_CIPHER_CTX_free(ctx); return ok;
}

int rns_token_decrypt(const uint8_t key[64], const uint8_t *token, size_t token_length,
                      uint8_t *out, size_t out_capacity, size_t *out_length) {
    EVP_CIPHER_CTX *ctx = NULL; uint8_t mac[32]; size_t cipher_length; int n = 0, final_n = 0, ok = 0;
    if (!key || !token || !out || !out_length || token_length < 64 || (token_length - 48) % 16 != 0) return 0;
    cipher_length = token_length - 48;
    if (cipher_length > INT_MAX || out_capacity < cipher_length) return 0;
    if (!rns_hmac_sha256(key, 32, token, token_length - 32, mac) || CRYPTO_memcmp(mac, token + token_length - 32, 32) != 0) goto done;
    ctx = EVP_CIPHER_CTX_new();
    if (ctx && EVP_DecryptInit_ex(ctx, EVP_aes_256_cbc(), NULL, key + 32, token) == 1 &&
        EVP_DecryptUpdate(ctx, out, &n, token + 16, (int)cipher_length) == 1 &&
        EVP_DecryptFinal_ex(ctx, out + n, &final_n) == 1) { *out_length = (size_t)n + (size_t)final_n; ok = 1; }
done:
    OPENSSL_cleanse(mac, sizeof(mac)); EVP_CIPHER_CTX_free(ctx); return ok;
}
