#include "reticulum/path_store.h"

#include <assert.h>
#include <string.h>

static double test_now;

static double clock_now(void *context) {
    (void)context;
    return test_now;
}

static void fill(uint8_t *value, size_t length, uint8_t seed) {
    for (size_t i = 0u; i < length; ++i) value[i] = (uint8_t)(seed + i);
}

static void repair_checksum(uint8_t *data, size_t length) {
    uint32_t crc = UINT32_MAX;
    for (size_t i = 32u; i < length; ++i) {
        crc ^= data[i];
        for (unsigned bit = 0u; bit < 8u; ++bit)
            crc = (crc >> 1u) ^ (0xedb88320u & (uint32_t)-(int32_t)(crc & 1u));
    }
    crc = ~crc;
    for (size_t i = 0u; i < 4u; ++i)
        data[28u + i] = (uint8_t)(crc >> (24u - 8u * i));
}

static void add_path(rns_transport *transport, uint8_t seed, uint64_t interface_id,
                     uint8_t hops) {
    uint8_t destination[16], next_hop[16], blob[10], packet_hash[32];
    fill(destination, sizeof destination, seed);
    fill(next_hop, sizeof next_hop, (uint8_t)(seed + 20u));
    fill(blob, sizeof blob, (uint8_t)(seed + 40u));
    blob[5] = 0u; blob[6] = 0u; blob[7] = 0u; blob[8] = 0u;
    blob[9] = seed;
    fill(packet_hash, sizeof packet_hash, (uint8_t)(seed + 60u));
    assert(rns_transport_consider_announce(transport, destination, next_hop,
                                           interface_id, 7, hops, blob,
                                           packet_hash) == RNS_PATH_INSERTED);
}

int main(void) {
    rns_transport_config config = {
        .path_capacity = 4u,
        .dedupe_capacity = 4u,
        .reverse_capacity = 4u,
        .random_blob_history = 3u,
        .path_lifetime = 30.0,
        .dedupe_lifetime = 5.0,
        .reverse_lifetime = 480.0,
        .clock = clock_now,
        .clock_context = NULL,
    };
    rns_transport source, restored;
    test_now = 100.0;
    assert(rns_transport_init(&source, &config));
    assert(rns_transport_init(&restored, &config));
    add_path(&source, 1u, 11u, 2u);
    test_now = 105.0;
    add_path(&source, 2u, 12u, 3u);
    uint8_t first[16], second[16], public_key[64];
    fill(first, sizeof first, 1u);
    fill(second, sizeof second, 2u);
    fill(public_key, sizeof public_key, 31u);
    assert(rns_transport_set_path_identity(&source, first, public_key));
    uint8_t encoded[1024];
    size_t encoded_length = 0u, count = 0u;
    assert(rns_path_store_encode(&source, 1000000u, NULL, 0u,
                                 &encoded_length, &count) == RNS_ERROR_OVERFLOW);
    assert(count == 2u && encoded_length > 32u);
    size_t required = encoded_length;
    assert(rns_path_store_encode(&source, 1000000u, encoded, required - 1u,
                                 &encoded_length, &count) == RNS_ERROR_OVERFLOW);
    assert(encoded_length == required);
    assert(rns_path_store_encode(&source, 1000000u, encoded, sizeof encoded,
                                 &encoded_length, &count) == RNS_OK);
    assert(count == 2u && encoded_length == required);

    test_now = 5.0;
    assert(rns_path_store_decode(&restored, 1010000u, encoded, encoded_length,
                                 &count) == RNS_OK);
    assert(count == 2u);
    const rns_path_entry *entry = rns_transport_lookup(&restored, first);
    assert(entry != NULL && entry->interface_id == 11u && entry->hops == 2u);
    assert(entry->has_identity && memcmp(entry->identity_public_key, public_key, 64u) == 0);
    assert(entry->expires_at == 20.0 && entry->updated_at == -10.0);
    entry = rns_transport_lookup(&restored, second);
    assert(entry != NULL && entry->expires_at == 25.0 && entry->updated_at == -5.0);
    assert(!entry->has_identity);
    assert(rns_transport_mark_unresponsive(&restored, second));

    uint8_t corrupted[1024];
    memcpy(corrupted, encoded, encoded_length);
    corrupted[encoded_length - 1u] ^= 0x80u;
    assert(rns_path_store_decode(&restored, 1010000u, corrupted, encoded_length,
                                 &count) == RNS_ERROR_PROTOCOL);
    assert(rns_transport_lookup(&restored, second) != NULL);

    /* Recompute the checksum so malformed identity metadata reaches the parser.
     * Rejected imports must leave the prior verified public key untouched. */
    for (uint8_t flag = 0u; flag <= 2u; flag += 2u) {
        memcpy(corrupted, encoded, encoded_length);
        corrupted[34u + 103u] = flag;
        repair_checksum(corrupted, encoded_length);
        assert(rns_path_store_decode(&restored, 1010000u, corrupted, encoded_length,
                                     &count) == RNS_ERROR_PROTOCOL);
        entry = rns_transport_lookup(&restored, first);
        assert(entry != NULL && entry->has_identity &&
               memcmp(entry->identity_public_key, public_key, 64u) == 0);
    }
    memcpy(corrupted, encoded, encoded_length);
    corrupted[9u] = 1u; /* Version 1 cannot carry the new identity flag. */
    assert(rns_path_store_decode(&restored, 1010000u, corrupted, encoded_length,
                                 &count) == RNS_ERROR_PROTOCOL);
    corrupted[9u] = 3u; /* Unknown future versions are not guessed. */
    assert(rns_path_store_decode(&restored, 1010000u, corrupted, encoded_length,
                                 &count) == RNS_ERROR_PROTOCOL);
    memcpy(corrupted, encoded, encoded_length);
    size_t first_size = ((size_t)encoded[32u] << 8u) | encoded[33u];
    corrupted[34u + first_size + 2u + 103u] = 1u; /* No key bytes follow. */
    repair_checksum(corrupted, encoded_length);
    assert(rns_path_store_decode(&restored, 1010000u, corrupted, encoded_length,
                                 &count) == RNS_ERROR_PROTOCOL);
    assert(rns_path_store_decode(&restored, 1010000u, encoded, encoded_length - 1u,
                                 &count) == RNS_ERROR_PROTOCOL);
    assert(rns_transport_lookup(&restored, second) != NULL);

    /* Time spent offline expires the older snapshot without partial import. */
    assert(rns_path_store_decode(&restored, 1045000u, encoded, encoded_length,
                                 &count) == RNS_OK);
    assert(count == 0u && rns_transport_lookup(&restored, first) == NULL);

    /* A backward wall-clock adjustment never lengthens the original TTL. */
    assert(rns_path_store_decode(&restored, 999000u, encoded, encoded_length,
                                 &count) == RNS_OK);
    assert(count == 2u);
    entry = rns_transport_lookup(&restored, first);
    assert(entry != NULL && entry->expires_at == 30.0);
    assert(entry->has_identity);

    /* Version 1 has the same route-only record bytes. It must remain readable
     * without inheriting identities left over in the destination table. */
    source.paths[0].has_identity = 0;
    test_now = 105.0;
    assert(rns_path_store_encode(&source, 1000000u, corrupted, sizeof corrupted,
                                 &required, &count) == RNS_OK);
    corrupted[9u] = 1u;
    test_now = 5.0;
    assert(rns_path_store_decode(&restored, 1010000u, corrupted, required, &count) == RNS_OK);
    entry = rns_transport_lookup(&restored, first);
    assert(entry != NULL && !entry->has_identity);
    uint8_t zero[64] = {0};
    assert(memcmp(entry->identity_public_key, zero, sizeof zero) == 0);

    assert(rns_path_store_decode(NULL, 0u, encoded, encoded_length, &count) ==
           RNS_ERROR_INVALID_ARGUMENT);
    assert(rns_path_store_encode(&source, 0u, encoded, sizeof encoded, NULL,
                                 &count) == RNS_ERROR_INVALID_ARGUMENT);
    rns_transport_free(&restored);
    rns_transport_free(&source);
    return 0;
}
