#include "reticulum/crypto.h"
#include "reticulum/identity.h"
#include "reticulum/hal.h"

#include <assert.h>
#include <stdint.h>
#include <string.h>

int main(void) {
    uint8_t digest[32];

    assert(rns_crypto_provider_current() == NULL);
    assert(!rns_sha256((const uint8_t *)"abc", 3U, digest));
    assert(!rns_constant_time_equal(digest, digest, sizeof(digest)));
    assert(rns_crypto_provider_install(NULL) == RNS_ERROR_INVALID_ARGUMENT);
    /* Compile and link the identity implementation with neither OpenSSL nor
       POSIX defaults. Pre-provider boot must fail closed and still erase. */
    rns_identity identity;
    uint8_t ciphertext[128];
    size_t length = 0U;
    memset(&identity, 0xa5, sizeof identity);
    assert(!rns_identity_generate(&identity));
    assert(!identity.has_private);
    assert(!rns_identity_encrypt(&identity, NULL, (const uint8_t *)"x", 1U,
                                 ciphertext, sizeof ciphertext, &length));
    memset(digest, 0xa5, sizeof digest);
    rns_hal_secure_zero(digest, sizeof digest);
    for (size_t i = 0U; i < sizeof digest; ++i) assert(digest[i] == 0U);
    return 0;
}
