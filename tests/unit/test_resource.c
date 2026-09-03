#include "reticulum/resource.h"
#include "reticulum/crypto.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

/*
 * A RESOURCE_ADV captured from a live Nomad Network node running the pinned
 * Reticulum 1.5.2 revision, answering a /page/index.mu request.
 */
static const char *const captured_advertisement =
    "8ba174cd0950a164cd52c8a16e06a168c42004147393c4b4f5a6933a689aa2a985f00b3556a25"
    "bfde407ea060586372395b4a172c40455073994a16fc42004147393c4b4f5a6933a689aa2a985"
    "f00b3556a25bfde407ea060586372395b4a16901a16c01a171c410b8634866a0a6e993f9af207"
    "aa640cee0a16613a16dc41818d01dafe9c8efe16a75f264d577f5b6aa928adc8478dd39";

static size_t unhex(const char *text, uint8_t *out) {
    size_t n = strlen(text) / 2u;
    for (size_t i = 0u; i < n; ++i) {
        unsigned v;
        sscanf(text + i * 2u, "%2x", &v);
        out[i] = (uint8_t)v;
    }
    return n;
}

static void test_captured_advertisement(void) {
    uint8_t raw[512];
    size_t length = unhex(captured_advertisement, raw);
    rns_resource_advertisement_t adv;
    assert(rns_resource_advertisement_parse(raw, length, &adv) == RNS_OK);
    assert(adv.transfer_size == 2384u);
    assert(adv.data_size == 21192u);
    assert(adv.parts == 6u);
    assert(adv.hashmap_length == 6u * RNS_RESOURCE_MAPHASH_LEN);
    assert(adv.segment_index == 1u && adv.total_segments == 1u);
    assert(adv.has_request_id);
    /* f = 19 = response | compressed | encrypted */
    assert(adv.flags == 19u);
    assert(adv.encrypted && adv.compressed && adv.is_response);
    assert(!adv.split && !adv.is_request && !adv.has_metadata);
    /* h and o match for a single-segment resource. */
    assert(memcmp(adv.hash, adv.original_hash, RNS_RESOURCE_HASH_SIZE) == 0);
    /* Truncation or trailing bytes must be rejected. */
    assert(rns_resource_advertisement_parse(raw, length - 1u, &adv) != RNS_OK);
    assert(rns_resource_advertisement_parse(raw, 1u, &adv) != RNS_OK);
}

/* Builds a msgpack advertisement for a plaintext resource. */
static size_t build_advertisement(uint8_t *out, size_t payload_length, size_t parts,
                                  const uint8_t random_hash[4],
                                  const uint8_t hash[32], const uint8_t *hashmap) {
    size_t o = 0u;
    out[o++] = 0x8bu; /* map of 11 */
    out[o++] = 0xa1u; out[o++] = 't'; out[o++] = 0xcdu;
    out[o++] = (uint8_t)(payload_length >> 8); out[o++] = (uint8_t)payload_length;
    out[o++] = 0xa1u; out[o++] = 'd'; out[o++] = 0xcdu;
    out[o++] = (uint8_t)(payload_length >> 8); out[o++] = (uint8_t)payload_length;
    out[o++] = 0xa1u; out[o++] = 'n'; out[o++] = (uint8_t)parts;
    out[o++] = 0xa1u; out[o++] = 'h'; out[o++] = 0xc4u; out[o++] = 32u;
    memcpy(out + o, hash, 32u); o += 32u;
    out[o++] = 0xa1u; out[o++] = 'r'; out[o++] = 0xc4u; out[o++] = 4u;
    memcpy(out + o, random_hash, 4u); o += 4u;
    out[o++] = 0xa1u; out[o++] = 'o'; out[o++] = 0xc4u; out[o++] = 32u;
    memcpy(out + o, hash, 32u); o += 32u;
    out[o++] = 0xa1u; out[o++] = 'i'; out[o++] = 1u;
    out[o++] = 0xa1u; out[o++] = 'l'; out[o++] = 1u;
    out[o++] = 0xa1u; out[o++] = 'q'; out[o++] = 0xc0u;
    out[o++] = 0xa1u; out[o++] = 'f'; out[o++] = 0u;
    out[o++] = 0xa1u; out[o++] = 'm'; out[o++] = 0xc4u;
    out[o++] = (uint8_t)(parts * RNS_RESOURCE_MAPHASH_LEN);
    memcpy(out + o, hashmap, parts * RNS_RESOURCE_MAPHASH_LEN);
    o += parts * RNS_RESOURCE_MAPHASH_LEN;
    return o;
}

static void test_plain_transfer(void) {
    /* stream = random hash || payload, split into parts. */
    const uint8_t random_hash[4] = {0xde, 0xad, 0xbe, 0xef};
    uint8_t payload[300];
    for (size_t i = 0u; i < sizeof payload; ++i) payload[i] = (uint8_t)(i * 7u + 3u);
    uint8_t stream[sizeof payload + 4u];
    memcpy(stream, random_hash, 4u);
    memcpy(stream + 4u, payload, sizeof payload);

    const size_t part_size = 64u;
    size_t parts = (sizeof stream + part_size - 1u) / part_size;
    uint8_t hashmap[16 * RNS_RESOURCE_MAPHASH_LEN];
    for (size_t i = 0u; i < parts; ++i) {
        uint8_t buffer[128];
        size_t len = i + 1u == parts ? sizeof stream - i * part_size : part_size;
        uint8_t digest[32];
        memcpy(buffer, stream + i * part_size, len);
        memcpy(buffer + len, random_hash, 4u);
        assert(rns_sha256(buffer, len + 4u, digest));
        memcpy(hashmap + i * RNS_RESOURCE_MAPHASH_LEN, digest, RNS_RESOURCE_MAPHASH_LEN);
    }
    uint8_t verify[sizeof payload + 4u], hash[32];
    memcpy(verify, payload, sizeof payload);
    memcpy(verify + sizeof payload, random_hash, 4u);
    assert(rns_sha256(verify, sizeof payload + 4u, hash));

    uint8_t raw[512];
    size_t length = build_advertisement(raw, sizeof payload, parts, random_hash,
                                        hash, hashmap);
    rns_resource_advertisement_t adv;
    assert(rns_resource_advertisement_parse(raw, length, &adv) == RNS_OK);
    assert(adv.parts == parts && !adv.encrypted && !adv.compressed);

    rns_resource_t *resource = NULL;
    assert(rns_resource_accept(&resource, &adv, 0u) == RNS_OK);
    assert(rns_resource_total_parts(resource) == parts);

    /* The first request asks for a bounded window, not the whole resource. */
    uint8_t request[512];
    size_t request_length = 0u;
    assert(rns_resource_build_request(resource, request, sizeof request,
                                      &request_length) == RNS_OK);
    assert(request[0] == 0x00u);
    assert(memcmp(request + 1, hash, 32u) == 0);
    size_t requested = (request_length - 33u) / RNS_RESOURCE_MAPHASH_LEN;
    assert(requested == RNS_RESOURCE_WINDOW || requested == parts);

    /* Deliver the parts out of order; matching is by map hash, not arrival. */
    for (size_t k = 0u; k < parts; ++k) {
        size_t i = (k * 3u) % parts;
        size_t len = i + 1u == parts ? sizeof stream - i * part_size : part_size;
        assert(rns_resource_receive_part(resource, stream + i * part_size, len) == RNS_OK);
    }
    assert(rns_resource_parts_complete(resource));
    assert(rns_resource_received_parts(resource) == parts);

    uint8_t assembled[1024];
    size_t assembled_length = 0u;
    assert(rns_resource_assemble(resource, NULL, assembled, sizeof assembled,
                                 &assembled_length) == RNS_OK);
    assert(assembled_length == sizeof payload);
    assert(memcmp(assembled, payload, sizeof payload) == 0);

    /* proof = resource hash || sha256(data || resource hash) */
    uint8_t proof[RNS_RESOURCE_PROOF_SIZE];
    assert(rns_resource_build_proof(resource, proof) == RNS_OK);
    assert(memcmp(proof, hash, 32u) == 0);
    uint8_t expect_input[sizeof payload + 32u], expect[32];
    memcpy(expect_input, payload, sizeof payload);
    memcpy(expect_input + sizeof payload, hash, 32u);
    assert(rns_sha256(expect_input, sizeof payload + 32u, expect));
    assert(memcmp(proof + 32u, expect, 32u) == 0);
    rns_resource_destroy(resource);
}

static void test_corrupt_and_bounds(void) {
    const uint8_t random_hash[4] = {1, 2, 3, 4};
    uint8_t payload[64];
    memset(payload, 0xa5, sizeof payload);
    uint8_t stream[sizeof payload + 4u];
    memcpy(stream, random_hash, 4u);
    memcpy(stream + 4u, payload, sizeof payload);
    uint8_t hashmap[RNS_RESOURCE_MAPHASH_LEN];
    uint8_t buffer[128], digest[32];
    memcpy(buffer, stream, sizeof stream);
    memcpy(buffer + sizeof stream, random_hash, 4u);
    assert(rns_sha256(buffer, sizeof stream + 4u, digest));
    memcpy(hashmap, digest, RNS_RESOURCE_MAPHASH_LEN);
    /* A hash that does not match the payload must fail verification. */
    uint8_t wrong_hash[32];
    memset(wrong_hash, 0x11, sizeof wrong_hash);

    uint8_t raw[256];
    size_t length = build_advertisement(raw, sizeof payload, 1u, random_hash,
                                        wrong_hash, hashmap);
    rns_resource_advertisement_t adv;
    assert(rns_resource_advertisement_parse(raw, length, &adv) == RNS_OK);
    rns_resource_t *resource = NULL;
    assert(rns_resource_accept(&resource, &adv, 0u) == RNS_OK);
    assert(rns_resource_receive_part(resource, stream, sizeof stream) == RNS_OK);
    /* An unknown part is rejected rather than stored. */
    assert(rns_resource_receive_part(resource, (const uint8_t *)"zzz", 3u) !=
           RNS_OK);
    uint8_t out[256];
    size_t out_length = 0u;
    assert(rns_resource_assemble(resource, NULL, out, sizeof out, &out_length) ==
           RNS_ERROR_PROTOCOL);
    /* No proof is available for a resource that failed verification. */
    uint8_t proof[RNS_RESOURCE_PROOF_SIZE];
    assert(rns_resource_build_proof(resource, proof) == RNS_ERROR_INVALID_STATE);
    rns_resource_destroy(resource);

    /* Too many parts for a single advertisement hashmap is refused. */
    adv.parts = RNS_RESOURCE_MAX_PARTS + 1u;
    adv.hashmap_length = adv.parts * RNS_RESOURCE_MAPHASH_LEN;
    assert(rns_resource_accept(&resource, &adv, 0u) == RNS_ERROR_UNSUPPORTED);
    rns_resource_destroy(NULL);
}

int main(void) {
    test_captured_advertisement();
    test_plain_transfer();
    test_corrupt_and_bounds();
    printf("bz2 decompression: %s\n",
           rns_resource_decompression_available() ? "available" : "absent");
    return 0;
}
