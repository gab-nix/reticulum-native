#include "reticulum/destination.h"
#include "reticulum/lxmf_delivery.h"
#include "reticulum/packet.h"

#include <assert.h>
#include <string.h>

typedef struct {
    const rns_identity *identity;
    uint8_t hash[16];
} resolver_context;

static const rns_identity *resolve(void *context, const uint8_t hash[16]) {
    resolver_context *resolver = context;
    return memcmp(hash, resolver->hash, 16) == 0 ? resolver->identity : NULL;
}

static void delivery_hash(const rns_identity *identity, uint8_t out[16]) {
    const char *aspects[] = {"delivery"};
    assert(rns_destination_hash(identity, "lxmf", aspects, 1, out));
}

int main(void) {
    rns_identity alice;
    rns_identity bob;
    rns_identity bob_public;
    uint8_t bob_public_bytes[64];
    lxmf_message_t outbound = {0};
    lxmf_message_t inbound;
    uint8_t packet[RNS_MTU];
    uint8_t plaintext[RNS_MTU];
    uint8_t packed[RNS_MTU];
    uint8_t encrypted_plaintext[RNS_MTU];
    size_t packet_length;
    size_t plaintext_length;
    size_t packed_length;
    size_t encrypted_plaintext_length;
    resolver_context resolver;
    lxmf_identity_verifier_context_t verifier;

    assert(rns_identity_generate(&alice));
    assert(rns_identity_generate(&bob));
    rns_identity_export_public(&bob, bob_public_bytes);
    assert(rns_identity_from_public(&bob_public, bob_public_bytes));
    delivery_hash(&alice, outbound.source);
    delivery_hash(&bob, outbound.destination);
    outbound.timestamp = 123.5;
    outbound.content.data = (const uint8_t *)"hello over Reticulum";
    outbound.content.len = strlen((const char *)outbound.content.data);

    assert(lxmf_opportunistic_packet_pack(&outbound, &alice, &bob_public, packet,
                                          sizeof(packet), &packet_length) == LXMF_OK);

    /* Match the pinned Python wire representation exactly: the destination is
     * in the outer packet, and only the packed tail is encrypted. This check
     * deliberately decrypts without using the LXMF unpack helper so a matching
     * encoder/decoder bug cannot hide the incompatibility. */
    assert(lxmf_pack(&outbound, lxmf_identity_signer, &alice, packed,
                     sizeof packed, &packed_length) == LXMF_OK);
    rns_packet outer;
    assert(rns_packet_decode(&outer, packet, packet_length));
    assert(memcmp(outer.destination_hash, packed, LXMF_DESTINATION_LENGTH) == 0);
    assert(rns_identity_decrypt(&bob, outer.data, outer.data_length,
                                encrypted_plaintext, sizeof encrypted_plaintext,
                                &encrypted_plaintext_length));
    assert(encrypted_plaintext_length == packed_length - LXMF_DESTINATION_LENGTH);
    assert(memcmp(encrypted_plaintext, packed + LXMF_DESTINATION_LENGTH,
                  encrypted_plaintext_length) == 0);

    resolver.identity = &alice;
    delivery_hash(&alice, resolver.hash);
    verifier.resolve = resolve;
    verifier.resolve_context = &resolver;
    assert(lxmf_opportunistic_packet_unpack(packet, packet_length, &bob,
                                            lxmf_identity_verifier, &verifier,
                                            plaintext, sizeof(plaintext),
                                            &plaintext_length, &inbound) == LXMF_OK);
    assert(plaintext_length == packed_length);
    assert(memcmp(plaintext, packed, packed_length) == 0);
    assert(inbound.content.len == outbound.content.len);
    assert(memcmp(inbound.content.data, outbound.content.data, outbound.content.len) == 0);

    uint8_t ratchet_private[32], ratchet_public[32], ratchet_id[16];
    uint8_t used_id[16];
    int used_ratchet = 0;
    assert(rns_identity_ratchet_generate(ratchet_private, ratchet_public,
                                         ratchet_id));
    assert(lxmf_opportunistic_packet_pack_ratchet(
               &outbound, &alice, &bob_public, ratchet_public, packet,
               sizeof packet, &packet_length) == LXMF_OK);
    assert(rns_packet_decode(&outer, packet, packet_length));
    assert(!rns_identity_decrypt(&bob, outer.data, outer.data_length,
                                 encrypted_plaintext,
                                 sizeof encrypted_plaintext,
                                 &encrypted_plaintext_length));
    assert(lxmf_opportunistic_packet_unpack_ratchets(
               packet, packet_length, &bob, ratchet_private, 1u, 1,
               lxmf_identity_verifier, &verifier, plaintext,
               sizeof plaintext, &plaintext_length, &inbound, used_id,
               &used_ratchet) == LXMF_OK);
    assert(used_ratchet && memcmp(used_id, ratchet_id, sizeof used_id) == 0 &&
           plaintext_length == packed_length &&
           memcmp(plaintext, packed, packed_length) == 0);

    assert(lxmf_opportunistic_packet_pack(
               &outbound, &alice, &bob_public, packet, sizeof packet,
               &packet_length) == LXMF_OK);
    assert(lxmf_opportunistic_packet_unpack_ratchets(
               packet, packet_length, &bob, ratchet_private, 1u, 1,
               lxmf_identity_verifier, &verifier, plaintext,
               sizeof plaintext, &plaintext_length, &inbound, used_id,
               &used_ratchet) == LXMF_ERR_CRYPTO);

    packet[packet_length - 1] ^= 1;
    assert(lxmf_opportunistic_packet_unpack(packet, packet_length, &bob,
                                            lxmf_identity_verifier, &verifier,
                                            plaintext, sizeof(plaintext),
                                            &plaintext_length, &inbound) == LXMF_ERR_CRYPTO);
    assert(plaintext_length == 0u);
    assert(lxmf_opportunistic_packet_unpack(packet, packet_length, &bob,
                                            lxmf_identity_verifier, &verifier,
                                            plaintext, LXMF_DESTINATION_LENGTH - 1u,
                                            &plaintext_length, &inbound) == LXMF_ERR_BOUNDS);
    return 0;
}
