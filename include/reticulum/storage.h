#ifndef RETICULUM_STORAGE_H
#define RETICULUM_STORAGE_H

#include <stddef.h>
#include <stdint.h>

#include "reticulum/status.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct rns_storage rns_storage_t;

typedef struct rns_storage_ops {
    rns_status_t (*read)(void *context, const char *key, uint8_t *output,
                         size_t capacity, size_t *length);
    rns_status_t (*write_atomic)(void *context, const char *key,
                                 const uint8_t *data, size_t length);
    rns_status_t (*remove)(void *context, const char *key);
    void (*destroy)(void *context);
} rns_storage_ops_t;

rns_status_t rns_storage_create(const rns_storage_ops_t *ops, void *context,
                                rns_storage_t **storage);
void rns_storage_destroy(rns_storage_t *storage);
rns_status_t rns_storage_read(rns_storage_t *storage, const char *key,
                              uint8_t *output, size_t capacity, size_t *length);
rns_status_t rns_storage_write_atomic(rns_storage_t *storage, const char *key,
                                      const uint8_t *data, size_t length);
rns_status_t rns_storage_remove(rns_storage_t *storage, const char *key);

#ifdef __cplusplus
}
#endif
#endif
