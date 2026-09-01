#ifndef RETICULUM_LXMF_H
#define RETICULUM_LXMF_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "reticulum/identity.h"

#ifdef __cplusplus
extern "C" {
#endif

#define LXMF_DESTINATION_LENGTH 16u
#define LXMF_SOURCE_LENGTH 16u
#define LXMF_SIGNATURE_LENGTH 64u
#define LXMF_MESSAGE_ID_LENGTH 32u
#define LXMF_TICKET_LENGTH 16u
#define LXMF_STAMP_LENGTH 16u
#define LXMF_POW_STAMP_LENGTH 32u
#define LXMF_STAMP_WORKBLOCK_ROUNDS 3000u

typedef enum {
    LXMF_OK = 0,
    LXMF_ERR_ARGUMENT,
    LXMF_ERR_BOUNDS,
    LXMF_ERR_FORMAT,
    LXMF_ERR_CRYPTO,
    LXMF_ERR_SIGNATURE,
    LXMF_ERR_CANCELLED
} lxmf_status_t;

/* Binary slices are borrowed. Callers retain ownership of pointed-to bytes. */
typedef struct {
    const uint8_t *data;
    size_t len;
} lxmf_slice_t;

typedef struct {
    uint8_t destination[LXMF_DESTINATION_LENGTH];
    uint8_t source[LXMF_SOURCE_LENGTH];
    uint8_t signature[LXMF_SIGNATURE_LENGTH];
    uint8_t message_id[LXMF_MESSAGE_ID_LENGTH];
    double timestamp;
    lxmf_slice_t title;
    lxmf_slice_t content;
    /* One complete MessagePack object. It must be a map. Empty means {}. */
    lxmf_slice_t fields_msgpack;
    bool has_stamp;
    size_t stamp_len;
    uint8_t stamp[LXMF_POW_STAMP_LENGTH];
} lxmf_message_t;

typedef lxmf_status_t (*lxmf_sign_fn)(void *context,
                                      const uint8_t *data, size_t data_len,
                                      uint8_t signature[LXMF_SIGNATURE_LENGTH]);
typedef lxmf_status_t (*lxmf_verify_fn)(void *context,
                                        const uint8_t source[LXMF_SOURCE_LENGTH],
                                        const uint8_t *data, size_t data_len,
                                        const uint8_t signature[LXMF_SIGNATURE_LENGTH]);

typedef const rns_identity *(*lxmf_identity_resolver_fn)(void *context,
    const uint8_t source[LXMF_SOURCE_LENGTH]);
typedef struct { lxmf_identity_resolver_fn resolve; void *resolve_context; }
    lxmf_identity_verifier_context_t;
lxmf_status_t lxmf_identity_signer(void *identity, const uint8_t *data,
    size_t data_len, uint8_t signature[LXMF_SIGNATURE_LENGTH]);
lxmf_status_t lxmf_identity_verifier(void *context,
    const uint8_t source[LXMF_SOURCE_LENGTH], const uint8_t *data,
    size_t data_len, const uint8_t signature[LXMF_SIGNATURE_LENGTH]);

/* Returns a conservative upper bound suitable for allocating an output buffer. */
size_t lxmf_pack_bound(const lxmf_message_t *message);

/* Packs and signs an LXMF base message. A NULL signer is rejected. */
lxmf_status_t lxmf_pack(const lxmf_message_t *message,
                        lxmf_sign_fn signer, void *sign_context,
                        uint8_t *output, size_t output_capacity,
                        size_t *output_len);

/* Unpacks borrowed slices, computes message_id, and optionally verifies. */
lxmf_status_t lxmf_unpack(const uint8_t *input, size_t input_len,
                          lxmf_verify_fn verifier, void *verify_context,
                          lxmf_message_t *message);

/* SHA-256 used by LXMF/Reticulum. Exposed for protocol vectors and tickets. */
void lxmf_sha256(const uint8_t *data, size_t len,
                 uint8_t digest[LXMF_MESSAGE_ID_LENGTH]);

/* A ticket-backed direct stamp is trunc16(SHA-256(ticket || message_id)). */
void lxmf_ticket_stamp(const uint8_t ticket[LXMF_TICKET_LENGTH],
                       const uint8_t message_id[LXMF_MESSAGE_ID_LENGTH],
                       uint8_t stamp[LXMF_STAMP_LENGTH]);
bool lxmf_ticket_stamp_valid(const uint8_t stamp[LXMF_STAMP_LENGTH],
                             const uint8_t ticket[LXMF_TICKET_LENGTH],
                             const uint8_t message_id[LXMF_MESSAGE_ID_LENGTH]);

typedef bool (*lxmf_stamp_progress_fn)(void *context, uint64_t attempts);
lxmf_status_t lxmf_pow_stamp_generate(const uint8_t message_id[32], uint8_t cost,
    lxmf_stamp_progress_fn progress, void *progress_context,
    uint8_t stamp[LXMF_POW_STAMP_LENGTH], uint8_t *value, uint64_t *attempts);
lxmf_status_t lxmf_pow_stamp_validate(const uint8_t message_id[32], uint8_t cost,
    const uint8_t stamp[LXMF_POW_STAMP_LENGTH], uint8_t *value);

const char *lxmf_status_string(lxmf_status_t status);

#ifdef __cplusplus
}
#endif

#endif
