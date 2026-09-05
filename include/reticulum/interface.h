#ifndef RETICULUM_INTERFACE_H
#define RETICULUM_INTERFACE_H

#include <stddef.h>
#include <stdint.h>

#include "reticulum/status.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct rns_interface rns_interface_t;

typedef struct rns_interface_stats {
    size_t effective_mtu;
    uint32_t bitrate_bps;
    uint64_t bytes_received;
    uint64_t bytes_sent;
    uint64_t rx_overflows;
    uint64_t tx_overflows;
    size_t pending_rx;
    size_t pending_tx;
    int online;
    int outbound;
    int broadcast;
    /* Optional radio telemetry; zero validity means unavailable, not quiet. */
    int radio_telemetry_valid;
    int radio_signal_valid;
    int16_t radio_last_rssi_dbm, radio_last_snr_db;
    uint64_t radio_rx_frames, radio_tx_frames, radio_cad_busy;
    uint64_t radio_airtime_us, radio_duty_deferrals;
} rns_interface_stats_t;

typedef rns_status_t (*rns_interface_receive_fn)(void *context,
                                                 const uint8_t *packet,
                                                 size_t length);

/* The receive packet span is immutable and valid only during the callback.
 * The operation table and context are borrowed until destroy(); destroy is
 * called exactly once and owns provider-context teardown. Destroy and stop
 * must not race poll, send, or callbacks on the same handle. */
typedef struct rns_interface_ops {
    rns_status_t (*start)(void *context);
    /* budget bounds receive callbacks. Stop invoking receive if it returns
     * an error and propagate that status. Zero performs maintenance only and
     * must preserve queued RX without invoking receive; event processing and
     * recovery requiring RX delivery resume with a positive budget. */
    rns_status_t (*poll)(void *context, rns_interface_receive_fn receive,
                         void *receive_context, size_t budget);
    rns_status_t (*send)(void *context, const uint8_t *packet, size_t length);
    rns_status_t (*get_stats)(void *context, rns_interface_stats_t *stats);
    void (*stop)(void *context);
    void (*destroy)(void *context);
    /* Optional tracked enqueue. Completion is asynchronous and must not run
     * before this returns; successful enqueue is not successful RF delivery. */
    rns_status_t (*send_with_id)(void *context, const uint8_t *packet,
                               size_t length, uint32_t *id);
} rns_interface_ops_t;

rns_status_t rns_interface_create(const rns_interface_ops_t *ops, void *context,
                                  rns_interface_t **interface_out);
void rns_interface_destroy(rns_interface_t *interface_value);
rns_status_t rns_interface_start(rns_interface_t *interface_value);
rns_status_t rns_interface_poll(rns_interface_t *interface_value,
                                rns_interface_receive_fn receive,
                                void *receive_context, size_t budget);
rns_status_t rns_interface_send(rns_interface_t *interface_value,
                                const uint8_t *packet, size_t length);
rns_status_t rns_interface_get_stats(rns_interface_t *interface_value,
                                     rns_interface_stats_t *stats);
void rns_interface_stop(rns_interface_t *interface_value);
rns_status_t rns_interface_send_with_id(rns_interface_t *interface_value,
    const uint8_t *packet, size_t length, uint32_t *id);

#ifdef __cplusplus
}
#endif
#endif
