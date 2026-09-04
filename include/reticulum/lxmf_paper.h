#ifndef RETICULUM_LXMF_PAPER_H
#define RETICULUM_LXMF_PAPER_H

#include "reticulum/identity.h"
#include "reticulum/lxmf.h"

#ifdef __cplusplus
extern "C" {
#endif

#define LXMF_PAPER_MAX_SIZE 2210u
#define LXMF_URI_SCHEME "lxm://"
#define LXMF_URI_SCHEME_LENGTH 6u
#define LXMF_URI_MAX_CANONICAL_LENGTH 2953u
/* Canonical URI plus bounded optional slash separators accepted on input. */
#define LXMF_URI_MAX_INPUT_LENGTH 4096u

/* Signs and encrypts one paper representation. transient_id is SHA-256 over
 * the exact encrypted paper bytes. */
lxmf_status_t lxmf_paper_pack(
    const lxmf_message_t *message, const rns_identity *source_identity,
    const rns_identity *destination_identity,
    const uint8_t ratchet_public[RNS_RATCHET_PUBLIC_SIZE],
    uint8_t *paper, size_t capacity, size_t *paper_length,
    uint8_t transient_id[LXMF_MESSAGE_ID_LENGTH]);

/* Decrypts paper bytes, reconstructs the destination prefix, then performs
 * normal LXMF parsing and optional signature verification. */
lxmf_status_t lxmf_paper_unpack(
    const uint8_t *paper, size_t paper_length,
    const rns_identity *local_identity, const uint8_t *ratchet_private_keys,
    size_t ratchet_count, int enforce_ratchets,
    lxmf_verify_fn verifier, void *verify_context,
    uint8_t *plaintext, size_t plaintext_capacity, size_t *plaintext_length,
    lxmf_message_t *message, uint8_t transient_id[LXMF_MESSAGE_ID_LENGTH],
    uint8_t ratchet_id[RNS_RATCHET_ID_SIZE], int *used_ratchet);

/* Canonical URL-safe Base64 omits padding. URI output is NUL-terminated, while
 * uri_length excludes the terminator. Decode accepts ASCII-case variants of
 * the scheme and slash separators, but otherwise requires the URL-safe alphabet
 * and no explicit padding. */
lxmf_status_t lxmf_uri_encode(const uint8_t *paper, size_t paper_length,
                              char *uri, size_t capacity, size_t *uri_length);
lxmf_status_t lxmf_uri_decode(const char *uri, size_t uri_length,
                              uint8_t *paper, size_t capacity,
                              size_t *paper_length,
                              uint8_t transient_id[LXMF_MESSAGE_ID_LENGTH]);

#ifdef __cplusplus
}
#endif
#endif
