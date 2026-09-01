#ifndef RETICULUM_FRAMING_H
#define RETICULUM_FRAMING_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "reticulum/status.h"

#ifdef __cplusplus
extern "C" {
#endif

#define RNS_HDLC_FLAG 0x7eU
#define RNS_HDLC_ESCAPE 0x7dU
#define RNS_KISS_FEND 0xc0U
#define RNS_KISS_FESC 0xdbU
#define RNS_KISS_TFEND 0xdcU
#define RNS_KISS_TFESC 0xddU
#define RNS_KISS_DATA_COMMAND 0x00U

typedef rns_status_t (*rns_frame_callback_t)(const uint8_t *frame,
                                             size_t frame_length,
                                             void *context);

typedef struct rns_hdlc_decoder {
    uint8_t *buffer;
    size_t capacity;
    size_t length;
    size_t malformed_frames;
    size_t oversized_frames;
    bool synchronized;
    bool escaped;
    bool discarding;
} rns_hdlc_decoder_t;

typedef struct rns_kiss_decoder {
    uint8_t *buffer;
    size_t capacity;
    size_t length;
    size_t malformed_frames;
    size_t oversized_frames;
    uint8_t command;
    bool synchronized;
    bool have_command;
    bool escaped;
    bool discarding;
} rns_kiss_decoder_t;

void rns_hdlc_decoder_init(rns_hdlc_decoder_t *decoder,
                           uint8_t *storage,
                           size_t storage_capacity);
void rns_hdlc_decoder_reset(rns_hdlc_decoder_t *decoder);
rns_status_t rns_hdlc_decoder_feed(rns_hdlc_decoder_t *decoder,
                                   const uint8_t *input,
                                   size_t input_length,
                                   rns_frame_callback_t callback,
                                   void *context);
rns_status_t rns_hdlc_encode(const uint8_t *frame,
                             size_t frame_length,
                             uint8_t *output,
                             size_t output_capacity,
                             size_t *output_length);

void rns_kiss_decoder_init(rns_kiss_decoder_t *decoder,
                           uint8_t *storage,
                           size_t storage_capacity);
void rns_kiss_decoder_reset(rns_kiss_decoder_t *decoder);
rns_status_t rns_kiss_decoder_feed(rns_kiss_decoder_t *decoder,
                                   const uint8_t *input,
                                   size_t input_length,
                                   rns_frame_callback_t callback,
                                   void *context);
rns_status_t rns_kiss_encode(uint8_t port,
                             const uint8_t *frame,
                             size_t frame_length,
                             uint8_t *output,
                             size_t output_capacity,
                             size_t *output_length);

#ifdef __cplusplus
}
#endif

#endif

