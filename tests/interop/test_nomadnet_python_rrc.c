#include "reticulum/rrc.h"

#include "../fixtures/nomadnet_rrc_vectors.h"

#include <assert.h>
#include <string.h>

static void assert_slice(rns_rrc_slice_t actual, const uint8_t *expected,
                         size_t expected_len) {
    assert(actual.length == expected_len);
    assert(expected_len == 0u || memcmp(actual.data, expected, expected_len) == 0);
}

int main(void) {
    uint64_t seen_types = 0u;
    for (size_t i = 0u; i < NOMADNET_RRC_FIXTURE_COUNT; ++i) {
        const nomadnet_rrc_fixture *fixture = &nomadnet_rrc_fixtures[i];
        rns_rrc_envelope_t envelope;
        uint8_t encoded[RNS_RRC_MAX_ENVELOPE_SIZE];
        size_t encoded_len = 0u;

        assert(rns_rrc_envelope_parse(fixture->wire, fixture->wire_len,
                                      &envelope) == RNS_OK);
        assert(envelope.version == RNS_RRC_VERSION);
        assert((uint8_t)envelope.type == fixture->type);
        assert(fixture->type < 64u);
        assert((seen_types & (UINT64_C(1) << fixture->type)) == 0u);
        seen_types |= UINT64_C(1) << fixture->type;
        assert(memcmp(envelope.message_id, fixture->message_id,
                      RNS_RRC_MESSAGE_ID_SIZE) == 0);
        assert(envelope.timestamp_ms == fixture->timestamp_ms);
        assert(memcmp(envelope.source, fixture->source,
                      RNS_RRC_SOURCE_SIZE) == 0);
        assert_slice(envelope.room, fixture->room, fixture->room_len);
        assert_slice(envelope.body_cbor, fixture->body, fixture->body_len);
        assert_slice(envelope.nick, fixture->nick, fixture->nick_len);

        assert(rns_rrc_envelope_encode(&envelope, encoded, sizeof encoded,
                                       &encoded_len) == RNS_OK);
        assert(encoded_len == fixture->canonical_len);
        assert(memcmp(encoded, fixture->canonical, encoded_len) == 0);
        if (fixture->has_unknown_outer) {
            assert(fixture->wire_len > fixture->canonical_len);
        } else {
            assert(fixture->wire_len == fixture->canonical_len);
            assert(memcmp(fixture->wire, fixture->canonical,
                          fixture->wire_len) == 0);
        }
    }
    assert(seen_types == ((UINT64_C(1) << RNS_RRC_HELLO) |
                          (UINT64_C(1) << RNS_RRC_WELCOME) |
                          (UINT64_C(1) << RNS_RRC_JOIN) |
                          (UINT64_C(1) << RNS_RRC_JOINED) |
                          (UINT64_C(1) << RNS_RRC_PART) |
                          (UINT64_C(1) << RNS_RRC_PARTED) |
                          (UINT64_C(1) << RNS_RRC_MESSAGE) |
                          (UINT64_C(1) << RNS_RRC_NOTICE) |
                          (UINT64_C(1) << RNS_RRC_PING) |
                          (UINT64_C(1) << RNS_RRC_PONG) |
                          (UINT64_C(1) << RNS_RRC_ERROR) |
                          (UINT64_C(1) << RNS_RRC_RESOURCE_ENVELOPE)));
    return 0;
}
