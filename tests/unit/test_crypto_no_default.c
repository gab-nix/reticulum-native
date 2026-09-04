#include "reticulum/crypto.h"

#include <assert.h>
#include <stdint.h>

int main(void) {
    uint8_t digest[32];

    assert(rns_crypto_provider_current() == NULL);
    assert(!rns_sha256((const uint8_t *)"abc", 3U, digest));
    assert(!rns_constant_time_equal(digest, digest, sizeof(digest)));
    assert(rns_crypto_provider_install(NULL) == RNS_ERROR_INVALID_ARGUMENT);
    return 0;
}
