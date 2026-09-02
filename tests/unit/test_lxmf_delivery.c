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
    size_t packet_length;
    size_t plaintext_length;
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
    resolver.identity = &alice;
    delivery_hash(&alice, resolver.hash);
    verifier.resolve = resolve;
    verifier.resolve_context = &resolver;
    assert(lxmf_opportunistic_packet_unpack(packet, packet_length, &bob,
                                            lxmf_identity_verifier, &verifier,
                                            plaintext, sizeof(plaintext),
                                            &plaintext_length, &inbound) == LXMF_OK);
    assert(inbound.content.len == outbound.content.len);
    assert(memcmp(inbound.content.data, outbound.content.data, outbound.content.len) == 0);

    packet[packet_length - 1] ^= 1;
    assert(lxmf_opportunistic_packet_unpack(packet, packet_length, &bob,
                                            lxmf_identity_verifier, &verifier,
                                            plaintext, sizeof(plaintext),
                                            &plaintext_length, &inbound) == LXMF_ERR_CRYPTO);
    return 0;
}
