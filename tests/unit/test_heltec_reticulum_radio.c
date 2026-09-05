/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "reticulum/boards/heltec_reticulum_radio.h"

#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>

#include "reticulum/hal.h"

/* White-box the private PHY adapter so metadata and borrowed-frame lifetime
   are verified without expanding the production API solely for tests. */
#include "../../firmware/heltec_wifi_lora_32_v3_1/components/heltec_reticulum_radio/heltec_reticulum_radio.c"

enum { EVENT_CAPACITY = 16 };

typedef struct fake_backend {
    rns_sx1262_config_t configured;
    rns_sx1262_cad_result_t cad[EVENT_CAPACITY];
    rns_sx1262_tx_result_t tx[EVENT_CAPACITY];
    rns_sx1262_packet_t rx[EVENT_CAPACITY];
    size_t cad_head, cad_count, tx_head, tx_count, rx_head, rx_count;
    uint32_t next_id;
    unsigned restarts, starts_cad, sends, stops, destroys;
    bool fail_restart;
    bool fail_stop;
    bool fail_destroy;
} fake_backend_t;

typedef struct fake_clock {
    uint64_t now_ms;
    uint8_t entropy;
} fake_clock_t;

typedef struct capture {
    uint8_t packet[RNS_RADIO_PACKET_MTU];
    size_t packet_length;
    uint32_t result_ids[8];
    rns_sx1262_packet_outcome_t outcomes[8];
    rns_status_t statuses[8];
    size_t result_count;
} capture_t;

static void clear_backend_events(fake_backend_t *fake) {
    memset(fake->cad, 0, sizeof(fake->cad));
    memset(fake->tx, 0, sizeof(fake->tx));
    memset(fake->rx, 0, sizeof(fake->rx));
    fake->cad_head = 0U;
    fake->cad_count = 0U;
    fake->tx_head = 0U;
    fake->tx_count = 0U;
    fake->rx_head = 0U;
    fake->rx_count = 0U;
}

static rns_status_t fake_restart(void *context,
                                 const rns_sx1262_config_t *config) {
    fake_backend_t *fake = context;
    fake->restarts++;
    clear_backend_events(fake);
    if (fake->fail_restart) {
        fake->fail_restart = false;
        return RNS_ERROR_IO;
    }
    fake->configured = *config;
    return RNS_OK;
}

static rns_status_t fake_start_cad(void *context) {
    fake_backend_t *fake = context;
    fake->starts_cad++;
    return RNS_OK;
}

static rns_status_t fake_receive_cad(
    void *context, rns_sx1262_cad_result_t *result) {
    fake_backend_t *fake = context;
    if (fake->cad_count == 0U) {
        return RNS_ERROR_NOT_FOUND;
    }
    *result = fake->cad[fake->cad_head];
    fake->cad_head = (fake->cad_head + 1U) % EVENT_CAPACITY;
    fake->cad_count--;
    return RNS_OK;
}

static rns_status_t fake_send(void *context, const uint8_t *frame,
                              size_t frame_length, uint32_t *frame_id) {
    fake_backend_t *fake = context;
    assert(frame != NULL && frame_length > 0U &&
           frame_length <= RNS_SX1262_MAX_PAYLOAD);
    fake->sends++;
    fake->next_id++;
    if (fake->next_id == 0U) {
        fake->next_id++;
    }
    *frame_id = fake->next_id;
    return RNS_OK;
}

static rns_status_t fake_receive_tx(void *context,
                                    rns_sx1262_tx_result_t *result) {
    fake_backend_t *fake = context;
    if (fake->tx_count == 0U) {
        return RNS_ERROR_NOT_FOUND;
    }
    *result = fake->tx[fake->tx_head];
    fake->tx_head = (fake->tx_head + 1U) % EVENT_CAPACITY;
    fake->tx_count--;
    return RNS_OK;
}

static rns_status_t fake_receive(void *context, rns_sx1262_packet_t *packet) {
    fake_backend_t *fake = context;
    if (fake->rx_count == 0U) {
        return RNS_ERROR_NOT_FOUND;
    }
    *packet = fake->rx[fake->rx_head];
    fake->rx_head = (fake->rx_head + 1U) % EVENT_CAPACITY;
    fake->rx_count--;
    return RNS_OK;
}

static rns_status_t fake_stop(void *context) {
    fake_backend_t *fake = context;
    fake->stops++;
    return fake->fail_stop ? RNS_ERROR_IO : RNS_OK;
}

static rns_status_t fake_destroy(void *context) {
    fake_backend_t *fake = context;
    fake->destroys++;
    return fake->fail_destroy ? RNS_ERROR_TIMEOUT : RNS_OK;
}

static const rns_heltec_reticulum_radio_backend_ops_t BACKEND_OPS = {
    .abort_and_restart = fake_restart,
    .start_cad = fake_start_cad,
    .receive_cad_result = fake_receive_cad,
    .send_with_id = fake_send,
    .receive_tx_result = fake_receive_tx,
    .receive = fake_receive,
    .stop = fake_stop,
    .destroy = fake_destroy};

static uint64_t clock_now(void *context) {
    return ((fake_clock_t *)context)->now_ms;
}

static rns_status_t clock_entropy(void *context, uint8_t *output,
                                  size_t length) {
    fake_clock_t *clock = context;
    size_t index;
    for (index = 0U; index < length; ++index) {
        output[index] = clock->entropy++;
    }
    return RNS_OK;
}

static const rns_sx1262_clock_ops_t CLOCK_OPS = {
    .monotonic_ms = clock_now, .entropy = clock_entropy};

static void push_cad(fake_backend_t *fake,
                     rns_sx1262_cad_outcome_t outcome,
                     rns_status_t status) {
    size_t tail = (fake->cad_head + fake->cad_count) % EVENT_CAPACITY;
    assert(fake->cad_count < EVENT_CAPACITY);
    fake->cad[tail] =
        (rns_sx1262_cad_result_t){.outcome = outcome, .status = status};
    fake->cad_count++;
}

static void push_tx(fake_backend_t *fake, uint32_t id,
                    rns_sx1262_tx_outcome_t outcome, rns_status_t status) {
    size_t tail = (fake->tx_head + fake->tx_count) % EVENT_CAPACITY;
    assert(fake->tx_count < EVENT_CAPACITY);
    fake->tx[tail] = (rns_sx1262_tx_result_t){
        .id = id, .outcome = outcome, .status = status, .length = 1U};
    fake->tx_count++;
}

static void push_rx(fake_backend_t *fake, const uint8_t *frame,
                    size_t frame_length, int16_t rssi, int8_t snr) {
    size_t tail = (fake->rx_head + fake->rx_count) % EVENT_CAPACITY;
    assert(fake->rx_count < EVENT_CAPACITY &&
           frame_length <= RNS_SX1262_MAX_PAYLOAD);
    memcpy(fake->rx[tail].data, frame, frame_length);
    fake->rx[tail].length = frame_length;
    fake->rx[tail].rssi_dbm = rssi;
    fake->rx[tail].snr_db = snr;
    fake->rx_count++;
}

static rns_status_t capture_receive(void *context, const uint8_t *packet,
                                    size_t packet_length) {
    capture_t *capture = context;
    assert(packet_length <= sizeof(capture->packet));
    memcpy(capture->packet, packet, packet_length);
    capture->packet_length = packet_length;
    return RNS_OK;
}

static void capture_result(void *context, uint32_t packet_id,
                           rns_sx1262_packet_outcome_t outcome,
                           rns_status_t status) {
    capture_t *capture = context;
    size_t index = capture->result_count++;
    assert(index < 8U);
    capture->result_ids[index] = packet_id;
    capture->outcomes[index] = outcome;
    capture->statuses[index] = status;
}

static rns_interface_t *create_interface(
    fake_backend_t *fake, fake_clock_t *clock, capture_t *capture,
    rns_heltec_reticulum_radio_config_t *config) {
    rns_interface_t *interface_value = NULL;
    rns_heltec_reticulum_radio_default_config(config);
    config->scheduler.difs_ms = 0U;
    config->scheduler.contention_window_ms = 0U;
    assert(rns_heltec_reticulum_radio_create_with_backend(
               config, &BACKEND_OPS, fake, &CLOCK_OPS, clock, capture_result,
               capture, &interface_value) == RNS_OK);
    return interface_value;
}

static void reach_cad(rns_interface_t *interface_value, capture_t *capture) {
    assert(rns_interface_poll(interface_value, capture_receive, capture, 1U) ==
           RNS_OK);
    assert(rns_interface_poll(interface_value, capture_receive, capture, 1U) ==
           RNS_OK);
}

static void test_config_and_lifecycle(void) {
    fake_backend_t fake = {0};
    fake_clock_t clock = {0};
    capture_t capture = {0};
    rns_heltec_reticulum_radio_config_t config;
    rns_interface_t *interface_value =
        create_interface(&fake, &clock, &capture, &config);
    assert(fake.restarts == 0U && fake.stops == 0U);

    config.frequency_hz = 915000000U;
    config.tx_power_dbm = 17;
    config.invert_iq = true;
    config.busy_timeout_us = 76543U;
    config.recovery_backoff_polls = 9U;
    config.scheduler.bandwidth_hz = 250000U;
    config.scheduler.spreading_factor = 10U;
    config.scheduler.coding_rate_denominator = 8U;
    config.scheduler.preamble_symbols = 23U;
    config.scheduler.crc_enabled = false;
    config.scheduler.tx_timeout_margin_ms = 789U;
    rns_interface_destroy(interface_value);
    memset(&fake, 0, sizeof(fake));
    assert(rns_heltec_reticulum_radio_create_with_backend(
               &config, &BACKEND_OPS, &fake, &CLOCK_OPS, &clock,
               capture_result, &capture, &interface_value) == RNS_OK);
    assert(rns_interface_start(interface_value) == RNS_OK);
    assert(fake.restarts == 1U && fake.configured.frequency_hz == 915000000U &&
           fake.configured.bandwidth_hz == 250000U &&
           fake.configured.spreading_factor == 10U &&
           fake.configured.coding_rate_denominator == 8U &&
           fake.configured.preamble_symbols == 23U &&
           !fake.configured.crc_enabled && fake.configured.invert_iq &&
           fake.configured.tx_power_dbm == 17 &&
           fake.configured.busy_timeout_us == 76543U &&
           fake.configured.tx_timeout_margin_ms == 789U &&
           fake.configured.recovery_backoff_polls == 9U);
    rns_interface_stop(interface_value);
    assert(fake.stops == 1U);
    assert(rns_interface_start(interface_value) == RNS_OK &&
           fake.restarts == 2U);
    fake.fail_destroy = true;
    rns_interface_destroy(interface_value);
    assert(fake.stops == 2U && fake.destroys == 1U);

    rns_heltec_reticulum_radio_default_config(&config);
    config.scheduler.explicit_header = false;
    interface_value = (rns_interface_t *)(uintptr_t)1U;
    assert(rns_heltec_reticulum_radio_create_with_backend(
               &config, &BACKEND_OPS, &fake, &CLOCK_OPS, &clock,
               capture_result, &capture, &interface_value) ==
           RNS_ERROR_INVALID_ARGUMENT);
    assert(interface_value == NULL);
}

static void test_one_and_two_frame_correlation(void) {
    fake_backend_t fake = {.next_id = UINT32_MAX - 1U};
    fake_clock_t clock = {0};
    capture_t capture = {0};
    rns_heltec_reticulum_radio_config_t config;
    rns_interface_t *interface_value =
        create_interface(&fake, &clock, &capture, &config);
    uint8_t short_packet[] = {0x11U, 0x22U};
    uint8_t split_packet[500];
    uint32_t stale_id;
    size_t index;
    for (index = 0U; index < sizeof(split_packet); ++index) {
        split_packet[index] = (uint8_t)index;
    }
    assert(rns_interface_start(interface_value) == RNS_OK);
    uint32_t tracked_id;
    assert(rns_interface_send_with_id(interface_value, short_packet,
                              sizeof(short_packet),&tracked_id) == RNS_OK);
    reach_cad(interface_value, &capture);
    assert(fake.starts_cad == 1U);
    push_cad(&fake, RNS_SX1262_CAD_CLEAR, RNS_OK);
    assert(rns_interface_poll(interface_value, capture_receive, &capture, 1U) ==
           RNS_OK);
    assert(fake.sends == 1U && fake.next_id == UINT32_MAX);
    stale_id = fake.next_id - 1U;
    push_tx(&fake, stale_id, RNS_SX1262_TX_SENT, RNS_OK);
    push_tx(&fake, fake.next_id, RNS_SX1262_TX_SENT, RNS_OK);
    assert(rns_interface_poll(interface_value, capture_receive, &capture, 1U) ==
           RNS_OK);
    assert(capture.result_count == 1U &&
           capture.outcomes[0] == RNS_SX1262_PACKET_SENT);
    assert(capture.result_ids[0]==tracked_id);

    assert(rns_interface_send(interface_value, split_packet,
                              sizeof(split_packet)) == RNS_OK);
    reach_cad(interface_value, &capture);
    push_cad(&fake, RNS_SX1262_CAD_CLEAR, RNS_OK);
    assert(rns_interface_poll(interface_value, capture_receive, &capture, 1U) ==
           RNS_OK);
    assert(fake.next_id == 1U);
    push_tx(&fake, 1U, RNS_SX1262_TX_SENT, RNS_OK);
    assert(rns_interface_poll(interface_value, capture_receive, &capture, 1U) ==
           RNS_OK);
    assert(fake.next_id == 2U && fake.sends == 3U &&
           capture.result_count == 1U);
    push_tx(&fake, 2U, RNS_SX1262_TX_SENT, RNS_OK);
    assert(rns_interface_poll(interface_value, capture_receive, &capture, 1U) ==
           RNS_OK);
    assert(capture.result_count == 2U &&
           capture.outcomes[1] == RNS_SX1262_PACKET_SENT);
    rns_interface_destroy(interface_value);
}

static void test_cad_outcomes_cancel_and_restart_failure(void) {
    fake_backend_t fake = {0};
    fake_clock_t clock = {0};
    capture_t capture = {0};
    rns_heltec_reticulum_radio_config_t config;
    rns_interface_t *interface_value =
        create_interface(&fake, &clock, &capture, &config);
    const uint8_t packet[] = {0x44U};
    assert(rns_interface_start(interface_value) == RNS_OK);

    assert(rns_interface_send(interface_value, packet, sizeof(packet)) ==
           RNS_OK);
    reach_cad(interface_value, &capture);
    push_cad(&fake, RNS_SX1262_CAD_BUSY, RNS_OK);
    assert(rns_interface_poll(interface_value, capture_receive, &capture, 1U) ==
           RNS_OK);
    clock.now_ms += config.scheduler.cad_busy_backoff_ms + 1U;
    assert(rns_interface_poll(interface_value, capture_receive, &capture, 1U) ==
           RNS_OK);
    push_cad(&fake, RNS_SX1262_CAD_FAILED, RNS_ERROR_IO);
    assert(rns_interface_poll(interface_value, capture_receive, &capture, 1U) ==
           RNS_ERROR_IO);
    assert(capture.result_count == 1U &&
           capture.outcomes[0] == RNS_SX1262_PACKET_DROPPED_PHY);

    assert(rns_interface_send(interface_value, packet, sizeof(packet)) ==
           RNS_OK);
    reach_cad(interface_value, &capture);
    push_cad(&fake, RNS_SX1262_CAD_CLEAR, RNS_OK);
    assert(rns_interface_poll(interface_value, capture_receive, &capture, 1U) ==
           RNS_OK);
    push_tx(&fake, fake.next_id, RNS_SX1262_TX_RADIO_FAILED, RNS_ERROR_IO);
    assert(rns_interface_poll(interface_value, capture_receive, &capture, 1U) ==
           RNS_ERROR_IO);
    assert(capture.result_count == 2U &&
           capture.outcomes[1] == RNS_SX1262_PACKET_DROPPED_PHY);

    assert(rns_interface_send(interface_value, packet, sizeof(packet)) ==
           RNS_OK);
    reach_cad(interface_value, &capture);
    push_cad(&fake, RNS_SX1262_CAD_CLEAR, RNS_OK);
    assert(rns_interface_poll(interface_value, capture_receive, &capture, 1U) ==
           RNS_OK);
    fake.fail_restart = true;
    clock.now_ms += config.scheduler.tx_timeout_margin_ms + 1000U;
    assert(rns_interface_poll(interface_value, capture_receive, &capture, 1U) ==
           RNS_ERROR_IO);
    assert(fake.restarts >= 3U && capture.result_count == 3U &&
           capture.statuses[2] == RNS_ERROR_IO);
    rns_interface_destroy(interface_value);
}

static void test_rx_reassembly_and_lifetime(void) {
    fake_backend_t fake = {0};
    fake_clock_t clock = {0};
    capture_t capture = {0};
    rns_heltec_reticulum_radio_config_t config;
    rns_interface_t *interface_value =
        create_interface(&fake, &clock, &capture, &config);
    const uint8_t frame[] = {0x20U, 0xdeU, 0xadU, 0xbeU, 0xefU};
    assert(rns_interface_start(interface_value) == RNS_OK);
    push_rx(&fake, frame, sizeof(frame), -103, -7);
    assert(rns_interface_poll(interface_value, capture_receive, &capture, 1U) ==
           RNS_OK);
    assert(capture.packet_length == 4U && capture.packet[0] == 0xdeU &&
           capture.packet[3] == 0xefU);
    assert(rns_interface_poll(interface_value, capture_receive, &capture, 1U) ==
           RNS_OK);
    assert(capture.packet[0] == 0xdeU);
    rns_interface_destroy(interface_value);
}

static void test_phy_metadata_stale_and_cancel(void) {
    fake_backend_t fake = {0};
    heltec_radio_adapter_t adapter = {0};
    rns_sx1262_phy_event_t event;
    rns_sx1262_scheduler_config_t scheduler_config;
    const uint8_t first[] = {0x10U, 0x20U};
    const uint8_t second[] = {0x30U};

    rns_sx1262_scheduler_default_config(&scheduler_config);
    adapter.backend_ops = &BACKEND_OPS;
    adapter.backend_context = &fake;
    adapter.backend_started = true;
    adapter.radio_config = (rns_sx1262_config_t){
        .frequency_hz = 868200000U,
        .bandwidth_hz = 125000U,
        .spreading_factor = 8U,
        .coding_rate_denominator = 5U,
        .preamble_symbols = 18U,
        .crc_enabled = true,
        .tx_power_dbm = 14,
        .busy_timeout_us = 100000U,
        .tx_timeout_margin_ms = 250U,
        .recovery_backoff_polls = 5U};

    push_rx(&fake, first, sizeof(first), -111, -9);
    assert(phy_poll_event(&adapter, &event) == RNS_OK &&
           event.type == RNS_SX1262_PHY_EVENT_RX_FRAME &&
           event.frame == adapter.rx_cache.data && event.frame_length == 2U &&
           event.frame[1] == 0x20U && event.rssi_dbm == -111 &&
           event.snr_db == -9);
    push_rx(&fake, second, sizeof(second), -80, 6);
    assert(phy_poll_event(&adapter, &event) == RNS_OK &&
           event.frame == adapter.rx_cache.data && event.frame_length == 1U &&
           event.frame[0] == 0x30U && event.rssi_dbm == -80 &&
           event.snr_db == 6);

    push_cad(&fake, RNS_SX1262_CAD_CLEAR, RNS_OK);
    assert(phy_poll_event(&adapter, &event) == RNS_ERROR_NOT_FOUND &&
           fake.cad_count == 0U);

    adapter.pending = PENDING_TX;
    adapter.scheduler_token = 0xa5a5U;
    adapter.lower_tx_id = 55U;
    push_tx(&fake, 54U, RNS_SX1262_TX_SENT, RNS_OK);
    push_tx(&fake, 55U, RNS_SX1262_TX_SENT, RNS_OK);
    assert(phy_poll_event(&adapter, &event) == RNS_OK &&
           event.type == RNS_SX1262_PHY_EVENT_TX_DONE &&
           event.operation_token == 0xa5a5U && fake.tx_count == 0U);

    adapter.pending = PENDING_CAD;
    adapter.scheduler_token = 77U;
    assert(phy_cancel(&adapter, &scheduler_config, 77U) == RNS_OK &&
           adapter.pending == PENDING_NONE && fake.restarts == 1U);
    push_tx(&fake, 55U, RNS_SX1262_TX_SENT, RNS_OK);
    assert(phy_poll_event(&adapter, &event) == RNS_ERROR_NOT_FOUND &&
           fake.tx_count == 0U);
}

int main(void) {
    test_config_and_lifecycle();
    test_one_and_two_frame_correlation();
    test_cad_outcomes_cancel_and_restart_failure();
    test_rx_reassembly_and_lifetime();
    test_phy_metadata_stale_and_cancel();
    puts("heltec Reticulum radio adapter tests passed");
    return 0;
}
