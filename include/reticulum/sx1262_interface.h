/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef RETICULUM_SX1262_INTERFACE_H
#define RETICULUM_SX1262_INTERFACE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "reticulum/interface.h"
#include "reticulum/radio_framing.h"
#include "reticulum/status.h"

#ifdef __cplusplus
extern "C" {
#endif

#define RNS_SX1262_PACKET_QUEUE_CAPACITY 4U
#define RNS_SX1262_DUTY_BUCKET_COUNT 61U
#define RNS_SX1262_DEFAULT_DUTY_CYCLE_PPM 10000U

typedef enum rns_sx1262_phy_event_type {
    RNS_SX1262_PHY_EVENT_RX_FRAME = 0,
    RNS_SX1262_PHY_EVENT_CAD_CLEAR,
    RNS_SX1262_PHY_EVENT_CAD_BUSY,
    RNS_SX1262_PHY_EVENT_CAD_FAILED,
    RNS_SX1262_PHY_EVENT_TX_DONE,
    RNS_SX1262_PHY_EVENT_TX_FAILED
} rns_sx1262_phy_event_type_t;

typedef struct rns_sx1262_phy_event {
    rns_sx1262_phy_event_type_t type;
    uint32_t operation_token;
    const uint8_t *frame;
    size_t frame_length;
    int16_t rssi_dbm;
    int8_t snr_db;
    rns_status_t status;
} rns_sx1262_phy_event_t;

struct rns_sx1262_scheduler_config;

/*
 * The PHY and clock contexts are borrowed until destroy. poll_event returns
 * RNS_ERROR_NOT_FOUND when no event is ready. Event frame storage is borrowed
 * only for that call. All methods are invoked by one caller-owned poll thread.
 * A ready completion for the active operation (CAD clear/busy/failure or TX
 * done/failure) must precede RX and stale notifications. This lets bounded
 * polling resolve authoritative completion before evaluating its deadline.
 * CAD_FAILED, TX_DONE and TX_FAILED are terminal and must leave the PHY ready
 * for a later operation. The scheduler calls cancel_operation for its own
 * deadline or a poll failure before releasing any borrowed frame storage.
 */
typedef struct rns_sx1262_phy_ops {
    rns_status_t (*start)(void *context,
                          const struct rns_sx1262_scheduler_config *config);
    rns_status_t (*poll_event)(void *context,
                               rns_sx1262_phy_event_t *event);
    rns_status_t (*start_cad)(
        void *context, const struct rns_sx1262_scheduler_config *config,
        uint32_t operation_token);
    rns_status_t (*transmit)(
        void *context, const struct rns_sx1262_scheduler_config *config,
        const uint8_t *frame, size_t frame_length, uint32_t operation_token);
    /* Cancels the matching CAD/TX operation and restores receive readiness. */
    rns_status_t (*cancel_operation)(
        void *context, const struct rns_sx1262_scheduler_config *config,
        uint32_t operation_token);
    rns_status_t (*stop)(void *context);
} rns_sx1262_phy_ops_t;

typedef struct rns_sx1262_clock_ops {
    uint64_t (*monotonic_ms)(void *context);
    rns_status_t (*entropy)(void *context, uint8_t *output, size_t length);
} rns_sx1262_clock_ops_t;

typedef struct rns_sx1262_scheduler_config {
    uint32_t bandwidth_hz;
    uint8_t spreading_factor;
    uint8_t coding_rate_denominator;
    uint16_t preamble_symbols;
    bool explicit_header;
    bool crc_enabled;
    uint32_t duty_cycle_ppm;
    uint32_t difs_ms;
    uint32_t contention_window_ms;
    uint32_t cad_busy_backoff_ms;
    uint32_t cad_timeout_ms;
    uint32_t tx_timeout_margin_ms;
    uint8_t max_cad_attempts;
    uint64_t fragment_timeout_ms;
    uint64_t sequence_reuse_guard_ms;
} rns_sx1262_scheduler_config_t;

typedef enum rns_sx1262_packet_outcome {
    RNS_SX1262_PACKET_SENT = 0,
    RNS_SX1262_PACKET_DROPPED_PHY,
    RNS_SX1262_PACKET_DROPPED_CONTENTION,
    RNS_SX1262_PACKET_DROPPED_STOPPED
} rns_sx1262_packet_outcome_t;

typedef void (*rns_sx1262_packet_result_fn)(
    void *context, uint32_t packet_id, rns_sx1262_packet_outcome_t outcome,
    rns_status_t status);

typedef struct rns_sx1262_scheduler_stats {
    uint64_t packets_queued;
    uint64_t packets_sent;
    uint64_t packets_dropped;
    uint64_t tx_overflows;
    uint64_t frames_sent;
    uint64_t tx_bytes;
    uint64_t rx_frames;
    uint64_t rx_packets;
    uint64_t rx_bytes;
    uint64_t rx_malformed;
    uint64_t rx_overflows;
    uint64_t stale_control_events;
    uint64_t cad_busy;
    uint64_t cad_failures;
    uint64_t duty_deferrals;
    uint64_t clock_regressions;
    uint64_t airtime_reserved_us;
    uint64_t rolling_airtime_us;
    size_t pending_packets;
    bool online;
    bool transmitting;
    rns_status_t last_error;
} rns_sx1262_scheduler_stats_t;

typedef struct rns_sx1262_interface rns_sx1262_interface_t;

void rns_sx1262_scheduler_default_config(
    rns_sx1262_scheduler_config_t *config);

/* Integer, round-up LoRa time-on-air calculation in microseconds. */
rns_status_t rns_sx1262_airtime_us(
    const rns_sx1262_scheduler_config_t *config, size_t frame_length,
    uint64_t *airtime_us);

rns_status_t rns_sx1262_interface_create(
    const rns_sx1262_scheduler_config_t *config,
    const rns_sx1262_phy_ops_t *phy_ops, void *phy_context,
    const rns_sx1262_clock_ops_t *clock_ops, void *clock_context,
    rns_sx1262_packet_result_fn result_callback, void *result_context,
    rns_sx1262_interface_t **interface_out);
void rns_sx1262_interface_destroy(rns_sx1262_interface_t *interface_value);
rns_status_t rns_sx1262_interface_start(
    rns_sx1262_interface_t *interface_value);
rns_status_t rns_sx1262_interface_poll(
    rns_sx1262_interface_t *interface_value, rns_interface_receive_fn receive,
    void *receive_context, size_t budget);
rns_status_t rns_sx1262_interface_send(
    rns_sx1262_interface_t *interface_value, const uint8_t *packet,
    size_t packet_length, uint32_t *packet_id);
rns_status_t rns_sx1262_interface_get_stats(
    rns_sx1262_interface_t *interface_value,
    rns_sx1262_scheduler_stats_t *stats);
rns_status_t rns_sx1262_interface_stop(
    rns_sx1262_interface_t *interface_value);

/*
 * Creates an owning generic interface adapter. Destroying the returned
 * rns_interface_t also destroys the scheduler; the borrowed PHY remains owned
 * by its provider. This is the runtime-facing integration seam.
 * Receive and result callbacks may enqueue packets and inspect statistics.
 * Recursive poll/destroy is rejected or unsupported; stop from a callback is
 * safe and causes the outer poll to return without touching the PHY again.
 */
rns_status_t rns_sx1262_interface_create_adapter(
    const rns_sx1262_scheduler_config_t *config,
    const rns_sx1262_phy_ops_t *phy_ops, void *phy_context,
    const rns_sx1262_clock_ops_t *clock_ops, void *clock_context,
    rns_sx1262_packet_result_fn result_callback, void *result_context,
    rns_interface_t **interface_out);

#ifdef __cplusplus
}
#endif

#endif
