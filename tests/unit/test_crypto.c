#include "reticulum/crypto.h"
#include <assert.h>
#include <string.h>

static int replacement_sha256(void *context, const uint8_t *data, size_t length,
                              uint8_t out[32]) {
    unsigned *calls = context;
    (void)data; (void)length;
    (*calls)++;
    memset(out, 0x5a, 32U);
    return 1;
}

int main(void) {
    static const uint8_t expected[32] = {0xba,0x78,0x16,0xbf,0x8f,0x01,0xcf,0xea,0x41,0x41,0x40,0xde,0x5d,0xae,0x22,0x23,0xb0,0x03,0x61,0xa3,0x96,0x17,0x7a,0x9c,0xb4,0x10,0xff,0x61,0xf2,0x00,0x15,0xad};
    uint8_t digest[32], a_prv[32], a_pub[32], b_prv[32], b_pub[32], s1[32], s2[32];
    uint8_t e_prv[32], e_pub[32], sig[64], key[64] = {0}, token[128], plain[128]; size_t token_n, plain_n;
    rns_crypto_provider_t replacement = *rns_crypto_provider_current();
    rns_crypto_provider_t second_replacement;
    unsigned replacement_calls = 0U;
    assert(rns_sha256((const uint8_t *)"abc", 3, digest)); assert(memcmp(digest, expected, 32) == 0);
    assert(rns_x25519_generate(a_prv, a_pub) && rns_x25519_generate(b_prv, b_pub));
    assert(rns_x25519_exchange(a_prv, b_pub, s1) && rns_x25519_exchange(b_prv, a_pub, s2)); assert(memcmp(s1, s2, 32) == 0);
    assert(rns_ed25519_generate(e_prv, e_pub)); assert(rns_ed25519_sign(e_prv, (const uint8_t *)"hello", 5, sig));
    assert(rns_ed25519_verify(e_pub, (const uint8_t *)"hello", 5, sig)); sig[0] ^= 1; assert(!rns_ed25519_verify(e_pub, (const uint8_t *)"hello", 5, sig));
    assert(rns_constant_time_equal((const uint8_t *)"same", (const uint8_t *)"same", 4U));
    assert(!rns_constant_time_equal((const uint8_t *)"same", (const uint8_t *)"samo", 4U));
    assert(rns_constant_time_equal(NULL, NULL, 0U));
    assert(!rns_constant_time_equal(NULL, (const uint8_t *)"x", 1U));
    memset(key, 7, sizeof(key)); assert(rns_token_encrypt(key, (const uint8_t *)"token", 5, token, sizeof(token), &token_n));
    assert(rns_token_decrypt(key, token, token_n, plain, sizeof(plain), &plain_n)); assert(plain_n == 5 && memcmp(plain, "token", 5) == 0);
    token[token_n-1] ^= 1; assert(!rns_token_decrypt(key, token, token_n, plain, sizeof(plain), &plain_n));
    replacement.context = &replacement_calls;
    replacement.sha256 = replacement_sha256;
    second_replacement = replacement;
    assert(rns_crypto_provider_install(&replacement) == RNS_OK);
    assert(rns_crypto_provider_install(&replacement) == RNS_OK);
    assert(rns_crypto_provider_install(&second_replacement) ==
           RNS_ERROR_INVALID_STATE);
    assert(rns_sha256((const uint8_t *)"provider", 8U, digest));
    assert(replacement_calls == 1U && digest[0] == 0x5a && digest[31] == 0x5a);
    rns_crypto_provider_restore_default();
    assert(rns_sha256((const uint8_t *)"abc", 3U, digest));
    assert(memcmp(digest, expected, sizeof(expected)) == 0);
    return 0;
}
