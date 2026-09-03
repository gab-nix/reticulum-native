#include "reticulum/rrc.h"

#include <assert.h>
#include <string.h>

int main(void) {
    static const uint8_t fixture[] = {
        0xa8,0x00,0x01,0x01,0x14,0x02,0x48,0x00,0x01,0x02,0x03,0x04,
        0x05,0x06,0x07,0x03,0x1b,0x00,0x00,0x01,0x8b,0xcf,0xe5,0x68,
        0x7b,0x04,0x50,0x00,0x01,0x02,0x03,0x04,0x05,0x06,0x07,0x08,
        0x09,0x0a,0x0b,0x0c,0x0d,0x0e,0x0f,0x05,0x65,'l','o','b','b',
        'y',0x06,0x65,'h','e','l','l','o',0x07,0x63,'r','e','i'};
    rns_rrc_envelope_t envelope;
    assert(rns_rrc_envelope_parse(fixture, sizeof fixture, &envelope) ==
           RNS_OK);
    assert(envelope.version == 1u && envelope.type == RNS_RRC_MESSAGE);
    assert(envelope.timestamp_ms == UINT64_C(1700000000123));
    assert(envelope.room.length == 5u &&
           memcmp(envelope.room.data, "lobby", 5u) == 0);
    assert(envelope.body_cbor.length == 6u &&
           envelope.body_cbor.data[0] == 0x65u);
    assert(envelope.nick.length == 3u &&
           memcmp(envelope.nick.data, "rei", 3u) == 0);
    uint8_t output[128];
    size_t output_length = 0u;
    assert(rns_rrc_envelope_encode(&envelope, output, sizeof output,
                                   &output_length) == RNS_OK);
    assert(output_length == sizeof fixture &&
           memcmp(output, fixture, output_length) == 0);
    uint8_t text[8];
    assert(rns_rrc_cbor_text((const uint8_t *)"hi", 2u, text, sizeof text,
                             &output_length) == RNS_OK);
    rns_rrc_slice_t parsed;
    assert(rns_rrc_cbor_text_parse(text, output_length, &parsed) == RNS_OK);
    assert(parsed.length == 2u && memcmp(parsed.data, "hi", 2u) == 0);
    static const uint8_t trailing[] = {0x62u, 'h', 'i', 0x00u};
    static const uint8_t noncanonical[] = {0x78u, 0x02u, 'h', 'i'};
    static const uint8_t invalid_utf8[] = {0x61u, 0xffu};
    assert(rns_rrc_cbor_text_parse(trailing, sizeof trailing, &parsed) ==
           RNS_ERROR_PROTOCOL);
    assert(rns_rrc_cbor_text_parse(noncanonical, sizeof noncanonical,
                                   &parsed) == RNS_ERROR_PROTOCOL);
    assert(rns_rrc_cbor_text_parse(invalid_utf8, sizeof invalid_utf8,
                                   &parsed) == RNS_ERROR_PROTOCOL);
    uint8_t bad[sizeof fixture];
    memcpy(bad, fixture, sizeof bad);
    bad[0] = 0xbfu;
    assert(rns_rrc_envelope_parse(bad, sizeof bad, &envelope) ==
           RNS_ERROR_PROTOCOL);
    memcpy(bad, fixture, sizeof bad);
    bad[48] = 0xffu;
    assert(rns_rrc_envelope_parse(bad, sizeof bad, &envelope) ==
           RNS_ERROR_PROTOCOL);
    assert(rns_rrc_envelope_parse(fixture, sizeof fixture - 1u, &envelope) ==
           RNS_ERROR_PROTOCOL);

    static const uint8_t member_list[] = {
        0x82u,0x50u,0x00u,0x01u,0x02u,0x03u,0x04u,0x05u,0x06u,0x07u,
        0x08u,0x09u,0x0au,0x0bu,0x0cu,0x0du,0x0eu,0x0fu,0x50u,0x10u,
        0x11u,0x12u,0x13u,0x14u,0x15u,0x16u,0x17u,0x18u,0x19u,0x1au,
        0x1bu,0x1cu,0x1du,0x1eu,0x1fu};
    uint8_t members[2][RNS_RRC_SOURCE_SIZE];
    size_t member_count = 0u;
    assert(rns_rrc_member_list_parse(member_list, sizeof member_list,
                                     members, 2u, &member_count) == RNS_OK);
    assert(member_count == 2u);
    assert(memcmp(members[0], member_list + 2u, RNS_RRC_SOURCE_SIZE) == 0);
    assert(rns_rrc_member_list_parse(member_list, sizeof member_list,
                                     members, 1u, &member_count) ==
           RNS_ERROR_OVERFLOW);
    static const uint8_t bad_member[] = {
        0x81u,0x4fu,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0};
    assert(rns_rrc_member_list_parse(bad_member, sizeof bad_member, members,
                                     2u, &member_count) ==
           RNS_ERROR_PROTOCOL);
    return 0;
}
