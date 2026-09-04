#include "reticulum/channel.h"

#include <assert.h>
#include <string.h>

typedef struct fixture {
    double now;
    unsigned sends;
    unsigned receives;
    unsigned retries;
    unsigned timeouts;
    unsigned duplicates;
    unsigned out_of_order;
    uint16_t received_sequence[8];
    char received_payload[8][16];
} fixture_t;

static double clock_now(void *context) { return ((fixture_t *)context)->now; }

static rns_status_t send_wire(const uint8_t *data, size_t length, void *context) {
    fixture_t *fixture = context;
    assert(data != NULL && length >= RNS_CHANNEL_HEADER_BYTES);
    fixture->sends++;
    return RNS_OK;
}

static rns_status_t receive_message(uint16_t type, uint16_t sequence, const uint8_t *payload,
                                    size_t length, void *context) {
    fixture_t *fixture = context;
    assert(type == 0x1234 && fixture->receives < 8 && length < 16);
    fixture->received_sequence[fixture->receives] = sequence;
    memcpy(fixture->received_payload[fixture->receives], payload, length);
    fixture->received_payload[fixture->receives][length] = '\0';
    fixture->receives++;
    return RNS_OK;
}

static void on_event(rns_channel_event_t event, uint16_t sequence, void *context) {
    fixture_t *fixture = context;
    (void)sequence;
    if (event == RNS_CHANNEL_EVENT_RETRY) fixture->retries++;
    if (event == RNS_CHANNEL_EVENT_TIMEOUT) fixture->timeouts++;
    if (event == RNS_CHANNEL_EVENT_DUPLICATE) fixture->duplicates++;
    if (event == RNS_CHANNEL_EVENT_OUT_OF_ORDER) fixture->out_of_order++;
}

static rns_channel_config_t config(fixture_t *fixture) {
    rns_channel_config_t result = {2, 4, 2, 5.0, clock_now, fixture, send_wire, fixture,
                                   receive_message, fixture, on_event, fixture};
    return result;
}

static void test_envelope(void) {
    uint8_t wire[32];
    const uint8_t *payload;
    size_t wire_length, payload_length;
    uint16_t type, sequence;
    assert(rns_channel_envelope_encode(0x1234, 0xabcd, (const uint8_t *)"hi", 2, wire,
                                       sizeof(wire), &wire_length) == RNS_OK);
    assert(wire_length == 8);
    assert(memcmp(wire, "\x12\x34\xab\xcd\x00\x02hi", 8) == 0);
    assert(rns_channel_envelope_decode(wire, wire_length, &type, &sequence, &payload,
                                       &payload_length) == RNS_OK);
    assert(type == 0x1234 && sequence == 0xabcd && payload_length == 2);
    wire[5] = 3;
    assert(rns_channel_envelope_decode(wire, wire_length, &type, &sequence, &payload,
                                       &payload_length) == RNS_ERROR_PROTOCOL);
}

static void test_send_retry_window(void) {
    fixture_t fixture = {0};
    rns_channel_t channel;
    rns_channel_config_t cfg = config(&fixture);
    uint16_t s0, s1, ignored;
    assert(rns_channel_init(&channel, &cfg) == RNS_OK);
    assert(rns_channel_send(&channel, 1, (const uint8_t *)"a", 1, &s0) == RNS_OK);
    assert(rns_channel_send(&channel, 1, (const uint8_t *)"b", 1, &s1) == RNS_OK);
    assert(s0 == 0 && s1 == 1 && fixture.sends == 2);
    assert(rns_channel_send(&channel, 1, NULL, 0, &ignored) == RNS_ERROR_OVERFLOW);
    assert(rns_channel_mark_delivered(&channel, s0) == RNS_OK);
    assert(rns_channel_mark_delivered(&channel, s1) == RNS_OK);
    assert(channel.window == 3 && channel.outstanding == 0);

    assert(rns_channel_send(&channel, 1, NULL, 0, &ignored) == RNS_OK);
    fixture.now = 5;
    assert(rns_channel_tick(&channel) == 1 && fixture.retries == 1);
    fixture.now = 10;
    assert(rns_channel_tick(&channel) == 1 && fixture.retries == 2);
    fixture.now = 15;
    assert(rns_channel_tick(&channel) == 1 && fixture.timeouts == 1);
    assert(channel.outstanding == 0 && channel.window == 1);
}

static void test_order_duplicates_and_wrap(void) {
    fixture_t fixture = {0};
    rns_channel_t channel;
    rns_channel_config_t cfg = config(&fixture);
    uint8_t zero[32], one[32];
    size_t zero_length, one_length;
    assert(rns_channel_init(&channel, &cfg) == RNS_OK);
    assert(rns_channel_envelope_encode(0x1234, 1, (const uint8_t *)"one", 3, one,
                                       sizeof(one), &one_length) == RNS_OK);
    assert(rns_channel_envelope_encode(0x1234, 0, (const uint8_t *)"zero", 4, zero,
                                       sizeof(zero), &zero_length) == RNS_OK);
    assert(rns_channel_receive(&channel, one, one_length) == RNS_OK);
    assert(fixture.receives == 0 && fixture.out_of_order == 1);
    assert(rns_channel_receive(&channel, one, one_length) == RNS_OK);
    assert(fixture.duplicates == 1);
    assert(rns_channel_receive(&channel, zero, zero_length) == RNS_OK);
    assert(fixture.receives == 2);
    assert(fixture.received_sequence[0] == 0 && fixture.received_sequence[1] == 1);
    assert(strcmp(fixture.received_payload[0], "zero") == 0);
    assert(rns_channel_receive(&channel, zero, zero_length) == RNS_OK);
    assert(fixture.duplicates == 2);

    channel.expected_sequence = UINT16_MAX;
    assert(rns_channel_envelope_encode(0x1234, UINT16_MAX, (const uint8_t *)"last", 4,
                                       zero, sizeof(zero), &zero_length) == RNS_OK);
    assert(rns_channel_envelope_encode(0x1234, 0, (const uint8_t *)"wrap", 4, one,
                                       sizeof(one), &one_length) == RNS_OK);
    assert(rns_channel_receive(&channel, one, one_length) == RNS_OK);
    assert(rns_channel_receive(&channel, zero, zero_length) == RNS_OK);
    assert(channel.expected_sequence == 1 && fixture.receives == 4);
}

int main(void) {
    test_envelope();
    test_send_retry_window();
    test_order_duplicates_and_wrap();
    return 0;
}
