/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef RETICULUM_BOARDS_HELTEC_RETICULUM_RADIO_H
#define RETICULUM_BOARDS_HELTEC_RETICULUM_RADIO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "reticulum/heltec_sx1262.h"
#include "reticulum/interface.h"
#include "reticulum/status.h"
#include "reticulum/sx1262_interface.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct rns_heltec_reticulum_radio_config {
    rns_sx1262_scheduler_config_t scheduler;
    uint32_t frequency_hz;
    int8_t tx_power_dbm;
    bool invert_iq;
    uint32_t busy_timeout_us;
    uint8_t recovery_backoff_polls;
} rns_heltec_reticulum_radio_config_t;

/*
 * Narrow low-level seam used by the ESP implementation and host simulations.
 * The adapter owns context after create_with_backend succeeds and invokes
 * destroy exactly once. destroy must consume the context or leave it in a
 * self-contained safe quarantine on failure; it must never retain pointers to
 * adapter storage. abort_and_restart must discard all prior operations and
 * unread results before returning RNS_OK.
 */
typedef struct rns_heltec_reticulum_radio_backend_ops {
    rns_status_t (*abort_and_restart)(void *context,
                                      const rns_sx1262_config_t *config);
    rns_status_t (*start_cad)(void *context);
    rns_status_t (*receive_cad_result)(void *context,
                                       rns_sx1262_cad_result_t *result);
    rns_status_t (*send_with_id)(void *context, const uint8_t *frame,
                                 size_t frame_length, uint32_t *frame_id);
    rns_status_t (*receive_tx_result)(void *context,
                                      rns_sx1262_tx_result_t *result);
    rns_status_t (*receive)(void *context, rns_sx1262_packet_t *packet);
    rns_status_t (*stop)(void *context);
    rns_status_t (*destroy)(void *context);
} rns_heltec_reticulum_radio_backend_ops_t;

void rns_heltec_reticulum_radio_default_config(
    rns_heltec_reticulum_radio_config_t *config);

rns_status_t rns_heltec_reticulum_radio_create_with_backend(
    const rns_heltec_reticulum_radio_config_t *config,
    const rns_heltec_reticulum_radio_backend_ops_t *backend_ops,
    void *backend_context, const rns_sx1262_clock_ops_t *clock_ops,
    void *clock_context, rns_sx1262_packet_result_fn result_callback,
    void *result_context, rns_interface_t **interface_out);

#ifdef ESP_PLATFORM
/* Opens and owns the physical Heltec SX1262 handle. Radio activity starts only
 * when the returned generic interface is explicitly started. */
rns_status_t rns_heltec_reticulum_radio_create(
    const rns_heltec_reticulum_radio_config_t *config,
    const rns_sx1262_clock_ops_t *clock_ops, void *clock_context,
    rns_sx1262_packet_result_fn result_callback, void *result_context,
    rns_interface_t **interface_out);
#endif

#ifdef __cplusplus
}
#endif
#endif
