#ifndef RETICULUM_KISS_H
#define RETICULUM_KISS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "reticulum/framing.h"
#include "reticulum/status.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct rns_kiss_endpoint rns_kiss_endpoint_t;
typedef double (*rns_kiss_clock_callback_t)(void *context);

typedef enum rns_kiss_state {
    RNS_KISS_DOWN = 0,
    RNS_KISS_CONFIGURING,
    RNS_KISS_UP
} rns_kiss_state_t;

typedef struct rns_kiss_options {
    const char *device;
    uint32_t speed;
    uint8_t data_bits;
    char parity;
    uint8_t stop_bits;
    uint16_t preamble_ms;
    uint16_t tx_tail_ms;
    uint8_t persistence;
    uint16_t slot_time_ms;
    bool flow_control;
    double configure_delay_seconds;
    double reconnect_seconds;
    rns_kiss_clock_callback_t clock;
    void *clock_context;
} rns_kiss_options_t;

typedef struct rns_kiss_info {
    rns_kiss_state_t state;
    rns_status_t last_error;
    uint64_t packets_received;
    uint64_t packets_sent;
    uint64_t bytes_received;
    uint64_t bytes_sent;
    uint64_t packets_dropped;
    uint64_t connection_attempts;
    uint64_t connections_established;
    uint64_t connections_lost;
    size_t malformed_frames;
    size_t oversized_frames;
    size_t pending_packets;
} rns_kiss_info_t;

void rns_kiss_options_init(rns_kiss_options_t *options);
rns_status_t rns_kiss_endpoint_create(rns_kiss_endpoint_t **endpoint,
                                      const rns_kiss_options_t *options);
void rns_kiss_endpoint_destroy(rns_kiss_endpoint_t *endpoint);
rns_status_t rns_kiss_endpoint_send(rns_kiss_endpoint_t *endpoint,
                                    const uint8_t *packet,
                                    size_t packet_length);
rns_status_t rns_kiss_endpoint_poll(rns_kiss_endpoint_t *endpoint,
                                    size_t max_packets,
                                    rns_frame_callback_t callback,
                                    void *context, size_t *processed);
rns_status_t rns_kiss_endpoint_info(const rns_kiss_endpoint_t *endpoint,
                                    rns_kiss_info_t *info);

#ifdef __cplusplus
}
#endif

#endif
