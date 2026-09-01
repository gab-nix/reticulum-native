#include "reticulum/identity.h"
#include <assert.h>
#include <string.h>

int main(void) {
    rns_identity a, b, public_only; uint8_t private_key[64], public_key[64], signature[64];
    assert(rns_identity_generate(&a)); assert(rns_identity_export_private(&a, private_key)); rns_identity_export_public(&a, public_key);
    assert(rns_identity_from_private(&b, private_key)); assert(memcmp(a.hash, b.hash, 16) == 0);
    assert(rns_identity_from_public(&public_only, public_key)); assert(memcmp(a.hash, public_only.hash, 16) == 0);
    assert(!rns_identity_export_private(&public_only, private_key));
    assert(rns_identity_sign(&a, (const uint8_t *)"message", 7, signature));
    assert(rns_identity_verify(&public_only, (const uint8_t *)"message", 7, signature));
    return 0;
}
