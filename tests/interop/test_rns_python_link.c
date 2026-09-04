#include "reticulum/crypto.h"
#include "reticulum/destination.h"
#include "reticulum/identity.h"
#include "reticulum/link.h"
#include "reticulum/packet.h"

#include "../fixtures/rns_link_vectors.h"

#include <assert.h>
#include <string.h>

static double fixture_clock(void *context) {
    return *(const double *)context;
}

int main(void) {
    static const char *aspects[] = {"delivery"};
    rns_identity destination;
    rns_identity identifying;
    rns_packet packet = {0};
    rns_link link = {0};
    uint8_t destination_hash[16];
    uint8_t signalling[3];
    uint8_t request_raw[RNS_MTU];
    uint8_t proof_raw[RNS_MTU];
    uint8_t proof_signed_data[83];
    uint8_t identification_signed_data[80];
    uint8_t rtt_token[128];
    uint8_t rtt_plaintext[32];
    size_t request_raw_len = 0u;
    size_t proof_raw_len = 0u;
    size_t rtt_token_len = 0u;
    size_t rtt_plaintext_len = 0u;
    uint32_t mtu = 0u;
    uint8_t mode = 0u;
    double now = 10.0;

    assert(rns_identity_from_public(&destination, rns_link_destination_public));
    assert(rns_destination_hash(&destination, "lxmf", aspects, 1u,
                                destination_hash));
    assert(memcmp(destination_hash, rns_link_destination_hash, 16u) == 0);

    assert(rns_link_signalling_encode(500u, RNS_LINK_MODE_AES256_CBC, signalling));
    assert(memcmp(signalling, rns_link_signalling, sizeof signalling) == 0);
    assert(rns_link_signalling_decode(rns_link_signalling, &mtu, &mode));
    assert(mtu == 500u && mode == RNS_LINK_MODE_AES256_CBC);

    packet.packet_type = 2u;
    memcpy(packet.destination_hash, rns_link_destination_hash, 16u);
    packet.data = rns_link_request_payload;
    packet.data_length = sizeof rns_link_request_payload;
    assert(rns_packet_encode(&packet, request_raw, sizeof request_raw,
                             &request_raw_len));
    assert(request_raw_len == sizeof rns_link_request_raw);
    assert(memcmp(request_raw, rns_link_request_raw, request_raw_len) == 0);
    assert(rns_link_id_from_request_packet(request_raw, request_raw_len,
                                           destination_hash));
    assert(memcmp(destination_hash, rns_link_id, 16u) == 0);

    memcpy(proof_signed_data, rns_link_id, 16u);
    memcpy(proof_signed_data + 16u, rns_link_responder_public, 32u);
    memcpy(proof_signed_data + 48u, destination.signing_public, 32u);
    memcpy(proof_signed_data + 80u, rns_link_signalling, 3u);
    assert(memcmp(proof_signed_data, rns_link_proof_signed_data,
                  sizeof proof_signed_data) == 0);
    assert(rns_identity_verify(&destination, proof_signed_data,
                               sizeof proof_signed_data, rns_link_proof_data));

    packet.destination_type = 3u;
    packet.packet_type = 3u;
    packet.context = 0xffu;
    memcpy(packet.destination_hash, rns_link_id, 16u);
    packet.data = rns_link_proof_data;
    packet.data_length = sizeof rns_link_proof_data;
    assert(rns_packet_encode(&packet, proof_raw, sizeof proof_raw, &proof_raw_len));
    assert(proof_raw_len == sizeof rns_link_proof_raw);
    assert(memcmp(proof_raw, rns_link_proof_raw, proof_raw_len) == 0);

    link.role = RNS_LINK_INITIATOR;
    link.state = RNS_LINK_ACTIVE;
    link.rtt = 0.125;
    link.clock = fixture_clock;
    link.clock_context = &now;
    for (size_t i = 0u; i < sizeof link.derived_key; ++i)
        link.derived_key[i] = (uint8_t)(i + 1u);
    assert(rns_link_build_rtt_confirm(&link, rtt_token, sizeof rtt_token,
                                      &rtt_token_len));
    assert(rns_token_decrypt(link.derived_key, rtt_token, rtt_token_len,
                             rtt_plaintext, sizeof rtt_plaintext,
                             &rtt_plaintext_len));
    assert(rtt_plaintext_len == sizeof rns_link_rtt_plaintext);
    assert(memcmp(rtt_plaintext, rns_link_rtt_plaintext,
                  rtt_plaintext_len) == 0);

    assert(rns_identity_from_public(&identifying, rns_link_identification_public));
    memcpy(identification_signed_data, rns_link_id, 16u);
    memcpy(identification_signed_data + 16u, rns_link_identification_public, 64u);
    assert(memcmp(identification_signed_data,
                  rns_link_identification_signed_data,
                  sizeof identification_signed_data) == 0);
    assert(memcmp(rns_link_identification_data,
                  rns_link_identification_public, 64u) == 0);
    assert(rns_identity_verify(&identifying, identification_signed_data,
                               sizeof identification_signed_data,
                               rns_link_identification_data + 64u));
    return 0;
}
