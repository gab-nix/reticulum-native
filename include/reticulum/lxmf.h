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
#define LXMF_FIELD_TICKET 0x0cu
/* Upper bound for one canonical LXMF representation handled by the codec.
 * Transport/resource policy can impose a smaller limit. */
#define LXMF_MAX_MESSAGE_SIZE (8u * 1024u * 1024u)

typedef enum {
    LXMF_OK = 0,
    LXMF_ERR_ARGUMENT,
    LXMF_ERR_BOUNDS,
    LXMF_ERR_FORMAT,
    LXMF_ERR_CRYPTO,
    LXMF_ERR_SIGNATURE,
    LXMF_ERR_CANCELLED,
    LXMF_ERR_TIMEOUT,
    /* The operation is valid but cannot progress until an external condition,
     * such as learning a peer identity or path, becomes available. */
    LXMF_ERR_PENDING,
    /* The signer's identity is not held locally, so the signature can be
     * neither confirmed nor refuted. Distinct from LXMF_ERR_SIGNATURE, which
     * means a known identity did not sign the message. */
    LXMF_ERR_UNKNOWN_SIGNER,
    /* The message did not satisfy the configured inbound stamp policy. */
    LXMF_ERR_STAMP,
    /* Application-owned source policy rejected this message; not a signature
     * failure. Applies even when the source identity is not yet known. */
    LXMF_ERR_BLOCKED
} lxmf_status_t;

/* Trust carried alongside a retained message. */
typedef enum {
    LXMF_SIGNATURE_VERIFIED = 0,
    LXMF_SIGNATURE_UNVERIFIED = 1,
    LXMF_SIGNATURE_FAILED = 2
} lxmf_signature_state_t;

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

typedef struct {
    bool present;
    uint64_t expires_at;
    uint8_t ticket[LXMF_TICKET_LENGTH];
} lxmf_ticket_field_t;

/* Extracts the standard FIELD_TICKET value [expiry, ticket] from an LXMF
 * fields map while leaving every field byte owned by the caller. An absent
 * field returns OK with present=false; malformed or duplicate ticket fields
 * return LXMF_ERR_FORMAT. */
lxmf_status_t lxmf_fields_parse_ticket(const uint8_t *fields,
                                       size_t fields_length,
                                       lxmf_ticket_field_t *ticket);

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

/* Unpacks borrowed slices, computes message_id, and optionally verifies.
 * On LXMF_ERR_UNKNOWN_SIGNER the message is fully populated and its borrowed
 * slices are valid: the payload parsed, only its signer is unknown. Every
 * other error leaves the message unusable. */
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
typedef struct lxmf_stamp_job lxmf_stamp_job_t;
typedef enum {
    LXMF_STAMP_PREPARING, LXMF_STAMP_SEARCHING, LXMF_STAMP_COMPLETE,
    LXMF_STAMP_CANCELLED, LXMF_STAMP_FAILED
} lxmf_stamp_state_t;
typedef struct {
    lxmf_stamp_state_t state;
    uint32_t prepared_rounds;
    uint64_t attempts;
    lxmf_status_t result;
} lxmf_stamp_job_progress_t;
/* One work unit performs one bounded HKDF preparation round or one candidate
 * hash. Each poll accepts at most this budget, including preparation. */
#define LXMF_STAMP_POLL_MAX_UNITS 64u
/* initial_nonce may be NULL for secure random selection, or a caller-owned
 * 32-byte nonce for reproducible diagnostics. The job copies all inputs. */
lxmf_status_t lxmf_stamp_job_create(const uint8_t message_id[32], uint8_t cost,
    const uint8_t initial_nonce[32], lxmf_stamp_job_t **job);
lxmf_status_t lxmf_stamp_job_poll(lxmf_stamp_job_t *job, uint32_t work_units);
void lxmf_stamp_job_cancel(lxmf_stamp_job_t *job);
void lxmf_stamp_job_destroy(lxmf_stamp_job_t *job);
lxmf_status_t lxmf_stamp_job_progress(const lxmf_stamp_job_t *job,
    lxmf_stamp_job_progress_t *progress);
lxmf_status_t lxmf_stamp_job_result(const lxmf_stamp_job_t *job,
    uint8_t stamp[LXMF_POW_STAMP_LENGTH], uint8_t *value);
lxmf_status_t lxmf_pow_stamp_generate(const uint8_t message_id[32], uint8_t cost,
    lxmf_stamp_progress_fn progress, void *progress_context,
    uint8_t stamp[LXMF_POW_STAMP_LENGTH], uint8_t *value, uint64_t *attempts);
lxmf_status_t lxmf_pow_stamp_validate(const uint8_t message_id[32], uint8_t cost,
    const uint8_t stamp[LXMF_POW_STAMP_LENGTH], uint8_t *value);

const char *lxmf_status_string(lxmf_status_t status);
const char *lxmf_signature_state_string(lxmf_signature_state_t state);

#ifdef __cplusplus
}
#endif

#endif
