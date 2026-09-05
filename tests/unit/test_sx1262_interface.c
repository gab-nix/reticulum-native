/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "reticulum/sx1262_interface.h"

#include <assert.h>
#include <stdint.h>
#include <string.h>

enum { FAKE_EVENT_CAPACITY = 64, FAKE_TX_CAPACITY = 16 };

typedef struct fake_clock {
    uint64_t now_ms;
    uint8_t entropy[128];
    size_t entropy_length;
    size_t entropy_offset;
    bool fail_entropy;
} fake_clock_t;

typedef struct fake_phy {
    rns_sx1262_phy_event_t events[FAKE_EVENT_CAPACITY];
    uint8_t event_frames[FAKE_EVENT_CAPACITY][RNS_RADIO_PHY_MTU];
    size_t event_head;
    size_t event_count;
    uint8_t transmitted[FAKE_TX_CAPACITY][RNS_RADIO_PHY_MTU];
    size_t transmitted_lengths[FAKE_TX_CAPACITY];
    uint32_t transmitted_tokens[FAKE_TX_CAPACITY];
    uint32_t cad_tokens[FAKE_TX_CAPACITY];
    rns_sx1262_scheduler_config_t last_config;
    size_t tx_count;
    size_t cad_count;
    size_t start_count;
    size_t stop_count;
    size_t cancel_count;
    size_t fail_transmit_number;
    bool fail_start;
    size_t fail_start_number;
    bool fail_cad;
    bool fail_poll;
    bool fail_cancel;
    uint32_t active_token;
} fake_phy_t;

typedef struct result_capture {
    uint32_t ids[16];
    rns_sx1262_packet_outcome_t outcomes[16];
    rns_status_t statuses[16];
    size_t count;
    rns_sx1262_interface_t *reentrant_interface;
    uint8_t reentrant_packet[2];
    uint32_t reentrant_id;
} result_capture_t;

typedef struct receive_capture {
    uint8_t packet[RNS_RADIO_PACKET_MTU];
    size_t length;
    size_t count;
    rns_status_t status;
    rns_sx1262_interface_t *reentrant_interface;
    uint8_t reentrant_packet[3];
    uint32_t reentrant_id;
} receive_capture_t;

static uint64_t fake_now(void *context) {
    return ((fake_clock_t *)context)->now_ms;
}

static rns_status_t fake_entropy(void *context, uint8_t *output,
                                 size_t length) {
    fake_clock_t *clock = context;
    size_t index;
    if (clock->fail_entropy) {
        return RNS_ERROR_IO;
    }
    for (index = 0U; index < length; ++index) {
        output[index] = clock->entropy_length == 0U
                            ? 0U
                            : clock->entropy[clock->entropy_offset++ %
                                             clock->entropy_length];
    }
    return RNS_OK;
}

static rns_status_t fake_start(
    void *context, const rns_sx1262_scheduler_config_t *config) {
    fake_phy_t *phy = context;
    phy->start_count++;
    phy->last_config = *config;
    return phy->fail_start || phy->fail_start_number == phy->start_count
               ? RNS_ERROR_IO
               : RNS_OK;
}

static rns_status_t fake_poll_event(void *context,
                                    rns_sx1262_phy_event_t *event) {
    fake_phy_t *phy = context;
    if (phy->fail_poll) {
        phy->fail_poll = false;
        return RNS_ERROR_IO;
    }
    if (phy->event_count == 0U) {
        return RNS_ERROR_NOT_FOUND;
    }
    for (size_t i=0U; i<phy->event_count; i++) {
        size_t slot=(phy->event_head+i)%FAKE_EVENT_CAPACITY;
        if (phy->events[slot].type != RNS_SX1262_PHY_EVENT_RX_FRAME &&
            phy->events[slot].operation_token == phy->active_token) {
            rns_sx1262_phy_event_t first=phy->events[phy->event_head];
            phy->events[phy->event_head]=phy->events[slot];
            phy->events[slot]=first;
            break;
        }
    }
    *event = phy->events[phy->event_head];
    phy->event_head = (phy->event_head + 1U) % FAKE_EVENT_CAPACITY;
    phy->event_count--;
    return RNS_OK;
}

static rns_status_t fake_start_cad(
    void *context, const rns_sx1262_scheduler_config_t *config,
    uint32_t token) {
    fake_phy_t *phy = context;
    assert(config->bandwidth_hz == phy->last_config.bandwidth_hz);
    phy->cad_tokens[phy->cad_count++] = token;
    phy->active_token = token;
    return phy->fail_cad ? RNS_ERROR_IO : RNS_OK;
}

static rns_status_t fake_transmit(
    void *context, const rns_sx1262_scheduler_config_t *config,
    const uint8_t *frame, size_t frame_length, uint32_t token) {
    fake_phy_t *phy = context;
    size_t index = phy->tx_count++;
    assert(config->spreading_factor == phy->last_config.spreading_factor);
    assert(index < FAKE_TX_CAPACITY);
    if (phy->fail_transmit_number == phy->tx_count) {
        return RNS_ERROR_IO;
    }
    memcpy(phy->transmitted[index], frame, frame_length);
    phy->transmitted_lengths[index] = frame_length;
    phy->transmitted_tokens[index] = token;
    phy->active_token = token;
    return RNS_OK;
}

static rns_status_t fake_stop(void *context) {
    ((fake_phy_t *)context)->stop_count++;
    return RNS_OK;
}

static rns_status_t fake_cancel(
    void *context, const rns_sx1262_scheduler_config_t *config,
    uint32_t token) {
    fake_phy_t *phy = context;
    assert(config->bandwidth_hz == phy->last_config.bandwidth_hz);
    assert(token != 0U);
    phy->cancel_count++;
    return phy->fail_cancel ? RNS_ERROR_IO : RNS_OK;
}

static const rns_sx1262_phy_ops_t PHY_OPS = {
    .start = fake_start,
    .poll_event = fake_poll_event,
    .start_cad = fake_start_cad,
    .transmit = fake_transmit,
    .cancel_operation = fake_cancel,
    .stop = fake_stop};

static const rns_sx1262_clock_ops_t CLOCK_OPS = {
    .monotonic_ms = fake_now, .entropy = fake_entropy};

static void push_control(fake_phy_t *phy, rns_sx1262_phy_event_type_t type,
                         uint32_t token, rns_status_t status) {
    size_t tail = (phy->event_head + phy->event_count) % FAKE_EVENT_CAPACITY;
    assert(phy->event_count < FAKE_EVENT_CAPACITY);
    phy->events[tail] = (rns_sx1262_phy_event_t){
        .type = type, .operation_token = token, .status = status};
    phy->event_count++;
}

static void push_frame(fake_phy_t *phy, const uint8_t *frame,
                       size_t frame_length) {
    size_t tail = (phy->event_head + phy->event_count) % FAKE_EVENT_CAPACITY;
    assert(phy->event_count < FAKE_EVENT_CAPACITY);
    assert(frame_length <= RNS_RADIO_PHY_MTU);
    memcpy(phy->event_frames[tail], frame, frame_length);
    phy->events[tail] = (rns_sx1262_phy_event_t){
        .type = RNS_SX1262_PHY_EVENT_RX_FRAME,
        .rssi_dbm = -112, .snr_db = -7,
        .frame = phy->event_frames[tail],
        .frame_length = frame_length};
    phy->event_count++;
}

static void capture_result(void *context, uint32_t packet_id,
                           rns_sx1262_packet_outcome_t outcome,
                           rns_status_t status) {
    result_capture_t *capture = context;
    assert(capture->count < 16U);
    capture->ids[capture->count] = packet_id;
    capture->outcomes[capture->count] = outcome;
    capture->statuses[capture->count] = status;
    capture->count++;
    if (capture->reentrant_interface != NULL) {
        rns_sx1262_interface_t *interface_value =
            capture->reentrant_interface;
        capture->reentrant_interface = NULL;
        assert(rns_sx1262_interface_send(
                   interface_value, capture->reentrant_packet,
                   sizeof(capture->reentrant_packet),
                   &capture->reentrant_id) == RNS_OK);
    }
}

static rns_status_t capture_receive(void *context, const uint8_t *packet,
                                    size_t packet_length) {
    receive_capture_t *capture = context;
    assert(packet_length <= sizeof(capture->packet));
    memcpy(capture->packet, packet, packet_length);
    capture->length = packet_length;
    capture->count++;
    if (capture->reentrant_interface != NULL) {
        rns_sx1262_interface_t *interface_value =
            capture->reentrant_interface;
        capture->reentrant_interface = NULL;
        assert(rns_sx1262_interface_send(
                   interface_value, capture->reentrant_packet,
                   sizeof(capture->reentrant_packet),
                   &capture->reentrant_id) == RNS_OK);
    }
    return capture->status;
}

static void fill_packet(uint8_t *packet, size_t length, uint8_t salt) {
    size_t index;
    for (index = 0U; index < length; ++index) {
        packet[index] = (uint8_t)(index * 29U + salt);
    }
}

static rns_sx1262_interface_t *create_started(
    fake_phy_t *phy, fake_clock_t *clock, result_capture_t *results,
    rns_sx1262_scheduler_config_t *config) {
    rns_sx1262_interface_t *interface_value = NULL;
    rns_sx1262_scheduler_default_config(config);
    config->difs_ms = 0U;
    config->contention_window_ms = 0U;
    config->cad_busy_backoff_ms = 0U;
    assert(rns_sx1262_interface_create(
               config, &PHY_OPS, phy, &CLOCK_OPS, clock, capture_result,
               results, &interface_value) == RNS_OK);
    assert(rns_sx1262_interface_start(interface_value) == RNS_OK);
    return interface_value;
}

static void reach_cad(rns_sx1262_interface_t *interface_value,
                      receive_capture_t *receive) {
    assert(rns_sx1262_interface_poll(interface_value, capture_receive, receive,
                                     1U) == RNS_OK);
    assert(rns_sx1262_interface_poll(interface_value, capture_receive, receive,
                                     1U) == RNS_OK);
}

static void complete_packet(rns_sx1262_interface_t *interface_value,
                            fake_phy_t *phy, receive_capture_t *receive);

static void test_airtime_vectors(void) {
    static const uint8_t spreading_factors[] = {5U, 6U, 8U, 12U};
    static const uint64_t expected[][4] = {
        {10816U, 10816U, 136256U, 140096U},
        {19072U, 21632U, 228992U, 236672U},
        {72192U, 82432U, 707072U, 727552U},
        {1155072U, 1155072U, 9183232U, 9347072U}};
    static const size_t lengths[] = {1U, 3U, 247U, 255U};
    static const uint64_t whole_500[] = {276352U, 465664U, 1434624U,
                                         18530304U};
    rns_sx1262_scheduler_config_t config;
    size_t sf_index;
    rns_sx1262_scheduler_default_config(&config);
    for (sf_index = 0U; sf_index < 4U; ++sf_index) {
        size_t length_index;
        uint64_t first;
        uint64_t second;
        config.spreading_factor = spreading_factors[sf_index];
        for (length_index = 0U; length_index < 4U; ++length_index) {
            uint64_t actual = 0U;
            assert(rns_sx1262_airtime_us(&config, lengths[length_index],
                                         &actual) == RNS_OK);
            assert(actual == expected[sf_index][length_index]);
        }
        assert(rns_sx1262_airtime_us(&config, 255U, &first) == RNS_OK);
        assert(rns_sx1262_airtime_us(&config, 247U, &second) == RNS_OK);
        assert(first + second == whole_500[sf_index]);
    }
    config.spreading_factor = 5U;
    config.preamble_symbols = 11U;
    {
        uint64_t ignored;
        assert(rns_sx1262_airtime_us(&config, 1U, &ignored) ==
               RNS_ERROR_INVALID_ARGUMENT);
    }
    config.spreading_factor = 8U;
    config.preamble_symbols = 18U;
    config.bandwidth_hz = 41700U;
    {
        uint64_t actual = 0U;
        assert(rns_sx1262_airtime_us(&config, 255U, &actual) == RNS_OK);
        assert(actual == 2182639U);
    }
}

static void test_split_sequence_guard(void) {
    fake_phy_t phy = {0};
    fake_clock_t clock = {.entropy = {2U}, .entropy_length = 1U};
    result_capture_t results = {0};
    receive_capture_t receive = {0};
    rns_sx1262_scheduler_config_t config;
    rns_sx1262_interface_t *interface_value =
        create_started(&phy, &clock, &results, &config);
    uint8_t packet[300U] = {0};
    uint32_t id;
    assert(rns_sx1262_interface_send(interface_value, packet, sizeof(packet),
                                     &id) == RNS_OK);
    complete_packet(interface_value, &phy, &receive);
    assert((phy.transmitted[0][0] >> 4U) == 2U);
    assert(rns_sx1262_interface_send(interface_value, packet, sizeof(packet),
                                     &id) == RNS_OK);
    complete_packet(interface_value, &phy, &receive);
    assert((phy.transmitted[2][0] >> 4U) == 3U);
    rns_sx1262_interface_destroy(interface_value);
}

static void test_one_and_two_frame_success(void) {
    fake_phy_t phy = {0};
    fake_clock_t clock = {.entropy = {3U}, .entropy_length = 1U};
    result_capture_t results = {0};
    receive_capture_t receive = {0};
    rns_sx1262_scheduler_config_t config;
    rns_sx1262_interface_t *interface_value =
        create_started(&phy, &clock, &results, &config);
    uint8_t packet[500U];
    uint32_t id1;
    uint32_t id2;
    fill_packet(packet, sizeof(packet), 7U);
    assert(rns_sx1262_interface_send(interface_value, packet, 10U, &id1) ==
           RNS_OK);
    reach_cad(interface_value, &receive);
    assert(phy.cad_count == 1U);
    push_control(&phy, RNS_SX1262_PHY_EVENT_CAD_CLEAR, phy.cad_tokens[0],
                 RNS_OK);
    assert(rns_sx1262_interface_poll(interface_value, capture_receive, &receive,
                                     1U) == RNS_OK);
    assert(phy.tx_count == 1U && phy.transmitted_lengths[0] == 11U);
    rns_radio_encoded_packet_t queued_rx;
    assert(rns_radio_frame_encode(packet,10U,9U,&queued_rx)==RNS_OK);
    push_frame(&phy,queued_rx.frames[0],queued_rx.lengths[0]);
    push_control(&phy,RNS_SX1262_PHY_EVENT_TX_DONE,0U,RNS_OK);
    push_control(&phy, RNS_SX1262_PHY_EVENT_TX_DONE,
                 phy.transmitted_tokens[0], RNS_OK);
    clock.now_ms += 60000U; /* Completion already queued before delayed poll. */
    assert(rns_sx1262_interface_poll(interface_value, capture_receive, &receive,
                                     0U) == RNS_OK);
    assert(phy.event_count == 3U && results.count == 0U);
    assert(rns_sx1262_interface_poll(interface_value, capture_receive, &receive,
                                     1U) == RNS_OK);
    assert(results.count == 1U && results.ids[0] == id1 &&
           results.outcomes[0] == RNS_SX1262_PACKET_SENT);
    assert(rns_sx1262_interface_poll(interface_value,capture_receive,&receive,4U)==RNS_ERROR_PROTOCOL);
    assert(phy.event_count==0U);

    assert(rns_sx1262_interface_send(interface_value, packet, sizeof(packet),
                                     &id2) == RNS_OK);
    reach_cad(interface_value, &receive);
    push_control(&phy, RNS_SX1262_PHY_EVENT_CAD_CLEAR, phy.cad_tokens[1],
                 RNS_OK);
    assert(rns_sx1262_interface_poll(interface_value, capture_receive, &receive,
                                     1U) == RNS_OK);
    assert(phy.transmitted_lengths[1] == 255U);
    push_control(&phy, RNS_SX1262_PHY_EVENT_TX_DONE,
                 phy.transmitted_tokens[1], RNS_OK);
    assert(rns_sx1262_interface_poll(interface_value, capture_receive, &receive,
                                     1U) == RNS_OK);
    assert(phy.tx_count == 3U && phy.transmitted_lengths[2] == 247U &&
           phy.cad_count == 2U);
    push_control(&phy, RNS_SX1262_PHY_EVENT_TX_DONE,
                 phy.transmitted_tokens[2], RNS_OK);
    assert(rns_sx1262_interface_poll(interface_value, capture_receive, &receive,
                                     1U) == RNS_OK);
    assert(results.count == 2U && results.ids[1] == id2 &&
           results.outcomes[1] == RNS_SX1262_PACKET_SENT);
    rns_sx1262_interface_destroy(interface_value);
}

static void test_entropy_queue_and_failures(void) {
    fake_phy_t phy = {0};
    fake_clock_t clock = {.entropy = {1U}, .entropy_length = 1U};
    result_capture_t results = {0};
    receive_capture_t receive = {0};
    rns_sx1262_scheduler_config_t config;
    rns_sx1262_interface_t *interface_value =
        create_started(&phy, &clock, &results, &config);
    uint8_t packet[300U] = {0};
    uint32_t ids[5];
    size_t index;
    clock.fail_entropy = true;
    assert(rns_sx1262_interface_send(interface_value, packet, sizeof(packet),
                                     &ids[0]) == RNS_ERROR_IO);
    clock.fail_entropy = false;
    for (index = 0U; index < RNS_SX1262_PACKET_QUEUE_CAPACITY; ++index) {
        assert(rns_sx1262_interface_send(interface_value, packet,
                                         sizeof(packet), &ids[index]) ==
               RNS_OK);
    }
    assert(rns_sx1262_interface_send(interface_value, packet, sizeof(packet),
                                     &ids[4]) == RNS_ERROR_OVERFLOW);
    assert((phy.tx_count == 0U));
    rns_sx1262_interface_stop(interface_value);
    assert(results.count == 4U);
    for (index = 0U; index < results.count; ++index) {
        assert(results.outcomes[index] == RNS_SX1262_PACKET_DROPPED_STOPPED);
    }
    rns_sx1262_interface_destroy(interface_value);

    memset(&phy, 0, sizeof(phy));
    memset(&clock, 0, sizeof(clock));
    memset(&results, 0, sizeof(results));
    interface_value = create_started(&phy, &clock, &results, &config);
    phy.fail_transmit_number = 1U;
    assert(rns_sx1262_interface_send(interface_value, packet, sizeof(packet),
                                     &ids[0]) == RNS_OK);
    reach_cad(interface_value, &receive);
    push_control(&phy, RNS_SX1262_PHY_EVENT_CAD_CLEAR, phy.cad_tokens[0],
                 RNS_OK);
    assert(rns_sx1262_interface_poll(interface_value, capture_receive, &receive,
                                     1U) == RNS_ERROR_IO);
    assert(results.count == 1U &&
           results.outcomes[0] == RNS_SX1262_PACKET_DROPPED_PHY &&
           phy.tx_count == 1U);
    rns_sx1262_interface_destroy(interface_value);

    memset(&phy, 0, sizeof(phy));
    memset(&clock, 0, sizeof(clock));
    memset(&results, 0, sizeof(results));
    interface_value = create_started(&phy, &clock, &results, &config);
    phy.fail_transmit_number = 2U;
    assert(rns_sx1262_interface_send(interface_value, packet, sizeof(packet),
                                     &ids[0]) == RNS_OK);
    reach_cad(interface_value, &receive);
    push_control(&phy, RNS_SX1262_PHY_EVENT_CAD_CLEAR, phy.cad_tokens[0],
                 RNS_OK);
    assert(rns_sx1262_interface_poll(interface_value, capture_receive, &receive,
                                     1U) == RNS_OK);
    push_control(&phy, RNS_SX1262_PHY_EVENT_TX_DONE,
                 phy.transmitted_tokens[0], RNS_OK);
    assert(rns_sx1262_interface_poll(interface_value, capture_receive, &receive,
                                     1U) == RNS_ERROR_IO);
    assert(phy.tx_count == 2U && results.count == 1U &&
           results.outcomes[0] == RNS_SX1262_PACKET_DROPPED_PHY);
    assert(rns_sx1262_interface_poll(interface_value, capture_receive, &receive,
                                     1U) == RNS_OK &&
           phy.tx_count == 2U);
    rns_sx1262_interface_destroy(interface_value);
}

static void test_result_callback_reentrancy_and_poll_failure(void) {
    fake_phy_t phy = {0};
    fake_clock_t clock = {.entropy = {6U}, .entropy_length = 1U};
    result_capture_t results = {0};
    receive_capture_t receive = {0};
    rns_sx1262_scheduler_config_t config;
    rns_sx1262_interface_t *interface_value =
        create_started(&phy, &clock, &results, &config);
    uint8_t packet[] = {9U};
    uint32_t id;
    assert(rns_sx1262_interface_send(interface_value, packet, sizeof(packet),
                                     &id) == RNS_OK);
    complete_packet(interface_value, &phy, &receive);
    results.reentrant_interface = interface_value;
    results.reentrant_packet[0] = 1U;
    results.reentrant_packet[1] = 2U;
    /* Complete a second packet and enqueue a third from its result callback. */
    assert(rns_sx1262_interface_send(interface_value, packet, sizeof(packet),
                                     &id) == RNS_OK);
    complete_packet(interface_value, &phy, &receive);
    assert(results.reentrant_id != 0U);
    {
        rns_sx1262_scheduler_stats_t stats;
        assert(rns_sx1262_interface_get_stats(interface_value, &stats) ==
               RNS_OK);
        assert(stats.pending_packets == 1U);
    }
    phy.fail_poll = true;
    assert(rns_sx1262_interface_poll(interface_value, capture_receive, &receive,
                                     1U) == RNS_ERROR_IO);
    assert(results.count == 3U &&
           results.outcomes[2] == RNS_SX1262_PACKET_DROPPED_PHY);
    rns_sx1262_interface_destroy(interface_value);
}

static void test_cad_busy_tokens_and_contention_entropy(void) {
    fake_phy_t phy = {0};
    fake_clock_t clock = {.entropy = {2U}, .entropy_length = 1U};
    result_capture_t results = {0};
    receive_capture_t receive = {0};
    rns_sx1262_scheduler_config_t config;
    rns_sx1262_interface_t *interface_value =
        create_started(&phy, &clock, &results, &config);
    uint8_t packet[20U] = {0};
    uint32_t id;
    rns_sx1262_interface_destroy(interface_value);
    config.max_cad_attempts = 2U;
    assert(rns_sx1262_interface_create(
               &config, &PHY_OPS, &phy, &CLOCK_OPS, &clock, capture_result,
               &results, &interface_value) == RNS_OK);
    assert(rns_sx1262_interface_start(interface_value) == RNS_OK);
    assert(rns_sx1262_interface_send(interface_value, packet, sizeof(packet),
                                     &id) == RNS_OK);
    reach_cad(interface_value, &receive);
    push_control(&phy, RNS_SX1262_PHY_EVENT_CAD_CLEAR,
                 phy.cad_tokens[0] + 77U, RNS_OK);
    assert(rns_sx1262_interface_poll(interface_value, capture_receive, &receive,
                                     1U) == RNS_ERROR_PROTOCOL);
    assert(phy.tx_count == 0U);
    push_control(&phy, RNS_SX1262_PHY_EVENT_CAD_BUSY, phy.cad_tokens[0],
                 RNS_OK);
    assert(rns_sx1262_interface_poll(interface_value, capture_receive, &receive,
                                     1U) == RNS_OK);
    assert(rns_sx1262_interface_poll(interface_value, capture_receive, &receive,
                                     1U) == RNS_OK);
    push_control(&phy, RNS_SX1262_PHY_EVENT_CAD_BUSY, phy.cad_tokens[1],
                 RNS_OK);
    assert(rns_sx1262_interface_poll(interface_value, capture_receive, &receive,
                                     1U) == RNS_OK);
    assert(results.count == 1U && results.ids[0] == id &&
           results.outcomes[0] == RNS_SX1262_PACKET_DROPPED_CONTENTION &&
           phy.tx_count == 0U);
    rns_sx1262_interface_destroy(interface_value);

    memset(&phy, 0, sizeof(phy));
    memset(&clock, 0, sizeof(clock));
    memset(&results, 0, sizeof(results));
    interface_value = create_started(&phy, &clock, &results, &config);
    config.contention_window_ms = 10U;
    rns_sx1262_interface_destroy(interface_value);
    assert(rns_sx1262_interface_create(
               &config, &PHY_OPS, &phy, &CLOCK_OPS, &clock, capture_result,
               &results, &interface_value) == RNS_OK);
    assert(rns_sx1262_interface_start(interface_value) == RNS_OK);
    /* The accepted packet consumed sequence entropy; delay entropy now fails. */
    assert(rns_sx1262_interface_send(interface_value, packet, sizeof(packet),
                                     &id) == RNS_OK);
    clock.fail_entropy = true;
    assert(rns_sx1262_interface_poll(interface_value, capture_receive, &receive,
                                     1U) == RNS_ERROR_IO);
    assert(results.count == 1U &&
           results.outcomes[0] == RNS_SX1262_PACKET_DROPPED_PHY);
    rns_sx1262_interface_destroy(interface_value);
}

static void test_never_fit_and_operation_timeout(void) {
    fake_phy_t phy = {0};
    fake_clock_t clock = {.entropy = {0U}, .entropy_length = 1U};
    result_capture_t results = {0};
    receive_capture_t receive = {0};
    rns_sx1262_scheduler_config_t config;
    rns_sx1262_interface_t *interface_value =
        create_started(&phy, &clock, &results, &config);
    uint8_t packet[10U] = {0};
    uint32_t id = 0U;
    rns_sx1262_interface_destroy(interface_value);
    config.duty_cycle_ppm = 1U;
    assert(rns_sx1262_interface_create(
               &config, &PHY_OPS, &phy, &CLOCK_OPS, &clock, capture_result,
               &results, &interface_value) == RNS_OK);
    assert(rns_sx1262_interface_start(interface_value) == RNS_OK);
    assert(rns_sx1262_interface_send(interface_value, packet, sizeof(packet),
                                     &id) == RNS_ERROR_OVERFLOW &&
           id == 0U && results.count == 0U);
    rns_sx1262_interface_destroy(interface_value);

    memset(&phy, 0, sizeof(phy));
    memset(&results, 0, sizeof(results));
    clock.now_ms = 0U;
    interface_value = create_started(&phy, &clock, &results, &config);
    config.cad_timeout_ms = 5U;
    rns_sx1262_interface_destroy(interface_value);
    assert(rns_sx1262_interface_create(
               &config, &PHY_OPS, &phy, &CLOCK_OPS, &clock, capture_result,
               &results, &interface_value) == RNS_OK);
    assert(rns_sx1262_interface_start(interface_value) == RNS_OK);
    assert(rns_sx1262_interface_send(interface_value, packet, sizeof(packet),
                                     &id) == RNS_OK);
    reach_cad(interface_value, &receive);
    clock.now_ms = 5U;
    rns_radio_encoded_packet_t inbound;
    assert(rns_radio_frame_encode(packet,sizeof packet,9U,&inbound)==RNS_OK);
    for(size_t i=0;i<4U;i++) push_frame(&phy,inbound.frames[0],inbound.lengths[0]);
    assert(rns_sx1262_interface_poll(interface_value, capture_receive, &receive,
                                     1U) == RNS_ERROR_TIMEOUT);
    assert(receive.count == 1U && phy.event_count == 3U);
    assert(results.count == 1U &&
           results.outcomes[0] == RNS_SX1262_PACKET_DROPPED_PHY &&
           results.statuses[0] == RNS_ERROR_TIMEOUT);
    assert(phy.cancel_count == 1U);
    rns_sx1262_interface_destroy(interface_value);

    memset(&phy, 0, sizeof(phy));
    memset(&results, 0, sizeof(results));
    clock.now_ms = 0U;
    interface_value = create_started(&phy, &clock, &results, &config);
    config.cad_timeout_ms = 5U;
    rns_sx1262_interface_destroy(interface_value);
    phy.fail_cancel = true;
    phy.fail_start_number = 3U;
    assert(rns_sx1262_interface_create(
               &config, &PHY_OPS, &phy, &CLOCK_OPS, &clock, capture_result,
               &results, &interface_value) == RNS_OK);
    assert(rns_sx1262_interface_start(interface_value) == RNS_OK);
    assert(rns_sx1262_interface_send(interface_value, packet, sizeof(packet),
                                     &id) == RNS_OK);
    assert(rns_sx1262_interface_send(interface_value, packet, sizeof(packet),
                                     &id) == RNS_OK);
    reach_cad(interface_value, &receive);
    clock.now_ms = 5U;
    assert(rns_sx1262_interface_poll(interface_value, capture_receive, &receive,
                                     1U) == RNS_ERROR_IO);
    assert(results.count == 2U &&
           results.outcomes[0] == RNS_SX1262_PACKET_DROPPED_PHY &&
           results.outcomes[1] == RNS_SX1262_PACKET_DROPPED_PHY &&
           phy.cancel_count == 1U && phy.stop_count == 2U &&
           phy.start_count == 3U);
    rns_sx1262_interface_destroy(interface_value);
    assert(results.count == 2U);
}

static void test_terminal_irq_failures(void) {
    fake_phy_t phy = {0};
    fake_clock_t clock = {.entropy = {12U}, .entropy_length = 1U};
    result_capture_t results = {0};
    receive_capture_t receive = {0};
    rns_sx1262_scheduler_config_t config;
    rns_sx1262_interface_t *interface_value =
        create_started(&phy, &clock, &results, &config);
    uint8_t packet[300U] = {0};
    uint32_t id;
    assert(rns_sx1262_interface_send(interface_value, packet, sizeof(packet),
                                     &id) == RNS_OK);
    reach_cad(interface_value, &receive);
    push_control(&phy, RNS_SX1262_PHY_EVENT_CAD_CLEAR, phy.cad_tokens[0],
                 RNS_OK);
    assert(rns_sx1262_interface_poll(interface_value, capture_receive, &receive,
                                     1U) == RNS_OK);
    push_control(&phy, RNS_SX1262_PHY_EVENT_TX_FAILED,
                 phy.transmitted_tokens[0], RNS_ERROR_IO);
    assert(rns_sx1262_interface_poll(interface_value, capture_receive, &receive,
                                     1U) == RNS_ERROR_IO);
    assert(results.count == 1U &&
           results.outcomes[0] == RNS_SX1262_PACKET_DROPPED_PHY &&
           phy.tx_count == 1U);

    assert(rns_sx1262_interface_send(interface_value, packet, sizeof(packet),
                                     &id) == RNS_OK);
    reach_cad(interface_value, &receive);
    push_control(&phy, RNS_SX1262_PHY_EVENT_CAD_CLEAR, phy.cad_tokens[1],
                 RNS_OK);
    assert(rns_sx1262_interface_poll(interface_value, capture_receive, &receive,
                                     1U) == RNS_OK);
    push_control(&phy, RNS_SX1262_PHY_EVENT_TX_DONE,
                 phy.transmitted_tokens[1], RNS_OK);
    assert(rns_sx1262_interface_poll(interface_value, capture_receive, &receive,
                                     1U) == RNS_OK);
    push_control(&phy, RNS_SX1262_PHY_EVENT_TX_FAILED,
                 phy.transmitted_tokens[2], RNS_ERROR_TIMEOUT);
    assert(rns_sx1262_interface_poll(interface_value, capture_receive, &receive,
                                     1U) == RNS_ERROR_TIMEOUT);
    assert(results.count == 2U &&
           results.outcomes[1] == RNS_SX1262_PACKET_DROPPED_PHY &&
           phy.tx_count == 3U);
    rns_sx1262_interface_destroy(interface_value);
}

static void complete_packet(rns_sx1262_interface_t *interface_value,
                            fake_phy_t *phy, receive_capture_t *receive) {
    const size_t cad_index = phy->cad_count;
    const size_t tx_index = phy->tx_count;
    reach_cad(interface_value, receive);
    push_control(phy, RNS_SX1262_PHY_EVENT_CAD_CLEAR,
                 phy->cad_tokens[cad_index], RNS_OK);
    assert(rns_sx1262_interface_poll(interface_value, capture_receive, receive,
                                     1U) == RNS_OK);
    push_control(phy, RNS_SX1262_PHY_EVENT_TX_DONE,
                 phy->transmitted_tokens[tx_index], RNS_OK);
    assert(rns_sx1262_interface_poll(interface_value, capture_receive, receive,
                                     1U) == RNS_OK);
    if (phy->tx_count == tx_index + 2U) {
        push_control(phy, RNS_SX1262_PHY_EVENT_TX_DONE,
                     phy->transmitted_tokens[tx_index + 1U], RNS_OK);
        assert(rns_sx1262_interface_poll(interface_value, capture_receive,
                                         receive, 1U) == RNS_OK);
    }
}

static void test_duty_boundaries_and_clock_rollback(void) {
    fake_phy_t phy = {0};
    fake_clock_t clock = {.now_ms = 59999U,
                          .entropy = {0U},
                          .entropy_length = 1U};
    result_capture_t results = {0};
    receive_capture_t receive = {0};
    rns_sx1262_scheduler_config_t config;
    rns_sx1262_interface_t *interface_value =
        create_started(&phy, &clock, &results, &config);
    uint8_t packet[500U] = {0};
    uint32_t id;
    rns_sx1262_interface_destroy(interface_value);
    config.spreading_factor = 12U;
    assert(rns_sx1262_interface_create(
               &config, &PHY_OPS, &phy, &CLOCK_OPS, &clock, capture_result,
               &results, &interface_value) == RNS_OK);
    assert(rns_sx1262_interface_start(interface_value) == RNS_OK);
    assert(rns_sx1262_interface_send(interface_value, packet, sizeof(packet),
                                     &id) == RNS_OK);
    complete_packet(interface_value, &phy, &receive);
    assert(results.count == 1U);
    assert(rns_sx1262_interface_send(interface_value, packet, sizeof(packet),
                                     &id) == RNS_OK);
    clock.now_ms = 3600000U;
    assert(rns_sx1262_interface_poll(interface_value, capture_receive, &receive,
                                     1U) == RNS_OK);
    assert(phy.cad_count == 1U);
    clock.now_ms = 3599999U;
    assert(rns_sx1262_interface_poll(interface_value, capture_receive, &receive,
                                     1U) == RNS_OK);
    clock.now_ms = 3659999U;
    assert(rns_sx1262_interface_poll(interface_value, capture_receive, &receive,
                                     1U) == RNS_OK);
    assert(phy.cad_count == 1U);
    clock.now_ms = 3660000U;
    assert(rns_sx1262_interface_poll(interface_value, capture_receive, &receive,
                                     1U) == RNS_OK);
    assert(phy.cad_count == 1U);
    clock.now_ms = 3720000U;
    assert(rns_sx1262_interface_poll(interface_value, capture_receive, &receive,
                                     1U) == RNS_OK);
    assert(rns_sx1262_interface_poll(interface_value, capture_receive, &receive,
                                     1U) == RNS_OK);
    assert(phy.cad_count == 2U);
    {
        rns_sx1262_scheduler_stats_t stats;
        assert(rns_sx1262_interface_get_stats(interface_value, &stats) ==
               RNS_OK);
        assert(stats.clock_regressions >= 1U &&
               stats.airtime_reserved_us == 18530304U);
    }
    rns_sx1262_interface_destroy(interface_value);
}

static void test_rx_reassembly_malformed_overflow_and_reentrancy(void) {
    fake_phy_t phy = {0};
    fake_clock_t clock = {.entropy = {4U}, .entropy_length = 1U};
    result_capture_t results = {0};
    receive_capture_t receive = {.status = RNS_OK};
    rns_sx1262_scheduler_config_t config;
    rns_sx1262_interface_t *interface_value =
        create_started(&phy, &clock, &results, &config);
    rns_radio_encoded_packet_t encoded;
    rns_radio_encoded_packet_t collision;
    uint8_t packet[300U];
    uint8_t malformed[] = {0x02U, 0xaaU};
    fill_packet(packet, sizeof(packet), 33U);
    assert(rns_radio_frame_encode(packet, sizeof(packet), 7U, &encoded) ==
           RNS_OK);
    push_frame(&phy, encoded.frames[1], encoded.lengths[1]);
    assert(rns_sx1262_interface_poll(interface_value, capture_receive, &receive,
                                     1U) == RNS_ERROR_PROTOCOL);
    push_frame(&phy, malformed, sizeof(malformed));
    assert(rns_sx1262_interface_poll(interface_value, capture_receive, &receive,
                                     1U) == RNS_ERROR_PROTOCOL);
    push_frame(&phy, encoded.frames[0], encoded.lengths[0]);
    push_frame(&phy, encoded.frames[1], encoded.lengths[1]);
    assert(rns_sx1262_interface_poll(interface_value, capture_receive, &receive,
                                     2U) == RNS_OK);
    assert(receive.count == 1U && receive.length == sizeof(packet) &&
           memcmp(receive.packet, packet, sizeof(packet)) == 0);

    packet[0] ^= 0xffU;
    assert(rns_radio_frame_encode(packet, sizeof(packet), 10U, &encoded) ==
           RNS_OK);
    packet[1] ^= 0xffU;
    assert(rns_radio_frame_encode(packet, sizeof(packet), 10U, &collision) ==
           RNS_OK);
    push_frame(&phy, encoded.frames[0], encoded.lengths[0]);
    push_frame(&phy, collision.frames[0], collision.lengths[0]);
    assert(rns_sx1262_interface_poll(interface_value, capture_receive, &receive,
                                     2U) == RNS_ERROR_PROTOCOL);
    push_frame(&phy, encoded.frames[1], encoded.lengths[1]);
    assert(rns_sx1262_interface_poll(interface_value, capture_receive, &receive,
                                     1U) == RNS_ERROR_PROTOCOL);

    assert(rns_radio_frame_encode(packet, sizeof(packet), 11U, &encoded) ==
           RNS_OK);
    push_frame(&phy, encoded.frames[0], encoded.lengths[0]);
    assert(rns_sx1262_interface_poll(interface_value, capture_receive, &receive,
                                     1U) == RNS_OK);
    clock.now_ms = config.fragment_timeout_ms;
    assert(rns_sx1262_interface_poll(interface_value, capture_receive, &receive,
                                     1U) == RNS_OK);
    push_frame(&phy, encoded.frames[1], encoded.lengths[1]);
    assert(rns_sx1262_interface_poll(interface_value, capture_receive, &receive,
                                     1U) == RNS_ERROR_PROTOCOL);

    receive.reentrant_interface = interface_value;
    receive.reentrant_packet[0] = 1U;
    receive.reentrant_packet[1] = 2U;
    receive.reentrant_packet[2] = 3U;
    assert(rns_radio_frame_encode(packet, 10U, 8U, &encoded) == RNS_OK);
    push_frame(&phy, encoded.frames[0], encoded.lengths[0]);
    assert(rns_sx1262_interface_poll(interface_value, capture_receive, &receive,
                                     1U) == RNS_OK);
    assert(receive.count == 2U && receive.reentrant_id != 0U);

    receive.status = RNS_ERROR_OVERFLOW;
    assert(rns_radio_frame_encode(packet, 9U, 9U, &encoded) == RNS_OK);
    push_frame(&phy, encoded.frames[0], encoded.lengths[0]);
    assert(rns_sx1262_interface_poll(interface_value, capture_receive, &receive,
                                     1U) == RNS_ERROR_OVERFLOW);
    {
        rns_sx1262_scheduler_stats_t stats;
        assert(rns_sx1262_interface_get_stats(interface_value, &stats) ==
               RNS_OK);
        assert(stats.rx_malformed >= 2U && stats.rx_overflows >= 1U);
        assert(stats.signal_valid && stats.last_rssi_dbm == -112 && stats.last_snr_db == -7);
    }
    rns_sx1262_interface_destroy(interface_value);
}

static void test_generic_adapter(void) {
    fake_phy_t phy = {0};
    fake_clock_t clock = {.entropy = {5U}, .entropy_length = 1U};
    result_capture_t results = {0};
    receive_capture_t receive = {0};
    rns_sx1262_scheduler_config_t config;
    rns_interface_t *adapter = NULL;
    rns_interface_stats_t stats;
    uint8_t packet[] = {1U, 2U, 3U};
    rns_sx1262_scheduler_default_config(&config);
    config.difs_ms = 0U;
    config.contention_window_ms = 0U;
    config.cad_busy_backoff_ms = 0U;
    assert(rns_sx1262_interface_create_adapter(
               &config, &PHY_OPS, &phy, &CLOCK_OPS, &clock, capture_result,
               &results, &adapter) == RNS_OK);
    assert(rns_interface_start(adapter) == RNS_OK);
    assert(rns_interface_send(adapter, packet, sizeof(packet)) == RNS_OK);
    assert(rns_interface_poll(adapter, capture_receive, &receive, 1U) == RNS_OK);
    assert(rns_interface_poll(adapter, capture_receive, &receive, 1U) == RNS_OK);
    assert(rns_interface_get_stats(adapter, &stats) == RNS_OK &&
           stats.effective_mtu == RNS_RADIO_PACKET_MTU && stats.pending_tx == 1U &&
           stats.online == 1);
    assert(stats.radio_telemetry_valid == 1 && stats.radio_rx_frames == 0U &&
           stats.radio_tx_frames == 0U && stats.radio_cad_busy == 0U);
    rns_interface_destroy(adapter);
    assert(phy.stop_count == 1U && results.count == 1U &&
           results.outcomes[0] == RNS_SX1262_PACKET_DROPPED_STOPPED);
}

static void test_rx_stream_does_not_starve_tx(void) {
    fake_phy_t phy = {0};
    fake_clock_t clock = {.entropy = {1U}, .entropy_length = 1U};
    result_capture_t results = {0};
    receive_capture_t receive = {0};
    rns_sx1262_scheduler_config_t config;
    rns_sx1262_interface_t *interface_value =
        create_started(&phy, &clock, &results, &config);
    rns_radio_encoded_packet_t inbound;
    uint8_t packet[] = {1U, 2U, 3U};
    uint32_t id;
    size_t index;
    assert(rns_radio_frame_encode(packet, sizeof(packet), 9U, &inbound) ==
           RNS_OK);
    for (index = 0U; index < 4U; ++index) {
        push_frame(&phy, inbound.frames[0], inbound.lengths[0]);
    }
    assert(rns_sx1262_interface_send(interface_value, packet, sizeof(packet),
                                     &id) == RNS_OK);
    assert(rns_sx1262_interface_poll(interface_value, capture_receive, &receive,
                                     0U) == RNS_OK);
    assert(phy.event_count == 4U && receive.count == 0U);
    assert(rns_sx1262_interface_poll(interface_value, capture_receive, &receive,
                                     1U) == RNS_OK);
    assert(rns_sx1262_interface_poll(interface_value, capture_receive, &receive,
                                     1U) == RNS_OK);
    assert(phy.cad_count == 1U && receive.count == 2U);
    rns_sx1262_interface_destroy(interface_value);
}

int main(void) {
    test_airtime_vectors();
    test_one_and_two_frame_success();
    test_entropy_queue_and_failures();
    test_result_callback_reentrancy_and_poll_failure();
    test_cad_busy_tokens_and_contention_entropy();
    test_never_fit_and_operation_timeout();
    test_terminal_irq_failures();
    test_split_sequence_guard();
    test_duty_boundaries_and_clock_rollback();
    test_rx_reassembly_malformed_overflow_and_reentrancy();
    test_generic_adapter();
    test_rx_stream_does_not_starve_tx();
    return 0;
}
