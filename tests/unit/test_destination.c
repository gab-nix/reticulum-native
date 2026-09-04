#include "reticulum/destination.h"
#include <assert.h>
#include <string.h>

int main(void) {
    const char *aspects[] = {"delivery"}; rns_identity identity = {0}; uint8_t name_hash[10], hash[16];
    const uint8_t expected_name[10] = {0x6e,0xc6,0x0b,0xc3,0x18,0xe2,0xc0,0xf0,0xd9,0x08};
    const uint8_t expected_hash[16] = {0xa9,0x53,0xc2,0x2c,0xba,0x1c,0xff,0x60,0xcb,0xaf,0x4d,0x3c,0x75,0xec,0xf3,0x5a};
    for (unsigned i = 0; i < 16; ++i) identity.hash[i] = (uint8_t)i;
    assert(rns_destination_name_hash("lxmf", aspects, 1, name_hash)); assert(memcmp(name_hash, expected_name, 10) == 0);
    assert(rns_destination_hash(&identity, "lxmf", aspects, 1, hash)); assert(memcmp(hash, expected_hash, 16) == 0);
    assert(!rns_destination_name_hash("bad.name", NULL, 0, name_hash));
    return 0;
}
