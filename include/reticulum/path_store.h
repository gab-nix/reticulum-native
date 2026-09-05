#ifndef RETICULUM_PATH_STORE_H
#define RETICULUM_PATH_STORE_H

#include <stddef.h>
#include <stdint.h>

#include "reticulum/status.h"
#include "reticulum/transport.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Encodes live path entries into a portable, checksummed snapshot. Wall time
 * is supplied explicitly so the codec remains independent of platform clocks.
 * Version 2 retains public identities already attached to paths by the caller;
 * it never serializes private keys. On overflow, output_length receives the
 * required capacity. Snapshots are trusted local storage: the checksum detects
 * corruption, not malicious replacement, and is not announce verification. */
rns_status_t rns_path_store_encode(const rns_transport *transport,
                                   uint64_t wall_time_ms,
                                   uint8_t *output, size_t output_capacity,
                                   size_t *output_length,
                                   size_t *encoded_count);

/* Transactionally replaces the path table from a valid snapshot. Time spent
 * offline is deducted from every stored lifetime; expired records are skipped.
 * Version 1 route-only snapshots remain readable and never gain an identity.
 * Dedupe state is intentionally never restored. */
rns_status_t rns_path_store_decode(rns_transport *transport,
                                   uint64_t wall_time_ms,
                                   const uint8_t *input, size_t input_length,
                                   size_t *decoded_count);

#ifdef __cplusplus
}
#endif

#endif
