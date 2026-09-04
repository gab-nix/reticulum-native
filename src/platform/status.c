#include "reticulum/status.h"

const char *rns_status_string(rns_status_t status) {
    switch (status) {
        case RNS_OK: return "success";
        case RNS_ERROR_INVALID_ARGUMENT: return "invalid argument";
        case RNS_ERROR_NO_MEMORY: return "out of memory";
        case RNS_ERROR_IO: return "I/O error";
        case RNS_ERROR_TIMEOUT: return "timeout";
        case RNS_ERROR_CRYPTO: return "cryptographic error";
        case RNS_ERROR_PROTOCOL: return "protocol error";
        case RNS_ERROR_NOT_FOUND: return "not found";
        case RNS_ERROR_INVALID_STATE: return "invalid state";
        case RNS_ERROR_UNSUPPORTED: return "unsupported";
        case RNS_ERROR_OVERFLOW: return "overflow";
        default: return "unknown error";
    }
}
