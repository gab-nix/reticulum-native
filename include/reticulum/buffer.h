#ifndef RETICULUM_BUFFER_H
#define RETICULUM_BUFFER_H

#include <stddef.h>
#include <stdint.h>

#include "reticulum/status.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct rns_buffer {
    uint8_t *data;
    size_t len;
    size_t capacity;
} rns_buffer_t;

void rns_buffer_init(rns_buffer_t *buffer);
void rns_buffer_clear(rns_buffer_t *buffer);
rns_status_t rns_buffer_reserve(rns_buffer_t *buffer, size_t capacity);
rns_status_t rns_buffer_resize(rns_buffer_t *buffer, size_t length);
rns_status_t rns_buffer_append(rns_buffer_t *buffer, const void *data, size_t length);

#ifdef __cplusplus
}
#endif

#endif

