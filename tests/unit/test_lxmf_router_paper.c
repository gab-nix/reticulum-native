#include "reticulum/destination.h"
#include "reticulum/lxmf_delivery.h"
#include "reticulum/lxmf_paper.h"
#include "reticulum/lxmf_router.h"
#include "reticulum/packet.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef struct {
    rns_identity sender;
    rns_identity recipient;
    uint8_t sender_hash[LXMF_SOURCE_LENGTH];
    bool known;
    bool blocked;
    size_t callbacks;
    lxmf_store_message_t last;
} fixture_t;

static void delivery_hash(const rns_identity *identity, uint8_t hash[16]) {
    static const char *const aspects[] = {"delivery"};
    assert(rns_destination_hash(identity, "lxmf", aspects, 1u, hash));
}

static const rns_identity *resolve(void *context, const uint8_t hash[16]) {
    fixture_t *fixture = context;
    return fixture->known && memcmp(hash, fixture->sender_hash, 16u) == 0
               ? &fixture->sender
               : NULL;
}

static bool blocked(void *context, const uint8_t hash[16]) {
    fixture_t *fixture = context;
    return fixture->blocked && memcmp(hash, fixture->sender_hash, 16u) == 0;
}

static lxmf_status_t no_send(void *context, const uint8_t *packet,
                             size_t packet_length) {
    (void)context;
    (void)packet;
    (void)packet_length;
    return LXMF_OK;
}

static void received(void *context, const lxmf_store_message_t *message) {
    fixture_t *fixture = context;
    fixture->callbacks++;
    fixture->last = *message;
}

static lxmf_message_t message(fixture_t *fixture, double timestamp,
                              bool stamp) {
    static const uint8_t fields[] = {
        0x81, 0xcc, 0xe7, 0x92, 0xc4, 0x02, 0x00, 0xff, 0xa2, 'o', 'k'};
    lxmf_message_t value = {
        .timestamp = timestamp,
        .title = {(const uint8_t *)"Paper", 5u},
        .content = {(const uint8_t *)"offline message", 15u},
        .fields_msgpack = {fields, sizeof fields}};
    memcpy(value.source, fixture->sender_hash, sizeof value.source);
    delivery_hash(&fixture->recipient, value.destination);
    if (stamp) {
        value.has_stamp = true;
        value.stamp_len = LXMF_STAMP_LENGTH;
        memset(value.stamp, 0x4a, LXMF_STAMP_LENGTH);
    }
    return value;
}

static size_t make_paper(fixture_t *fixture, lxmf_message_t *value,
                         const uint8_t ratchet[32], uint8_t paper[2210],
                         uint8_t expected[2210], size_t *expected_length) {
    uint8_t transient[32];
    assert(lxmf_pack(value, lxmf_identity_signer, &fixture->sender, expected,
                     2210u, expected_length) == LXMF_OK);
    size_t paper_length = 0u;
    assert(lxmf_paper_pack(value, &fixture->sender, &fixture->recipient,
                           ratchet, paper, 2210u, &paper_length, transient) ==
           LXMF_OK);
    return paper_length;
}

static size_t make_forged_paper(fixture_t *fixture, lxmf_message_t *value,
                                uint8_t paper[2210]) {
    rns_identity forger;
    assert(rns_identity_generate(&forger));
    uint8_t packed[2210];
    size_t packed_length = 0u;
    assert(lxmf_pack(value, lxmf_identity_signer, &forger, packed,
                     sizeof packed, &packed_length) == LXMF_OK);
    memcpy(paper, value->destination, LXMF_DESTINATION_LENGTH);
    size_t encrypted_length = 0u;
    assert(rns_identity_encrypt(
        &fixture->recipient, NULL, packed + LXMF_DESTINATION_LENGTH,
        packed_length - LXMF_DESTINATION_LENGTH,
        paper + LXMF_DESTINATION_LENGTH,
        2210u - LXMF_DESTINATION_LENGTH, &encrypted_length));
    return LXMF_DESTINATION_LENGTH + encrypted_length;
}

int main(void) {
    fixture_t fixture = {.known = true};
    assert(rns_identity_generate(&fixture.sender));
    assert(rns_identity_generate(&fixture.recipient));
    delivery_hash(&fixture.sender, fixture.sender_hash);

    char path[] = "/tmp/lxmf-router-paper-XXXXXX";
    int descriptor = mkstemp(path);
    assert(descriptor >= 0);
    close(descriptor);
    lxmf_store_t store = {0};
    assert(lxmf_store_open(&store, path) == LXMF_OK);
    lxmf_router_config_t config = {
        .identity = &fixture.recipient,
        .store = &store,
        .resolve_identity = resolve,
        .resolve_context = &fixture,
        .send_packet = no_send,
        .is_source_blocked = blocked,
        .source_policy_context = &fixture,
        .inbound_stamp_cost = 254u,
        .message_callback = received,
        .message_context = &fixture};
    lxmf_router_t router;
    assert(lxmf_router_init(&router, &config) == LXMF_OK);

    uint8_t paper[2210], expected[2210];
    size_t expected_length = 0u;
    lxmf_message_t first = message(&fixture, 10.0, false);
    size_t paper_length = make_paper(&fixture, &first, NULL, paper, expected,
                                    &expected_length);
    uint8_t packet[RNS_MTU];
    size_t packet_length = 0u;
    assert(lxmf_opportunistic_packet_pack(
               &first, &fixture.sender, &fixture.recipient, packet,
               sizeof packet, &packet_length) == LXMF_OK);
    assert(lxmf_router_receive_packet(&router, packet, packet_length) ==
           LXMF_ERR_STAMP);
    char uri[LXMF_URI_MAX_CANONICAL_LENGTH + 1u];
    size_t uri_length = 0u;
    assert(lxmf_uri_encode(paper, paper_length, uri, sizeof uri, &uri_length) ==
           LXMF_OK);
    lxmf_router_paper_result_t result;
    assert(lxmf_router_receive_uri(&router, uri, uri_length, NULL, 0u, false,
                                   &result) == LXMF_OK);
    assert(!result.duplicate && !result.used_ratchet && fixture.callbacks == 1u &&
           fixture.last.signature_state == LXMF_SIGNATURE_VERIFIED &&
           fixture.last.delivery.actual_method ==
               LXMF_DELIVERY_METHOD_PROPAGATED);
    /* The configured cost is deliberately impossible for an absent stamp;
     * pinned paper ingestion exempts only this stamp check. */
    assert(lxmf_store_count(&store) == 1u);
    uint8_t first_id[LXMF_MESSAGE_ID_LENGTH];
    memcpy(first_id, fixture.last.message_id, sizeof first_id);
    uint8_t retained[2210];
    size_t retained_length = 0u;
    assert(lxmf_store_read_packed(&store, first_id, retained,
                                  sizeof retained, &retained_length) == LXMF_OK &&
           retained_length == expected_length &&
           memcmp(retained, expected, expected_length) == 0);

    assert(lxmf_router_receive_paper(&router, paper, paper_length, NULL, 0u,
                                     false, &result) == LXMF_OK &&
           result.duplicate && fixture.callbacks == 1u);

    fixture.known = false;
    lxmf_message_t unknown = message(&fixture, 11.0, true);
    paper_length = make_paper(&fixture, &unknown, NULL, paper, expected,
                              &expected_length);
    assert(lxmf_router_receive_paper(&router, paper, paper_length, NULL, 0u,
                                     false, &result) == LXMF_OK &&
           !result.duplicate && fixture.callbacks == 2u &&
           fixture.last.signature_state == LXMF_SIGNATURE_UNVERIFIED);
    assert(lxmf_store_unverified_count(&store) == 1u);

    fixture.blocked = true;
    lxmf_message_t denied = message(&fixture, 12.0, false);
    paper_length = make_paper(&fixture, &denied, NULL, paper, expected,
                              &expected_length);
    assert(lxmf_router_receive_paper(&router, paper, paper_length, NULL, 0u,
                                     false, &result) == LXMF_ERR_BLOCKED &&
           fixture.callbacks == 2u && lxmf_store_count(&store) == 2u);
    fixture.blocked = false;
    fixture.known = true;

    lxmf_message_t forged = message(&fixture, 13.0, false);
    paper_length = make_forged_paper(&fixture, &forged, paper);
    assert(lxmf_router_receive_paper(&router, paper, paper_length, NULL, 0u,
                                     false, &result) == LXMF_ERR_SIGNATURE);

    rns_identity other;
    assert(rns_identity_generate(&other));
    lxmf_message_t wrong = message(&fixture, 14.0, false);
    delivery_hash(&other, wrong.destination);
    uint8_t transient[32];
    assert(lxmf_paper_pack(&wrong, &fixture.sender, &other, NULL, paper,
                           sizeof paper, &paper_length, transient) == LXMF_OK);
    assert(lxmf_router_receive_paper(&router, paper, paper_length, NULL, 0u,
                                     false, &result) == LXMF_ERR_FORMAT);

    uint8_t ratchet_private[32], ratchet_public[32], ratchet_id[16];
    assert(rns_identity_ratchet_generate(ratchet_private, ratchet_public,
                                         ratchet_id));
    lxmf_message_t ratcheted = message(&fixture, 15.0, false);
    paper_length = make_paper(&fixture, &ratcheted, ratchet_public, paper,
                              expected, &expected_length);
    uint8_t explicit_empty = 0u;
    assert(lxmf_router_receive_paper(&router, paper, paper_length,
                                     &explicit_empty, 0u, true, &result) ==
           LXMF_ERR_CRYPTO);
    assert(lxmf_router_receive_paper(&router, paper, paper_length,
                                     ratchet_private, 1u, true, &result) ==
           LXMF_OK && result.used_ratchet &&
           memcmp(result.ratchet_id, ratchet_id, sizeof ratchet_id) == 0);
    assert(fixture.callbacks == 3u);

    assert(lxmf_router_receive_uri(&router, "lxmf://AAAA", 11u, NULL, 0u,
                                   false, &result) == LXMF_ERR_FORMAT);

    lxmf_router_destroy(&router);
    lxmf_store_close(&store);
    /* Durable message-ID suppression survives the process-local transient
     * cache being rebuilt. */
    assert(lxmf_store_open(&store, path) == LXMF_OK);
    config.store = &store;
    assert(lxmf_router_init(&router, &config) == LXMF_OK);
    assert(lxmf_router_receive_uri(&router, uri, uri_length, NULL, 0u, false,
                                   &result) == LXMF_OK && result.duplicate &&
           fixture.callbacks == 3u);
    lxmf_router_destroy(&router);

    config.max_incoming_message_size = expected_length - 1u;
    assert(lxmf_router_init(&router, &config) == LXMF_OK);
    lxmf_message_t oversized = message(&fixture, 17.0, false);
    paper_length = make_paper(&fixture, &oversized, NULL, paper, expected,
                              &expected_length);
    assert(lxmf_router_receive_paper(&router, paper, paper_length, NULL, 0u,
                                     false, &result) == LXMF_ERR_BOUNDS);
    lxmf_router_destroy(&router);
    config.max_incoming_message_size = 0u;
    lxmf_store_close(&store);
    unlink(path);

    /* The configured durable ratchet history is used when callers do not
     * supply an explicit history. */
    char ratchet_path[] = "/tmp/lxmf-paper-ratchets-XXXXXX";
    descriptor = mkstemp(ratchet_path);
    assert(descriptor >= 0);
    close(descriptor);
    unlink(ratchet_path);
    rns_ratchet_store_t *ratchet_store = NULL;
    assert(rns_ratchet_store_open(&ratchet_store, ratchet_path,
                                  &fixture.recipient, 2u, 3600u) == RNS_OK);
    bool rotated = false;
    assert(rns_ratchet_store_current(ratchet_store, 100u, ratchet_private,
                                     ratchet_public, ratchet_id, &rotated) ==
           RNS_OK && rotated);
    char second_path[] = "/tmp/lxmf-paper-store-ratchet-XXXXXX";
    descriptor = mkstemp(second_path);
    assert(descriptor >= 0);
    close(descriptor);
    assert(lxmf_store_open(&store, second_path) == LXMF_OK);
    config.store = &store;
    config.ratchet_store = ratchet_store;
    assert(lxmf_router_init(&router, &config) == LXMF_OK);
    lxmf_message_t stored_ratchet = message(&fixture, 16.0, false);
    paper_length = make_paper(&fixture, &stored_ratchet, ratchet_public, paper,
                              expected, &expected_length);
    assert(lxmf_router_receive_paper(&router, paper, paper_length, NULL, 0u,
                                     true, &result) == LXMF_OK &&
           result.used_ratchet &&
           memcmp(result.ratchet_id, ratchet_id, sizeof ratchet_id) == 0);
    lxmf_router_destroy(&router);
    lxmf_store_close(&store);
    rns_ratchet_store_close(ratchet_store);
    unlink(second_path);
    unlink(ratchet_path);
    return 0;
}
