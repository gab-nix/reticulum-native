#include "reticulum/ifac.h"

#include "reticulum/crypto.h"

#include <string.h>

static const uint8_t IFAC_SALT[32] = {
    0xad,0xf5,0x4d,0x88,0x2c,0x9a,0x9b,0x80,0x77,0x1e,0xb4,0x99,0x5d,0x70,0x2d,0x4a,
    0x3e,0x73,0x33,0x91,0xb2,0xa0,0xf5,0x3f,0x41,0x6d,0x9f,0x90,0x7e,0x55,0xcf,0xf8
};

static int valid(const rns_ifac *ifac) {
    return ifac && ifac->tag_size >= RNS_IFAC_MIN_SIZE &&
           ifac->tag_size <= RNS_IFAC_MAX_SIZE && ifac->identity.has_private;
}

static int equal_ct(const uint8_t *a, const uint8_t *b, size_t n) {
    uint8_t different = 0; size_t i;
    for (i = 0; i < n; ++i) different |= (uint8_t)(a[i] ^ b[i]);
    return different == 0;
}

int rns_ifac_derive(rns_ifac *ifac, const uint8_t *network_name,
                    size_t network_name_length, const uint8_t *passphrase,
                    size_t passphrase_length, size_t tag_size) {
    uint8_t origin[64], origin_hash[32]; size_t n = 0;
    if (!ifac || tag_size < RNS_IFAC_MIN_SIZE || tag_size > RNS_IFAC_MAX_SIZE ||
        (!network_name_length && !passphrase_length) ||
        (network_name_length && !network_name) || (passphrase_length && !passphrase)) return 0;
    if (network_name_length) { if (!rns_sha256(network_name, network_name_length, origin + n)) return 0; n += 32; }
    if (passphrase_length) { if (!rns_sha256(passphrase, passphrase_length, origin + n)) return 0; n += 32; }
    if (!rns_sha256(origin, n, origin_hash)) return 0;
    memset(ifac, 0, sizeof(*ifac));
    if (!rns_hkdf_sha256(origin_hash, sizeof(origin_hash), IFAC_SALT, sizeof(IFAC_SALT),
                         NULL, 0, ifac->key, sizeof(ifac->key)) ||
        !rns_identity_from_private(&ifac->identity, ifac->key)) {
        memset(ifac, 0, sizeof(*ifac)); return 0;
    }
    ifac->tag_size = tag_size; return 1;
}

size_t rns_ifac_protected_bound(size_t raw_length, size_t tag_size) {
    if (raw_length < 2 || raw_length > RNS_MTU || tag_size < RNS_IFAC_MIN_SIZE ||
        tag_size > RNS_IFAC_MAX_SIZE || raw_length > SIZE_MAX - tag_size) return 0;
    return raw_length + tag_size;
}

int rns_ifac_protect(const rns_ifac *ifac, const uint8_t *raw,
                     size_t raw_length, uint8_t *out, size_t out_capacity,
                     size_t *out_length) {
    uint8_t signature[64], tag[64], mask[RNS_MTU + RNS_IFAC_MAX_SIZE];
    size_t total, i;
    if (!valid(ifac) || !raw || !out || !out_length ||
        !(total = rns_ifac_protected_bound(raw_length, ifac->tag_size)) || out_capacity < total ||
        !rns_identity_sign(&ifac->identity, raw, raw_length, signature)) return 0;
    memcpy(tag, signature + sizeof(signature) - ifac->tag_size, ifac->tag_size);
    if (!rns_hkdf_sha256(tag, ifac->tag_size, ifac->key, sizeof(ifac->key), NULL, 0, mask, total)) return 0;
    out[0] = (uint8_t)((raw[0] | 0x80u) ^ mask[0]); out[0] |= 0x80u;
    out[1] = (uint8_t)(raw[1] ^ mask[1]); memcpy(out + 2, tag, ifac->tag_size);
    for (i = 2; i < raw_length; ++i) out[i + ifac->tag_size] = (uint8_t)(raw[i] ^ mask[i + ifac->tag_size]);
    *out_length = total; return 1;
}

int rns_ifac_unprotect(const rns_ifac *ifac, const uint8_t *protected_raw,
                       size_t protected_length, uint8_t *out,
                       size_t out_capacity, size_t *out_length) {
    uint8_t expected[64], mask[RNS_MTU + RNS_IFAC_MAX_SIZE];
    const uint8_t *tag; size_t raw_length, i;
    if (!valid(ifac) || !protected_raw || !out || !out_length || !(protected_raw[0] & 0x80u) ||
        protected_length <= 2 + ifac->tag_size) return 0;
    raw_length = protected_length - ifac->tag_size;
    if (raw_length > RNS_MTU || out_capacity < raw_length) return 0;
    tag = protected_raw + 2;
    if (!rns_hkdf_sha256(tag, ifac->tag_size, ifac->key, sizeof(ifac->key), NULL, 0,
                         mask, protected_length)) return 0;
    out[0] = (uint8_t)((protected_raw[0] ^ mask[0]) & 0x7fu);
    out[1] = (uint8_t)(protected_raw[1] ^ mask[1]);
    for (i = 2; i < raw_length; ++i) out[i] = (uint8_t)(protected_raw[i + ifac->tag_size] ^ mask[i + ifac->tag_size]);
    if (!rns_identity_sign(&ifac->identity, out, raw_length, expected) ||
        !equal_ct(tag, expected + sizeof(expected) - ifac->tag_size, ifac->tag_size)) {
        memset(out, 0, raw_length); return 0;
    }
    *out_length = raw_length; return 1;
}
