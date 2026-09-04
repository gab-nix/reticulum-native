#include "reticulum/crypto.h"
#include "reticulum/esp_idf.h"
#include "reticulum/hal.h"

#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct fake_entropy {
    uint8_t next;
    int fail_after_write;
} fake_entropy_t;

static rns_status_t fake_time(void *context, uint64_t *milliseconds) {
    (void)context;
    if (milliseconds == NULL) return RNS_ERROR_INVALID_ARGUMENT;
    *milliseconds = 1U;
    return RNS_OK;
}

static rns_status_t fake_random(void *context, void *output, size_t length) {
    fake_entropy_t *entropy = context;
    uint8_t *bytes = output;
    size_t index;
    if (output == NULL && length != 0U) return RNS_ERROR_INVALID_ARGUMENT;
    for (index = 0U; index < length; index++) bytes[index] = entropy->next++;
    return entropy->fail_after_write != 0 ? RNS_ERROR_IO : RNS_OK;
}

static void fake_zero(void *context, void *memory, size_t length) {
    volatile uint8_t *bytes = memory;
    (void)context;
    while (bytes != NULL && length != 0U) {
        *bytes++ = 0U;
        length--;
    }
}

static void *fake_allocate(void *context, size_t size) {
    (void)context;
    return malloc(size);
}

static void fake_deallocate(void *context, void *memory) {
    (void)context;
    free(memory);
}

static int all_zero(const uint8_t *bytes, size_t length) {
    size_t index;
    uint8_t value = 0U;
    for (index = 0U; index < length; index++) value |= bytes[index];
    return value == 0U;
}

static uint8_t hex_nibble(char value) {
    if (value >= '0' && value <= '9') return (uint8_t)(value - '0');
    if (value >= 'a' && value <= 'f') return (uint8_t)(value - 'a' + 10);
    assert(0 && "invalid test vector");
    return 0U;
}

static void decode_hex(const char *hex, uint8_t *output, size_t output_length) {
    size_t index;
    assert(strlen(hex) == output_length * 2U);
    for (index = 0U; index < output_length; index++) {
        output[index] = (uint8_t)((hex_nibble(hex[index * 2U]) << 4U) |
                                  hex_nibble(hex[index * 2U + 1U]));
    }
}

int main(void) {
    fake_entropy_t entropy = {0U, 0};
    const rns_platform_ops_t platform = {
        .context = &entropy,
        .monotonic_ms = fake_time,
        .wallclock_ms = fake_time,
        .random_bytes = fake_random,
        .secure_zero = fake_zero,
        .allocate = fake_allocate,
        .deallocate = fake_deallocate
    };
    uint8_t digest[32];
    uint8_t expected[64];
    uint8_t key_material[22];
    uint8_t output[128];
    uint8_t long_info[256] = {0};
    uint8_t private_key[32];
    uint8_t public_key[32];
    uint8_t peer_public[32];
    uint8_t shared[32];
    uint8_t signature[64];
    uint8_t token[128];
    uint8_t plaintext[64];
    uint8_t token_key[64];
    size_t token_length = 0U;
    size_t plaintext_length = 0U;

    assert(rns_platform_install(&platform) == RNS_OK);
    assert(rns_esp_crypto_install() == RNS_OK);

    decode_hex("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
               expected, 32U);
    assert(rns_sha256((const uint8_t *)"abc", 3U, digest));
    assert(memcmp(digest, expected, 32U) == 0);
    assert(rns_constant_time_equal(digest, expected, 32U));
    expected[31] ^= 1U;
    assert(!rns_constant_time_equal(digest, expected, 32U));
    expected[31] ^= 1U;

    decode_hex("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
               expected, 32U);
    assert(rns_sha256(NULL, 0U, digest));
    assert(memcmp(digest, expected, 32U) == 0);
    assert(!rns_sha256(NULL, 1U, digest));

    memset(key_material, 0x0b, sizeof(key_material));
    decode_hex("b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7",
               expected, 32U);
    assert(rns_hmac_sha256(key_material, 20U,
                           (const uint8_t *)"Hi There", 8U, digest));
    assert(memcmp(digest, expected, 32U) == 0);

    decode_hex("b613679a0814d9ec772f95d778c35fc5ff1697c493715653c6c712144292c5ad",
               expected, 32U);
    assert(rns_hmac_sha256(NULL, 0U, NULL, 0U, digest));
    assert(memcmp(digest, expected, 32U) == 0);
    assert(!rns_hmac_sha256(NULL, 1U, NULL, 0U, digest));

    decode_hex("000102030405060708090a0b0c", key_material, 13U);
    decode_hex("f0f1f2f3f4f5f6f7f8f9", output, 10U);
    memset(private_key, 0x0b, 22U);
    assert(rns_hkdf_sha256(private_key, 22U, key_material, 13U, output, 10U,
                           digest, 32U));
    decode_hex("3cb25f25faacd57a90434f64d0362f2a2d2d0a90cf1a5a4c5db02d56ecc4c5bf",
               expected, 32U);
    assert(memcmp(digest, expected, 32U) == 0);

    private_key[0] = 0x0bU;
    assert(rns_hkdf_sha256(private_key, 1U, NULL, 0U, NULL, 0U,
                           digest, 32U));
    decode_hex("d73d5cae33b38a008b04bcabbe0a83239ec9a100b09eee2370c3dc874304b4c1",
               expected, 32U);
    assert(memcmp(digest, expected, 32U) == 0);
    assert(!rns_hkdf_sha256(NULL, 0U, NULL, 0U, NULL, 0U, digest, 32U));
    assert(!rns_hkdf_sha256(private_key, 1U, NULL, 0U, NULL, 0U, digest, 0U));
    assert(!rns_hkdf_sha256(private_key, 1U, NULL, 0U, long_info,
                            sizeof(long_info), digest, 32U));
    assert(!rns_hkdf_sha256(private_key, 1U, NULL, 0U, NULL, 0U,
                            digest, 255U * 32U + 1U));

    decode_hex("9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60",
               private_key, 32U);
    decode_hex("d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a",
               expected, 32U);
    assert(rns_ed25519_public_from_private(private_key, public_key));
    assert(memcmp(public_key, expected, 32U) == 0);
    assert(rns_ed25519_sign(private_key, NULL, 0U, signature));
    decode_hex("e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e06522490155"
               "5fb8821590a33bacc61e39701cf9b46bd25bf5f0595bbe24655141438e7a100b",
               expected, 64U);
    assert(memcmp(signature, expected, 64U) == 0);
    assert(rns_ed25519_verify(public_key, NULL, 0U, signature));

    decode_hex("77076d0a7318a57d3c16c17251b26645df4c2f87ebc0992ab177fba51db92c2a",
               private_key, 32U);
    decode_hex("8520f0098930a754748b7ddcb43ef75a0dbf3a0d26381af4eba4a98eaa9b4e6a",
               expected, 32U);
    assert(rns_x25519_public_from_private(private_key, public_key));
    assert(memcmp(public_key, expected, 32U) == 0);
    decode_hex("de9edb7d7b7dc1b4d35b61c2ece435373f8343c85b78674dadfc7e146f882b4f",
               peer_public, 32U);
    decode_hex("4a5d9d5ba4ce2de1728e3bf480350f25e07e21c947d19e3376f09b3c1e161742",
               expected, 32U);
    assert(rns_x25519_exchange(private_key, peer_public, shared));
    assert(memcmp(shared, expected, 32U) == 0);

    memset(peer_public, 0, sizeof(peer_public));
    memset(shared, 0xa5, sizeof(shared));
    assert(!rns_x25519_exchange(private_key, peer_public, shared));
    assert(all_zero(shared, sizeof(shared)));

    memset(token_key, 0x5a, sizeof(token_key));
    entropy.next = 0U;
    assert(rns_token_encrypt(token_key, (const uint8_t *)"cross-backend", 13U,
                             token, sizeof(token), &token_length));
    decode_hex("000102030405060708090a0b0c0d0e0f"
               "17dc521d7a8675379b43bb7c721606a7"
               "ffe67e9f643fe2aa9497dab67ac70d36589aef9c240f7fe2657be05c0ad7a3e1",
               expected, 64U);
    assert(token_length == 64U && memcmp(token, expected, token_length) == 0);
    rns_crypto_provider_restore_default();
    assert(rns_token_decrypt(token_key, token, token_length, plaintext,
                             sizeof(plaintext), &plaintext_length));
    assert(plaintext_length == 13U &&
           memcmp(plaintext, "cross-backend", plaintext_length) == 0);

    assert(rns_token_encrypt(token_key, (const uint8_t *)"openssl", 7U,
                             token, sizeof(token), &token_length));
    assert(rns_esp_crypto_install() == RNS_OK);
    assert(rns_token_decrypt(token_key, token, token_length, plaintext,
                             sizeof(plaintext), &plaintext_length));
    assert(plaintext_length == 7U && memcmp(plaintext, "openssl", 7U) == 0);
    plaintext_length = 99U;
    assert(!rns_token_decrypt(token_key, token, token_length, plaintext, 15U,
                              &plaintext_length));
    assert(plaintext_length == 0U);
    plaintext_length = 99U;
    assert(!rns_token_decrypt(token_key, token, 63U, plaintext,
                              sizeof(plaintext), &plaintext_length));
    assert(plaintext_length == 0U);
    token[token_length - 1U] ^= 1U;
    assert(!rns_token_decrypt(token_key, token, token_length, plaintext,
                              sizeof(plaintext), &plaintext_length));
    assert(plaintext_length == 0U);

    entropy.fail_after_write = 1;
    memset(private_key, 0xa5, sizeof(private_key));
    memset(public_key, 0xa5, sizeof(public_key));
    assert(!rns_x25519_generate(private_key, public_key));
    assert(all_zero(private_key, sizeof(private_key)) &&
           all_zero(public_key, sizeof(public_key)));
    memset(private_key, 0xa5, sizeof(private_key));
    memset(public_key, 0xa5, sizeof(public_key));
    assert(!rns_ed25519_generate(private_key, public_key));
    assert(all_zero(private_key, sizeof(private_key)) &&
           all_zero(public_key, sizeof(public_key)));

    memset(token, 0xa5, sizeof(token));
    token_length = 99U;
    assert(!rns_token_encrypt(token_key, (const uint8_t *)"failure", 7U,
                              token, sizeof(token), &token_length));
    assert(token_length == 0U && all_zero(token, 64U));

    entropy.fail_after_write = 0;
    memset(token, 0xa5, sizeof(token));
    token_length = 99U;
    assert(!rns_token_encrypt(token_key, (const uint8_t *)"bounds", 6U,
                              token, 63U, &token_length));
    assert(token_length == 0U && token[0] == 0xa5U);

    rns_crypto_provider_restore_default();
    rns_platform_restore_default();
    return 0;
}
