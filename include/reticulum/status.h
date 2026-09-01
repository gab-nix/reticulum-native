#ifndef RETICULUM_STATUS_H
#define RETICULUM_STATUS_H

#ifdef __cplusplus
extern "C" {
#endif

typedef enum rns_status {
    RNS_OK = 0,
    RNS_ERROR_INVALID_ARGUMENT,
    RNS_ERROR_NO_MEMORY,
    RNS_ERROR_IO,
    RNS_ERROR_TIMEOUT,
    RNS_ERROR_CRYPTO,
    RNS_ERROR_PROTOCOL,
    RNS_ERROR_NOT_FOUND,
    RNS_ERROR_INVALID_STATE,
    RNS_ERROR_UNSUPPORTED,
    RNS_ERROR_OVERFLOW
} rns_status_t;

const char *rns_status_string(rns_status_t status);

#ifdef __cplusplus
}
#endif

#endif

