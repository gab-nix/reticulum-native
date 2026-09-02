#ifndef RETICULUM_REQUEST_H
#define RETICULUM_REQUEST_H

#include <stddef.h>
#include <stdint.h>

#include "reticulum/status.h"

#ifdef __cplusplus
extern "C" {
#endif

#define RNS_REQUEST_ID_LENGTH 16u
#define RNS_REQUEST_PATH_MAX 1024u

typedef struct rns_request_view {
    double requested_at;
    uint8_t path_hash[RNS_REQUEST_ID_LENGTH];
    const uint8_t *data_msgpack;
    size_t data_msgpack_length;
} rns_request_view_t;

typedef struct rns_response_view {
    uint8_t request_id[RNS_REQUEST_ID_LENGTH];
    const uint8_t *response;
    size_t response_length;
} rns_response_view_t;

/* data_msgpack must contain exactly one MessagePack object. Empty emits nil. */
rns_status_t rns_request_encode(const char *path, double requested_at,
                                const uint8_t *data_msgpack,
                                size_t data_msgpack_length, uint8_t *output,
                                size_t output_capacity, size_t *output_length);
rns_status_t rns_request_decode(const uint8_t *input, size_t input_length,
                                rns_request_view_t *request);
rns_status_t rns_response_decode(const uint8_t *input, size_t input_length,
                                 rns_response_view_t *response);

#ifdef __cplusplus
}
#endif
#endif
