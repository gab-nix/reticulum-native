#include "reticulum/announce.h"
#include "reticulum/destination.h"

#include <assert.h>
#include <string.h>

int main(void) {
    uint8_t private_key[64], destination_hash[16], name_hash[10], random_prefix[5] = {1,2,3,4,5};
    uint8_t ratchet[32], body[500], context_flag, changed_hash[16]; size_t body_length;
    const uint8_t app_data[] = {0x81, 0xa4, 'n','a','m','e', 0xa3, 'r','e','i'};
    const char *aspects[] = {"delivery"}; rns_identity identity; rns_announce parsed;
    for (size_t i = 0; i < sizeof(private_key); ++i) private_key[i] = (uint8_t)(i + 1);
    for (size_t i = 0; i < sizeof(ratchet); ++i) ratchet[i] = (uint8_t)(0xa0 + i);
    assert(rns_identity_from_private(&identity, private_key));
    assert(rns_destination_name_hash("lxmf", aspects, 1, name_hash));
    assert(rns_destination_hash(&identity, "lxmf", aspects, 1, destination_hash));

    assert(rns_announce_build(&identity, destination_hash, name_hash, random_prefix,
                              UINT64_C(0x0102030405), NULL, app_data, sizeof(app_data),
                              body, sizeof(body), &body_length, &context_flag));
    assert(context_flag == 0 && body_length == RNS_ANNOUNCE_MIN_BODY_SIZE + sizeof(app_data));
    assert(rns_announce_parse(&parsed, body, body_length, context_flag));
    assert(!parsed.has_ratchet && parsed.timestamp == UINT64_C(0x0102030405));
    assert(parsed.app_data_length == sizeof(app_data) && memcmp(parsed.app_data, app_data, sizeof(app_data)) == 0);
    assert(rns_announce_verify(destination_hash, body, body_length, context_flag));
    memcpy(changed_hash, destination_hash, 16); changed_hash[0] ^= 1;
    assert(!rns_announce_verify(changed_hash, body, body_length, context_flag));
    body[body_length - 1] ^= 1; assert(!rns_announce_verify(destination_hash, body, body_length, context_flag));
    body[body_length - 1] ^= 1;

    assert(rns_announce_build(&identity, destination_hash, name_hash, random_prefix, 42, ratchet,
                              NULL, 0, body, sizeof(body), &body_length, &context_flag));
    assert(context_flag == 1 && body_length == RNS_ANNOUNCE_RATCHET_BODY_SIZE);
    assert(rns_announce_parse(&parsed, body, body_length, context_flag));
    assert(parsed.has_ratchet && parsed.timestamp == 42 && memcmp(parsed.ratchet, ratchet, 32) == 0);
    assert(rns_announce_verify(destination_hash, body, body_length, context_flag));
    assert(!rns_announce_parse(&parsed, body, RNS_ANNOUNCE_RATCHET_BODY_SIZE - 1, 1));
    assert(!rns_announce_parse(&parsed, body, RNS_ANNOUNCE_MIN_BODY_SIZE - 1, 0));
    assert(!rns_announce_parse(&parsed, body, RNS_ANNOUNCE_MAX_BODY_SIZE + 1, 0));
    assert(!rns_announce_build(&identity, destination_hash, name_hash, random_prefix,
                               RNS_ANNOUNCE_MAX_TIMESTAMP + 1, NULL, NULL, 0,
                               body, sizeof(body), &body_length, &context_flag));
    return 0;
}
