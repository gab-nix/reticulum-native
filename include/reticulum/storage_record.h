#ifndef RETICULUM_STORAGE_RECORD_H
#define RETICULUM_STORAGE_RECORD_H

#include <stddef.h>
#include <stdint.h>

#include "reticulum/status.h"

#ifdef __cplusplus
extern "C" {
#endif

#define RNS_STORAGE_RECORD_VERSION 1U
#define RNS_STORAGE_RECORD_HEADER_SIZE 24U
#define RNS_STORAGE_RECORD_MAX_PAYLOAD 4096U

/* Portable envelope used by transactional embedded storage providers. Records
 * bind the logical key, generation and payload with a CRC32 checksum. */
rns_status_t rns_storage_record_encoded_size(size_t payload_length,
                                             size_t *encoded_length);
rns_status_t rns_storage_record_encode(const char *key, uint32_t generation,
                                       const uint8_t *payload,
                                       size_t payload_length,
                                       uint8_t *output, size_t capacity,
                                       size_t *encoded_length);
rns_status_t rns_storage_record_decode(const char *key, const uint8_t *record,
                                       size_t record_length, uint8_t *output,
                                       size_t capacity, size_t *payload_length,
                                       uint32_t *generation);

/* Deterministic dual-slot journal policy. Invalid/corrupt slots are ignored;
 * a valid older slot remains readable after an interrupted replacement. */
rns_status_t rns_storage_record_select_slot(int valid_a, uint32_t generation_a,
                                            int valid_b, uint32_t generation_b,
                                            char *selected_slot);
rns_status_t rns_storage_record_next_slot(int valid_a, uint32_t generation_a,
                                          int valid_b, uint32_t generation_b,
                                          char *write_slot,
                                          uint32_t *next_generation);

#ifdef __cplusplus
}
#endif

#endif
