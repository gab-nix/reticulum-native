#ifndef RETICULUM_LINK_H
#define RETICULUM_LINK_H

#include <stddef.h>
#include <stdint.h>

#include "reticulum/identity.h"

#define RNS_LINK_REQUEST_KEY_BYTES 64u
#define RNS_LINK_SIGNALLING_BYTES 3u
#define RNS_LINK_REQUEST_BYTES 67u
#define RNS_LINK_PROOF_BYTES 99u
#define RNS_LINK_MODE_AES256_CBC 1u
#define RNS_LINK_MTU_MASK 0x1fffffu

typedef double (*rns_link_clock)(void *context);

typedef enum {
    RNS_LINK_PENDING = 0,
    RNS_LINK_HANDSHAKE = 1,
    RNS_LINK_ACTIVE = 2,
    RNS_LINK_CLOSED = 3
} rns_link_state;

typedef enum { RNS_LINK_INITIATOR = 1, RNS_LINK_RESPONDER = 2 } rns_link_role;

typedef struct {
    rns_link_role role;
    rns_link_state state;
    rns_link_clock clock;
    void *clock_context;
    double request_time;
    double deadline;
    double rtt;
    double activated_at;
    uint32_t mtu;
    uint8_t mode;
    uint8_t link_id[16];
    uint8_t private_key[32];
    uint8_t public_key[32];
    uint8_t signing_private[32];
    uint8_t signing_public[32];
    uint8_t peer_public[32];
    uint8_t peer_signing_public[32];
    uint8_t derived_key[64];
    rns_identity remote_identity;
} rns_link;

int rns_link_signalling_encode(uint32_t mtu, uint8_t mode, uint8_t out[3]);
int rns_link_signalling_decode(const uint8_t in[3], uint32_t *mtu, uint8_t *mode);
int rns_link_id_from_request_packet(const uint8_t *raw, size_t raw_length, uint8_t out[16]);

int rns_link_initiator_init(rns_link *link, const rns_identity *destination,
                            uint32_t mtu, double timeout,
                            rns_link_clock clock, void *clock_context);
/* Deterministic test/vector constructor; private inputs are raw 32-byte seeds/scalars. */
int rns_link_initiator_init_keys(rns_link *link, const rns_identity *destination,
                                 const uint8_t x25519_private[32], const uint8_t ed25519_private[32],
                                 uint32_t mtu, double timeout,
                                 rns_link_clock clock, void *clock_context);
int rns_link_build_request_payload(const rns_link *link, uint8_t out[67]);
int rns_link_initiator_set_request_packet(rns_link *link, const uint8_t *raw, size_t raw_length);

int rns_link_responder_accept(rns_link *link, const rns_identity *local_identity,
                              const uint8_t *request_raw, size_t request_raw_length,
                              double timeout, rns_link_clock clock, void *clock_context);
int rns_link_responder_accept_key(rns_link *link, const rns_identity *local_identity,
                                  const uint8_t responder_x25519_private[32],
                                  const uint8_t *request_raw, size_t request_raw_length,
                                  double timeout, rns_link_clock clock, void *clock_context);
int rns_link_build_proof(const rns_link *link, uint8_t out[99]);
int rns_link_initiator_accept_proof(rns_link *link, const uint8_t *proof, size_t proof_length);

int rns_link_build_rtt_confirm(rns_link *link, uint8_t *out, size_t capacity, size_t *out_length);
int rns_link_responder_accept_rtt(rns_link *link, const uint8_t *token, size_t token_length);
int rns_link_check_timeout(rns_link *link);

int rns_link_encrypt(const rns_link *link, const uint8_t *plaintext, size_t plaintext_length,
                     uint8_t *out, size_t capacity, size_t *out_length);
int rns_link_decrypt(const rns_link *link, const uint8_t *token, size_t token_length,
                     uint8_t *out, size_t capacity, size_t *out_length);

#endif
