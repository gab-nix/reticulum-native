/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef RETICULUM_HELTEC_SX1262_H
#define RETICULUM_HELTEC_SX1262_H
#include "reticulum/status.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
enum {
  RNS_SX1262_MAX_PAYLOAD = 255,
  RNS_SX1262_RX_QUEUE_CAPACITY = 8,
  RNS_SX1262_TX_QUEUE_CAPACITY = 4
};
enum {
  RNS_SX1262_IRQ_TX_DONE = 1U << 0,
  RNS_SX1262_IRQ_RX_DONE = 1U << 1,
  RNS_SX1262_IRQ_HEADER_ERROR = 1U << 5,
  RNS_SX1262_IRQ_CRC_ERROR = 1U << 6,
  RNS_SX1262_IRQ_TIMEOUT = 1U << 9
};
typedef enum {
  RNS_SX1262_STOPPED = 0,
  RNS_SX1262_RECEIVING,
  RNS_SX1262_TRANSMITTING,
  RNS_SX1262_FAULT
} rns_sx1262_state_t;
typedef struct {
  uint32_t frequency_hz;
  uint32_t bandwidth_hz;
  uint8_t spreading_factor;
  uint8_t coding_rate_denominator;
  uint16_t preamble_symbols;
  bool crc_enabled;
  bool invert_iq;
  int8_t tx_power_dbm;
  uint32_t busy_timeout_us;
  uint32_t tx_timeout_ms;
} rns_sx1262_config_t;
typedef struct {
  uint8_t data[RNS_SX1262_MAX_PAYLOAD];
  size_t length;
  int16_t rssi_dbm;
  int8_t snr_db;
} rns_sx1262_packet_t;
typedef struct {
  rns_sx1262_state_t state;
  uint64_t rx_packets;
  uint64_t tx_packets;
  uint64_t rx_bytes;
  uint64_t tx_bytes;
  uint64_t header_errors;
  uint64_t crc_errors;
  uint64_t timeouts;
  uint64_t recoveries;
  uint64_t command_errors;
  uint64_t rx_overflows;
  uint64_t tx_overflows;
  rns_status_t last_error;
  size_t pending_rx;
  size_t pending_tx;
  int16_t last_rssi_dbm;
  int8_t last_snr_db;
} rns_sx1262_stats_t;
typedef struct {
  rns_status_t (*reset)(void *);
  rns_status_t (*configure)(void *, const rns_sx1262_config_t *);
  rns_status_t (*start_rx)(void *, const rns_sx1262_config_t *);
  rns_status_t (*start_tx)(void *, const rns_sx1262_config_t *,
                          const uint8_t *, size_t, uint32_t);
  rns_status_t (*get_and_clear_irq)(void *, uint16_t *);
  rns_status_t (*read_packet)(void *, rns_sx1262_packet_t *);
  rns_status_t (*standby)(void *);
} rns_sx1262_chip_ops_t;
typedef struct rns_sx1262_radio rns_sx1262_radio_t;
void rns_sx1262_default_config(rns_sx1262_config_t *);
rns_status_t rns_sx1262_radio_create(const rns_sx1262_chip_ops_t *, void *,
                                     rns_sx1262_radio_t **);
rns_status_t rns_sx1262_radio_destroy(rns_sx1262_radio_t *);
rns_status_t rns_sx1262_radio_start(rns_sx1262_radio_t *,
                                    const rns_sx1262_config_t *);
rns_status_t rns_sx1262_radio_poll(rns_sx1262_radio_t *, size_t);
rns_status_t rns_sx1262_radio_send(rns_sx1262_radio_t *, const uint8_t *,
                                   size_t);
rns_status_t rns_sx1262_radio_receive(rns_sx1262_radio_t *,
                                      rns_sx1262_packet_t *);
rns_status_t rns_sx1262_radio_get_stats(const rns_sx1262_radio_t *,
                                        rns_sx1262_stats_t *);
rns_status_t rns_sx1262_radio_stop(rns_sx1262_radio_t *);
const rns_sx1262_chip_ops_t *rns_sx1262_semtech_chip_ops(void);
#ifdef ESP_PLATFORM
typedef struct rns_heltec_sx1262 rns_heltec_sx1262_t;
rns_status_t rns_heltec_sx1262_open(rns_heltec_sx1262_t **);
rns_status_t rns_heltec_sx1262_open_with_config(
    const rns_sx1262_config_t *, rns_heltec_sx1262_t **);
rns_status_t rns_heltec_sx1262_send(rns_heltec_sx1262_t *, const uint8_t *,
                                    size_t);
rns_status_t rns_heltec_sx1262_receive(rns_heltec_sx1262_t *,
                                       rns_sx1262_packet_t *);
rns_status_t rns_heltec_sx1262_get_stats(rns_heltec_sx1262_t *,
                                         rns_sx1262_stats_t *);
rns_status_t rns_heltec_sx1262_close(rns_heltec_sx1262_t *);
#endif
#ifdef __cplusplus
}
#endif
#endif
