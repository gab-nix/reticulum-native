#ifndef RETICULUM_CHANNEL_H
#define RETICULUM_CHANNEL_H

#include <stddef.h>
#include <stdint.h>

#include "reticulum/status.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Reticulum 1.5.2 Channel envelopes use three network-order uint16 fields. */
#define RNS_CHANNEL_HEADER_BYTES 6u
#define RNS_CHANNEL_MAX_ENVELOPE_BYTES 500u
#define RNS_CHANNEL_MAX_PAYLOAD_BYTES (RNS_CHANNEL_MAX_ENVELOPE_BYTES - RNS_CHANNEL_HEADER_BYTES)
#define RNS_CHANNEL_MAX_WINDOW 32u

typedef double (*rns_channel_clock_fn)(void *context);
typedef rns_status_t (*rns_channel_send_fn)(const uint8_t *envelope, size_t length,
                                            void *context);
typedef rns_status_t (*rns_channel_receive_fn)(uint16_t message_type, uint16_t sequence,
                                               const uint8_t *payload, size_t length,
                                               void *context);

typedef enum rns_channel_event {
    RNS_CHANNEL_EVENT_DELIVERED = 1,
    RNS_CHANNEL_EVENT_RETRY = 2,
    RNS_CHANNEL_EVENT_TIMEOUT = 3,
    RNS_CHANNEL_EVENT_DUPLICATE = 4,
    RNS_CHANNEL_EVENT_OUT_OF_ORDER = 5
} rns_channel_event_t;

typedef void (*rns_channel_event_fn)(rns_channel_event_t event, uint16_t sequence,
                                     void *context);

typedef struct rns_channel_config {
    uint8_t initial_window;
    uint8_t max_window;
    uint8_t max_retries;
    double retry_timeout;
    rns_channel_clock_fn clock;
    void *clock_context;
    rns_channel_send_fn send;
    void *send_context;
    rns_channel_receive_fn receive;
    void *receive_context;
    rns_channel_event_fn event;
    void *event_context;
} rns_channel_config_t;

typedef struct rns_channel_slot {
    uint8_t used;
    uint8_t retries;
    uint16_t sequence;
    uint16_t length;
    double deadline;
    uint8_t envelope[RNS_CHANNEL_MAX_ENVELOPE_BYTES];
} rns_channel_slot_t;

typedef struct rns_channel {
    rns_channel_config_t config;
    uint16_t next_sequence;
    uint16_t expected_sequence;
    uint8_t window;
    uint8_t success_count;
    size_t outstanding;
    rns_channel_slot_t transmit[RNS_CHANNEL_MAX_WINDOW];
    rns_channel_slot_t receive[RNS_CHANNEL_MAX_WINDOW];
} rns_channel_t;

rns_status_t rns_channel_envelope_encode(uint16_t message_type, uint16_t sequence,
                                         const uint8_t *payload, size_t payload_length,
                                         uint8_t *output, size_t capacity, size_t *output_length);
rns_status_t rns_channel_envelope_decode(const uint8_t *envelope, size_t envelope_length,
                                         uint16_t *message_type, uint16_t *sequence,
                                         const uint8_t **payload, size_t *payload_length);

rns_status_t rns_channel_init(rns_channel_t *channel, const rns_channel_config_t *config);
rns_status_t rns_channel_send(rns_channel_t *channel, uint16_t message_type,
                              const uint8_t *payload, size_t payload_length,
                              uint16_t *sequence);
rns_status_t rns_channel_mark_delivered(rns_channel_t *channel, uint16_t sequence);
rns_status_t rns_channel_receive(rns_channel_t *channel, const uint8_t *envelope,
                                 size_t envelope_length);
size_t rns_channel_tick(rns_channel_t *channel);

#ifdef __cplusplus
}
#endif

#endif
