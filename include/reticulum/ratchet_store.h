#ifndef RETICULUM_RATCHET_STORE_H
#define RETICULUM_RATCHET_STORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "reticulum/identity.h"
#include "reticulum/status.h"

#ifdef __cplusplus
extern "C" {
#endif

#define RNS_RATCHET_STORE_DEFAULT_RETAINED 512u
#define RNS_RATCHET_STORE_DEFAULT_INTERVAL 1800u
#define RNS_RATCHET_STORE_MAX_RETAINED 512u
#define RNS_RATCHET_STORE_PATH_MAX 1023u

typedef struct rns_ratchet_store rns_ratchet_store_t;

/* Opens the signed MessagePack ratchet file used by Reticulum destinations.
 * A missing file creates and persists an empty store. The identity is borrowed
 * for the store lifetime and must contain its private signing key. */
rns_status_t rns_ratchet_store_open(rns_ratchet_store_t **store,
                                    const char *path,
                                    const rns_identity *identity,
                                    size_t retained,
                                    uint64_t rotation_interval_seconds);
void rns_ratchet_store_close(rns_ratchet_store_t *store);

/* Ensures a current ratchet exists. Rotation occurs only when now is strictly
 * later than the last rotation plus the configured interval, matching RNS.
 * All output buffers are caller-owned. */
rns_status_t rns_ratchet_store_current(
    rns_ratchet_store_t *store, uint64_t now,
    uint8_t private_key[RNS_RATCHET_PRIVATE_SIZE],
    uint8_t public_key[RNS_RATCHET_PUBLIC_SIZE],
    uint8_t ratchet_id[RNS_RATCHET_ID_SIZE], bool *rotated);

/* Copies private ratchets newest-first for decryption. */
rns_status_t rns_ratchet_store_copy_private(
    const rns_ratchet_store_t *store, uint8_t *private_keys,
    size_t capacity_keys, size_t *key_count);

size_t rns_ratchet_store_count(const rns_ratchet_store_t *store);

#ifdef __cplusplus
}
#endif

#endif
