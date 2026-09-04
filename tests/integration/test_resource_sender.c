#include "reticulum/resource.h"
#include "reticulum/crypto.h"
#include "reticulum/packet.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>

static void initialise_link(rns_link *link) {
    memset(link, 0, sizeof *link);
    link->state = RNS_LINK_ACTIVE;
    link->mode = RNS_LINK_MODE_AES256_CBC;
    link->mtu = 500u;
    for (size_t i = 0u; i < sizeof link->derived_key; ++i)
        link->derived_key[i] = (uint8_t)(i * 13u + 7u);
}

static size_t find_last_key_before(const uint8_t *data, size_t limit,
                                   uint8_t key) {
    assert(limit >= 2U);
    for (size_t i = limit - 1U; i > 0U; --i)
        if (data[i - 1U] == 0xa1U && data[i] == key) return i;
    assert(false);
    return 0U;
}

static size_t find_first_key_before(const uint8_t *data, size_t limit,
                                    uint8_t key) {
    assert(limit >= 2U);
    for (size_t i = 1U; i < limit; ++i)
        if (data[i - 1U] == 0xa1U && data[i] == key) return i;
    assert(false);
    return 0U;
}

static void transfer_resource(bool response) {
    rns_link link;
    initialise_link(&link);
    uint8_t source[1400];
    for (size_t i = 0u; i < sizeof source; ++i)
        source[i] = (uint8_t)(i * 73u + i / 11u);
    uint8_t request_id[RNS_RESOURCE_REQUEST_ID_SIZE];
    for (size_t i = 0u; i < sizeof request_id; ++i)
        request_id[i] = (uint8_t)(0xa0u + i);
    rns_resource_sender_options_t options = {
        .auto_compress = false,
        .is_response = response,
        .request_id = response ? request_id : NULL};
    rns_resource_sender_t *sender = NULL;
    assert(rns_resource_sender_create(&sender, &link, source, sizeof source,
                                      &options) == RNS_OK);
    assert(sender != NULL);
    assert(rns_resource_sender_data_size(sender) == sizeof source);
    assert(rns_resource_sender_transfer_size(sender) > sizeof source);
    assert(rns_resource_sender_total_parts(sender) == 4u);

    uint8_t packed_advertisement[RNS_MTU];
    size_t advertisement_length = 0u;
    assert(rns_resource_sender_advertisement(
               sender, packed_advertisement, sizeof packed_advertisement,
               &advertisement_length) == RNS_OK);
    rns_resource_advertisement_t advertisement;
    assert(rns_resource_advertisement_parse(
               packed_advertisement, advertisement_length,
               &advertisement) == RNS_OK);
    assert(advertisement.transfer_size ==
           rns_resource_sender_transfer_size(sender));
    assert(advertisement.data_size == sizeof source);
    assert(advertisement.parts == 4u);
    assert(advertisement.encrypted && !advertisement.compressed);
    assert(!advertisement.split && advertisement.segment_index == 1u &&
           advertisement.total_segments == 1u);
    assert(advertisement.is_response == response);
    assert(advertisement.has_request_id == response);
    if (response)
        assert(memcmp(advertisement.request_id, request_id,
                      sizeof request_id) == 0);
    assert(memcmp(advertisement.hash, advertisement.original_hash,
                  RNS_RESOURCE_HASH_SIZE) == 0);
    assert(memcmp(advertisement.hash, rns_resource_sender_hash(sender),
                  RNS_RESOURCE_HASH_SIZE) == 0);

    rns_resource_t *receiver = NULL;
    assert(rns_resource_accept(&receiver, &advertisement, sizeof source) ==
           RNS_OK);
    while (!rns_resource_parts_complete(receiver)) {
        uint8_t request[RNS_MTU];
        size_t request_length = 0u;
        assert(rns_resource_build_request(receiver, request, sizeof request,
                                          &request_length) == RNS_OK);
        size_t indexes[RNS_RESOURCE_WINDOW];
        size_t count = 0u;
        assert(rns_resource_sender_requested_parts(
                   sender, request, request_length, indexes,
                   RNS_RESOURCE_WINDOW, &count) == RNS_OK);
        assert(count > 0u && count <= RNS_RESOURCE_WINDOW);
        /* Reordered requested parts remain matchable by their map hashes. */
        for (size_t offset = count; offset > 0u; --offset) {
            const uint8_t *part = NULL;
            size_t part_length = 0u;
            assert(rns_resource_sender_part(sender, indexes[offset - 1u],
                                            &part, &part_length) == RNS_OK);
            assert(part_length > 0u && part_length <= RNS_RESOURCE_PART_MAX);
            assert(rns_resource_receive_part(receiver, part, part_length) ==
                   RNS_OK);
        }
    }
    uint8_t assembled[sizeof source];
    size_t assembled_length = 0u;
    assert(rns_resource_assemble(receiver, &link, assembled, sizeof assembled,
                                 &assembled_length) == RNS_OK);
    assert(assembled_length == sizeof source);
    assert(memcmp(assembled, source, sizeof source) == 0);
    uint8_t proof[RNS_RESOURCE_PROOF_SIZE];
    assert(rns_resource_build_proof(receiver, proof) == RNS_OK);
    assert(rns_resource_sender_validate_proof(sender, proof, sizeof proof) ==
           RNS_OK);
    proof[sizeof proof - 1u] ^= 0x01u;
    assert(rns_resource_sender_validate_proof(sender, proof, sizeof proof) ==
           RNS_ERROR_CRYPTO);

    uint8_t malformed_request[1u + RNS_RESOURCE_HASH_SIZE +
                              RNS_RESOURCE_MAPHASH_LEN] = {0};
    size_t indexes[1];
    size_t count = 0u;
    assert(rns_resource_sender_requested_parts(
               sender, malformed_request, sizeof malformed_request, indexes,
               1u, &count) == RNS_ERROR_PROTOCOL);
    rns_resource_destroy(receiver);
    rns_resource_sender_destroy(sender);
}

static void test_compression_and_bounds(void) {
    rns_link link;
    initialise_link(&link);
    uint8_t compressible[4096];
    memset(compressible, 'x', sizeof compressible);
    rns_resource_sender_t *sender = NULL;
    assert(rns_resource_sender_create(&sender, &link, compressible,
                                      sizeof compressible, NULL) == RNS_OK);
    uint8_t advertisement_bytes[RNS_MTU];
    size_t advertisement_length = 0u;
    assert(rns_resource_sender_advertisement(
               sender, advertisement_bytes, sizeof advertisement_bytes,
               &advertisement_length) == RNS_OK);
    rns_resource_advertisement_t advertisement;
    assert(rns_resource_advertisement_parse(
               advertisement_bytes, advertisement_length, &advertisement) ==
           RNS_OK);
    if (rns_resource_decompression_available()) {
        assert(advertisement.compressed);
        rns_resource_advertisement_t bounded = advertisement;
        bounded.data_size = 100U;
        rns_resource_t *receiver = NULL;
        assert(rns_resource_accept(&receiver, &bounded, 100U) == RNS_OK);
        for (size_t i = 0U; i < rns_resource_sender_total_parts(sender); ++i) {
            const uint8_t *part = NULL;
            size_t part_length = 0U;
            assert(rns_resource_sender_part(sender, i, &part, &part_length) ==
                   RNS_OK);
            assert(rns_resource_receive_part(receiver, part, part_length) ==
                   RNS_OK);
        }
        uint8_t output[sizeof compressible];
        memset(output, 0xa5, sizeof output);
        size_t output_length = 0U;
        assert(rns_resource_assemble(receiver, &link, output, sizeof output,
                                     &output_length) == RNS_ERROR_OVERFLOW);
        for (size_t i = bounded.data_size; i < sizeof output; ++i)
            assert(output[i] == 0xa5U);
        rns_resource_destroy(receiver);
    }
    rns_resource_sender_destroy(sender);

    rns_resource_sender_options_t invalid = {
        .auto_compress = false, .is_response = true, .request_id = NULL};
    assert(rns_resource_sender_create(&sender, &link, compressible,
                                      sizeof compressible, &invalid) ==
           RNS_ERROR_INVALID_ARGUMENT);
    assert(rns_resource_sender_create(&sender, &link, compressible,
                                      RNS_RESOURCE_MAX_SIZE + 1u, NULL) ==
           RNS_ERROR_INVALID_ARGUMENT);
}

static void test_encrypted_stream_prefix_is_independent(void) {
    rns_link link;
    initialise_link(&link);
    uint8_t source[100];
    for (size_t i = 0U; i < sizeof source; ++i)
        source[i] = (uint8_t)(i * 29U + 11U);
    const uint8_t advertised_random[4] = {0x10U, 0x20U, 0x30U, 0x40U};
    const uint8_t stream_prefix[4] = {0xa1U, 0xb2U, 0xc3U, 0xd4U};
    uint8_t hash_input[sizeof source + 4U];
    memcpy(hash_input, source, sizeof source);
    memcpy(hash_input + sizeof source, advertised_random,
           sizeof advertised_random);

    rns_resource_advertisement_t advertisement = {0};
    assert(rns_sha256(hash_input, sizeof hash_input, advertisement.hash));
    memcpy(advertisement.original_hash, advertisement.hash,
           sizeof advertisement.original_hash);
    memcpy(advertisement.random_hash, advertised_random,
           sizeof advertisement.random_hash);
    advertisement.data_size = sizeof source;
    advertisement.parts = 1U;
    advertisement.segment_index = 1U;
    advertisement.total_segments = 1U;
    advertisement.flags = RNS_RESOURCE_FLAG_ENCRYPTED;

    uint8_t token_plain[sizeof source + 4U];
    memcpy(token_plain, stream_prefix, sizeof stream_prefix);
    memcpy(token_plain + sizeof stream_prefix, source, sizeof source);
    uint8_t wire[256];
    assert(rns_link_encrypt(&link, token_plain, sizeof token_plain, wire,
                            sizeof wire, &advertisement.transfer_size));
    uint8_t part_hash_input[sizeof wire + 4U];
    assert(advertisement.transfer_size + sizeof advertised_random <=
           sizeof part_hash_input);
    memcpy(part_hash_input, wire, advertisement.transfer_size);
    memcpy(part_hash_input + advertisement.transfer_size, advertised_random,
           sizeof advertised_random);
    uint8_t part_digest[32];
    assert(rns_sha256(part_hash_input,
                      advertisement.transfer_size + sizeof advertised_random,
                      part_digest));
    advertisement.hashmap = part_digest;
    advertisement.hashmap_length = RNS_RESOURCE_MAPHASH_LEN;

    rns_resource_t *receiver = NULL;
    assert(rns_resource_accept(&receiver, &advertisement, sizeof source) ==
           RNS_OK);
    assert(rns_resource_receive_part(receiver, wire,
                                     advertisement.transfer_size) == RNS_OK);
    uint8_t output[sizeof source];
    size_t output_length = 0U;
    assert(rns_resource_assemble(receiver, &link, output, sizeof output,
                                 &output_length) == RNS_OK);
    assert(output_length == sizeof source);
    assert(memcmp(output, source, sizeof source) == 0);
    rns_resource_destroy(receiver);
}

static void receive_current_segment(rns_resource_sender_t *sender,
                                    const rns_link *link, uint8_t *output,
                                    size_t capacity, size_t *output_length) {
    uint8_t advertisement_bytes[RNS_MTU];
    size_t advertisement_length = 0u;
    assert(rns_resource_sender_advertisement(
               sender, advertisement_bytes, sizeof advertisement_bytes,
               &advertisement_length) == RNS_OK);
    rns_resource_advertisement_t advertisement;
    assert(rns_resource_advertisement_parse(
               advertisement_bytes, advertisement_length, &advertisement) ==
           RNS_OK);
    assert(advertisement.hashmap_length <=
           RNS_RESOURCE_HASHMAP_MAX_ENTRIES * RNS_RESOURCE_MAPHASH_LEN);
    rns_resource_t *receiver = NULL;
    assert(rns_resource_accept(&receiver, &advertisement,
                               RNS_RESOURCE_MAX_SIZE) == RNS_OK);
    bool duplicated = false;
    while (!rns_resource_parts_complete(receiver)) {
        uint8_t request[RNS_MTU];
        size_t request_length = 0u;
        assert(rns_resource_build_request(receiver, request, sizeof request,
                                          &request_length) == RNS_OK);
        size_t indexes[RNS_RESOURCE_WINDOW];
        size_t count = 0u;
        assert(rns_resource_sender_requested_parts(
                   sender, request, request_length, indexes,
                   RNS_RESOURCE_WINDOW, &count) == RNS_OK);
        for (size_t i = count; i > 0u; --i) {
            const uint8_t *part = NULL;
            size_t part_length = 0u;
            assert(rns_resource_sender_part(sender, indexes[i - 1u], &part,
                                            &part_length) == RNS_OK);
            assert(rns_resource_receive_part(receiver, part, part_length) ==
                   RNS_OK);
            if (!duplicated) {
                size_t received = rns_resource_received_parts(receiver);
                assert(rns_resource_receive_part(receiver, part, part_length) ==
                       RNS_OK);
                assert(rns_resource_received_parts(receiver) == received);
                duplicated = true;
            }
        }
        if (rns_resource_waiting_for_hashmap(receiver)) {
            uint8_t update[RNS_MTU];
            size_t update_length = 0u;
            assert(rns_resource_sender_hashmap_update(
                       sender, request, request_length, update, sizeof update,
                       &update_length) == RNS_OK);
            uint8_t corrupt[RNS_MTU];
            memcpy(corrupt, update, update_length);
            corrupt[0] ^= 1u;
            assert(rns_resource_apply_hashmap_update(receiver, corrupt,
                                                      update_length) ==
                   RNS_ERROR_PROTOCOL);
            memcpy(corrupt, update, update_length);
            assert(corrupt[RNS_RESOURCE_HASH_SIZE] == 0x92U);
            corrupt[RNS_RESOURCE_HASH_SIZE + 1U] = 0x7fU;
            assert(rns_resource_apply_hashmap_update(receiver, corrupt,
                                                      update_length) ==
                   RNS_ERROR_PROTOCOL);
            assert(rns_resource_apply_hashmap_update(receiver, update,
                                                      update_length) == RNS_OK);
            /* An identical reordered/duplicate HMU is harmless. */
            assert(rns_resource_apply_hashmap_update(receiver, update,
                                                      update_length) == RNS_OK);
            memcpy(corrupt, update, update_length);
            corrupt[update_length - 1U] ^= 1U;
            assert(rns_resource_apply_hashmap_update(receiver, corrupt,
                                                      update_length) ==
                   RNS_ERROR_PROTOCOL);
        }
    }
    assert(rns_resource_assemble(receiver, link, output, capacity,
                                 output_length) == RNS_OK);
    uint8_t proof[RNS_RESOURCE_PROOF_SIZE];
    assert(rns_resource_build_proof(receiver, proof) == RNS_OK);
    assert(rns_resource_sender_validate_proof(sender, proof, sizeof proof) ==
           RNS_OK);
    rns_resource_destroy(receiver);
}

static void test_hashmap_updates_and_segments(void) {
    rns_link link;
    initialise_link(&link);
    const size_t source_length = RNS_RESOURCE_SINGLE_SEGMENT_MAX_SIZE + 257u;
    uint8_t *source = malloc(source_length);
    uint8_t *assembled = malloc(source_length);
    assert(source != NULL && assembled != NULL);
    for (size_t i = 0u; i < source_length; ++i)
        source[i] = (uint8_t)(i * 37u + i / 251u);
    rns_resource_sender_options_t options = {.auto_compress = false};
    rns_resource_sender_t *sender = NULL;
    assert(rns_resource_sender_create(&sender, &link, source, source_length,
                                      &options) == RNS_OK);
    assert(rns_resource_sender_total_segments(sender) == 2u);
    assert(rns_resource_sender_total_parts(sender) >
           RNS_RESOURCE_HASHMAP_MAX_ENTRIES);

    size_t assembled_total = 0u;
    for (size_t segment = 1u; segment <= 2u; ++segment) {
        assert(rns_resource_sender_segment_index(sender) == segment);
        size_t segment_length = 0u;
        receive_current_segment(sender, &link, assembled + assembled_total,
                                source_length - assembled_total,
                                &segment_length);
        assembled_total += segment_length;
        if (segment != 2u) {
            rns_link inactive = link;
            inactive.state = RNS_LINK_CLOSED;
            uint8_t previous_hash[RNS_RESOURCE_HASH_SIZE];
            memcpy(previous_hash, rns_resource_sender_hash(sender),
                   sizeof previous_hash);
            size_t previous_parts = rns_resource_sender_total_parts(sender);
            assert(rns_resource_sender_advance_segment(sender, &inactive) ==
                   RNS_ERROR_INVALID_ARGUMENT);
            assert(rns_resource_sender_segment_index(sender) == segment);
            assert(rns_resource_sender_total_parts(sender) == previous_parts);
            assert(memcmp(rns_resource_sender_hash(sender), previous_hash,
                          sizeof previous_hash) == 0);
            assert(rns_resource_sender_advance_segment(sender, &link) ==
                   RNS_OK);
        }
    }
    assert(rns_resource_sender_advance_segment(sender, &link) ==
           RNS_ERROR_INVALID_STATE);
    assert(assembled_total == source_length);
    assert(memcmp(assembled, source, source_length) == 0);
    assert(rns_resource_sender_total_data_parts(sender) >
           RNS_RESOURCE_HASHMAP_MAX_ENTRIES);
    rns_resource_sender_destroy(sender);
    free(assembled);
    free(source);
}

static void test_advertisement_hardening(void) {
    rns_link link;
    initialise_link(&link);
    uint8_t source[1400];
    for (size_t i = 0U; i < sizeof source; ++i)
        source[i] = (uint8_t)(i * 19U + 3U);
    rns_resource_sender_options_t options = {.auto_compress = false};
    rns_resource_sender_t *sender = NULL;
    assert(rns_resource_sender_create(&sender, &link, source, sizeof source,
                                      &options) == RNS_OK);
    uint8_t advertisement_bytes[RNS_MTU];
    size_t advertisement_length = 0U;
    assert(rns_resource_sender_advertisement(
               sender, advertisement_bytes, sizeof advertisement_bytes,
               &advertisement_length) == RNS_OK);
    rns_resource_advertisement_t advertisement;
    assert(rns_resource_advertisement_parse(
               advertisement_bytes, advertisement_length,
               &advertisement) == RNS_OK);
    size_t map_offset = (size_t)(advertisement.hashmap - advertisement_bytes);
    size_t hash_key = find_first_key_before(advertisement_bytes, map_offset, 'h');
    uint8_t malformed[RNS_MTU];

    assert(hash_key + 1U < advertisement_length &&
           advertisement_bytes[hash_key + 1U] == 0xc4U);
    memcpy(malformed, advertisement_bytes, advertisement_length);
    malformed[hash_key + 1U] = 0xd9U;
    assert(rns_resource_advertisement_parse(
               malformed, advertisement_length, &advertisement) ==
           RNS_ERROR_PROTOCOL);
    memcpy(malformed, advertisement_bytes, advertisement_length);
    malformed[find_last_key_before(malformed, map_offset, 'q')] = 'z';
    assert(rns_resource_advertisement_parse(
               malformed, advertisement_length, &advertisement) ==
           RNS_ERROR_PROTOCOL);
    memcpy(malformed, advertisement_bytes, advertisement_length);
    malformed[find_last_key_before(malformed, map_offset, 'q')] = 'f';
    assert(rns_resource_advertisement_parse(
               malformed, advertisement_length, &advertisement) ==
           RNS_ERROR_PROTOCOL);
    memcpy(malformed, advertisement_bytes, advertisement_length);
    malformed[find_last_key_before(malformed, map_offset, 'm')] = 'z';
    assert(rns_resource_advertisement_parse(
               malformed, advertisement_length, &advertisement) ==
           RNS_ERROR_PROTOCOL);

    assert(rns_resource_advertisement_parse(
               advertisement_bytes, advertisement_length,
               &advertisement) == RNS_OK);
    rns_resource_t *receiver = NULL;
    advertisement.parts = 1U;
    assert(rns_resource_accept(&receiver, &advertisement,
                               RNS_RESOURCE_MAX_SIZE) == RNS_ERROR_PROTOCOL);
    advertisement.transfer_size = 8U;
    advertisement.parts = RNS_RESOURCE_MAX_PARTS;
    assert(rns_resource_accept(&receiver, &advertisement,
                               RNS_RESOURCE_MAX_SIZE) == RNS_ERROR_PROTOCOL);
    assert(rns_resource_advertisement_parse(
               advertisement_bytes, advertisement_length,
               &advertisement) == RNS_OK);
    advertisement.transfer_size -= 16U;
    assert(rns_resource_accept(&receiver, &advertisement,
                               RNS_RESOURCE_MAX_SIZE) == RNS_OK);
    for (size_t i = 0U; i < rns_resource_sender_total_parts(sender); ++i) {
        const uint8_t *part = NULL;
        size_t part_length = 0U;
        assert(rns_resource_sender_part(sender, i, &part, &part_length) ==
               RNS_OK);
        assert(rns_resource_receive_part(receiver, part, part_length) ==
               RNS_OK);
    }
    uint8_t output[sizeof source];
    size_t output_length = 0U;
    assert(rns_resource_assemble(receiver, &link, output, sizeof output,
                                 &output_length) == RNS_ERROR_PROTOCOL);
    rns_resource_destroy(receiver);
    rns_resource_sender_destroy(sender);
}

int main(void) {
    transfer_resource(false);
    transfer_resource(true);
    test_compression_and_bounds();
    test_encrypted_stream_prefix_is_independent();
    test_hashmap_updates_and_segments();
    test_advertisement_hardening();
    return 0;
}
