#include "reticulum/identity.h"
#include "reticulum/crypto.h"

#include <string.h>

static int update_hash(rns_identity *identity) {
    uint8_t pub[64], digest[32];
    memcpy(pub, identity->encryption_public, 32); memcpy(pub + 32, identity->signing_public, 32);
    if (!rns_sha256(pub, sizeof(pub), digest)) return 0;
    memcpy(identity->hash, digest, 16); return 1;
}

int rns_identity_generate(rns_identity *identity) {
    if (!identity) return 0; memset(identity, 0, sizeof(*identity));
    if (!rns_x25519_generate(identity->encryption_private, identity->encryption_public) ||
        !rns_ed25519_generate(identity->signing_private, identity->signing_public)) {
        memset(identity, 0, sizeof(*identity)); return 0;
    }
    identity->has_private = 1; return update_hash(identity);
}

int rns_identity_from_private(rns_identity *identity, const uint8_t private_key[64]) {
    if (!identity || !private_key) return 0; memset(identity, 0, sizeof(*identity));
    memcpy(identity->encryption_private, private_key, 32); memcpy(identity->signing_private, private_key + 32, 32);
    if (!rns_x25519_public_from_private(identity->encryption_private, identity->encryption_public) ||
        !rns_ed25519_public_from_private(identity->signing_private, identity->signing_public)) {
        memset(identity, 0, sizeof(*identity)); return 0;
    }
    identity->has_private = 1; return update_hash(identity);
}

int rns_identity_from_public(rns_identity *identity, const uint8_t public_key[64]) {
    if (!identity || !public_key) return 0; memset(identity, 0, sizeof(*identity));
    memcpy(identity->encryption_public, public_key, 32); memcpy(identity->signing_public, public_key + 32, 32);
    return update_hash(identity);
}

int rns_identity_export_private(const rns_identity *identity, uint8_t out[64]) {
    if (!identity || !out || !identity->has_private) return 0;
    memcpy(out, identity->encryption_private, 32); memcpy(out + 32, identity->signing_private, 32); return 1;
}

void rns_identity_export_public(const rns_identity *identity, uint8_t out[64]) {
    if (!identity || !out) return;
    memcpy(out, identity->encryption_public, 32); memcpy(out + 32, identity->signing_public, 32);
}

int rns_identity_sign(const rns_identity *identity, const uint8_t *message, size_t message_length, uint8_t signature[64]) {
    return identity && identity->has_private && rns_ed25519_sign(identity->signing_private, message, message_length, signature);
}

int rns_identity_verify(const rns_identity *identity, const uint8_t *message, size_t message_length, const uint8_t signature[64]) {
    return identity && rns_ed25519_verify(identity->signing_public, message, message_length, signature);
}
