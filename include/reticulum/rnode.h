#ifndef RETICULUM_RNODE_H
#define RETICULUM_RNODE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "reticulum/framing.h"
#include "reticulum/status.h"

#ifdef __cplusplus
extern "C" {
#endif

#define RNS_RNODE_FREQUENCY_MIN 137000000U
#define RNS_RNODE_FREQUENCY_MAX 3000000000U
#define RNS_RNODE_FIRMWARE_MAJOR 1U
#define RNS_RNODE_FIRMWARE_MINOR 52U

typedef struct rns_rnode_endpoint rns_rnode_endpoint_t;
typedef double (*rns_rnode_clock_callback_t)(void *context);

typedef enum rns_rnode_state {
    RNS_RNODE_DOWN = 0,
    RNS_RNODE_STARTING,
    RNS_RNODE_DETECTING,
    RNS_RNODE_CONFIGURING,
    RNS_RNODE_UP
} rns_rnode_state_t;

typedef struct rns_rnode_options {
    const char *device;
    uint32_t frequency;
    uint32_t bandwidth;
    uint8_t tx_power;
    uint8_t spreading_factor;
    uint8_t coding_rate;
    bool flow_control;
    bool short_airtime_limit_set;
    bool long_airtime_limit_set;
    uint16_t short_airtime_limit_hundredths;
    uint16_t long_airtime_limit_hundredths;
    double startup_delay_seconds;
    double detect_timeout_seconds;
    double validation_timeout_seconds;
    double reconnect_seconds;
    rns_rnode_clock_callback_t clock;
    void *clock_context;
} rns_rnode_options_t;

typedef struct rns_rnode_info {
    rns_rnode_state_t state;
    rns_status_t last_error;
    uint8_t firmware_major;
    uint8_t firmware_minor;
    uint8_t platform;
    uint8_t mcu;
    uint8_t last_hardware_error;
    uint32_t reported_frequency;
    uint32_t reported_bandwidth;
    uint8_t reported_tx_power;
    uint8_t reported_spreading_factor;
    uint8_t reported_coding_rate;
    bool radio_on;
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
} rns_rnode_info_t;

void rns_rnode_options_init(rns_rnode_options_t *options);
rns_status_t rns_rnode_endpoint_create(rns_rnode_endpoint_t **endpoint,
                                       const rns_rnode_options_t *options);
void rns_rnode_endpoint_destroy(rns_rnode_endpoint_t *endpoint);
rns_status_t rns_rnode_endpoint_send(rns_rnode_endpoint_t *endpoint,
                                     const uint8_t *packet,
                                     size_t packet_length);
rns_status_t rns_rnode_endpoint_poll(rns_rnode_endpoint_t *endpoint,
                                     size_t max_packets,
                                     rns_frame_callback_t callback,
                                     void *context, size_t *processed);
rns_status_t rns_rnode_endpoint_leave(rns_rnode_endpoint_t *endpoint);
rns_status_t rns_rnode_endpoint_info(const rns_rnode_endpoint_t *endpoint,
                                     rns_rnode_info_t *info);

#ifdef __cplusplus
}
#endif

#endif
