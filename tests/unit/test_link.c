#include "reticulum/link.h"
#include "reticulum/packet.h"

#include <assert.h>
#include <string.h>

static double test_clock(void *context) {
    return *(const double *)context;
}

int main(void) {
    rns_identity destination;
    rns_link initiator;
    rns_link responder;
    rns_packet request_packet = {0};
    uint8_t request_payload[RNS_LINK_REQUEST_BYTES];
    uint8_t request_raw[RNS_MTU];
    uint8_t legacy_raw[RNS_MTU];
    uint8_t proof[RNS_LINK_PROOF_BYTES];
    uint8_t rtt_token[128];
    uint8_t encrypted[128];
    uint8_t decrypted[128];
    size_t request_length;
    size_t legacy_length;
    size_t token_length;
    size_t encrypted_length;
    size_t decrypted_length;
    double initiator_time = 100.0;
    double responder_time = 100.1;

    assert(rns_identity_generate(&destination));
    assert(rns_link_initiator_init(&initiator, &destination, RNS_MTU, 10.0,
                                   test_clock, &initiator_time));
    assert(rns_link_build_request_payload(&initiator, request_payload));

    request_packet.packet_type = 2;
    request_packet.destination_type = 0;
    request_packet.data = request_payload;
    request_packet.data_length = sizeof(request_payload);
    memcpy(request_packet.destination_hash, destination.hash,
           sizeof(request_packet.destination_hash));
    assert(rns_packet_encode(&request_packet, request_raw, sizeof(request_raw),
                             &request_length));
    assert(rns_link_initiator_set_request_packet(&initiator, request_raw,
                                                 request_length));
    assert(rns_link_responder_accept(&responder, &destination, request_raw,
                                     request_length, 10.0, test_clock,
                                     &responder_time));
    assert(memcmp(initiator.link_id, responder.link_id,
                  sizeof(initiator.link_id)) == 0);

    assert(rns_link_build_proof(&responder, proof));
    initiator_time = 100.2;
    assert(rns_link_initiator_accept_proof(&initiator, proof, sizeof(proof)));
    assert(initiator.state == RNS_LINK_ACTIVE);
    assert(memcmp(initiator.derived_key, responder.derived_key,
                  sizeof(initiator.derived_key)) == 0);

    assert(rns_link_build_rtt_confirm(&initiator, rtt_token, sizeof(rtt_token),
                                      &token_length));
    responder_time = 100.3;
    assert(rns_link_responder_accept_rtt(&responder, rtt_token, token_length));
    assert(responder.state == RNS_LINK_ACTIVE);

    assert(rns_link_encrypt(&initiator, (const uint8_t *)"hello", 5,
                            encrypted, sizeof(encrypted), &encrypted_length));
    assert(rns_link_decrypt(&responder, encrypted, encrypted_length, decrypted,
                            sizeof(decrypted), &decrypted_length));
    assert(decrypted_length == 5 && memcmp(decrypted, "hello", 5) == 0);

    encrypted[encrypted_length - 1] ^= 1;
    assert(!rns_link_decrypt(&responder, encrypted, encrypted_length, decrypted,
                             sizeof(decrypted), &decrypted_length));

    /* Peers predating MTU signalling send only their two ephemeral public
     * keys. Current responders accept that bounded legacy request and answer
     * with the current proof shape. */
    request_packet.data = request_payload;
    request_packet.data_length = RNS_LINK_REQUEST_KEY_BYTES;
    assert(rns_packet_encode(&request_packet, legacy_raw, sizeof legacy_raw,
                             &legacy_length));
    responder_time = 200.0;
    assert(rns_link_responder_accept(&responder, &destination, legacy_raw,
                                     legacy_length, 10.0, test_clock,
                                     &responder_time));
    assert(responder.mtu == RNS_MTU &&
           responder.mode == RNS_LINK_MODE_AES256_CBC);
    assert(rns_link_build_proof(&responder, proof));

    request_packet.data_length = RNS_LINK_REQUEST_KEY_BYTES - 1u;
    assert(rns_packet_encode(&request_packet, legacy_raw, sizeof legacy_raw,
                             &legacy_length));
    assert(!rns_link_responder_accept(&responder, &destination, legacy_raw,
                                      legacy_length, 10.0, test_clock,
                                      &responder_time));

    initiator.state = RNS_LINK_PENDING;
    initiator.deadline = 101.0;
    initiator_time = 101.0;
    assert(rns_link_check_timeout(&initiator));
    assert(initiator.state == RNS_LINK_CLOSED);
    return 0;
}
