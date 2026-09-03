#include "reticulum/resource.h"
#include "reticulum/packet.h"

#include <assert.h>
#include <string.h>

static void initialise_link(rns_link *link) {
    memset(link, 0, sizeof *link);
    link->state = RNS_LINK_ACTIVE;
    link->mode = RNS_LINK_MODE_AES256_CBC;
    link->mtu = 500u;
    for (size_t i = 0u; i < sizeof link->derived_key; ++i)
        link->derived_key[i] = (uint8_t)(i * 13u + 7u);
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
    if (rns_resource_decompression_available())
        assert(advertisement.compressed);
    rns_resource_sender_destroy(sender);

    rns_resource_sender_options_t invalid = {
        .auto_compress = false, .is_response = true, .request_id = NULL};
    assert(rns_resource_sender_create(&sender, &link, compressible,
                                      sizeof compressible, &invalid) ==
           RNS_ERROR_INVALID_ARGUMENT);
}

int main(void) {
    transfer_resource(false);
    transfer_resource(true);
    test_compression_and_bounds();
    return 0;
}
