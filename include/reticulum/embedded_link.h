#ifndef RETICULUM_EMBEDDED_LINK_H
#define RETICULUM_EMBEDDED_LINK_H
#include "reticulum/link.h"
#include "reticulum/interface.h"
#define RNS_EMBEDDED_LINK_CAPACITY 4u
typedef struct rns_embedded_link_manager rns_embedded_link_manager;
typedef struct {
    void (*state)(void *, const uint8_t[16], rns_link_state, rns_status_t);
    rns_status_t (*data)(void *, const uint8_t[16], const uint8_t *, size_t);
    void (*proof)(void *, const uint8_t[16], const uint8_t[32]);
    void (*transmission)(void *, const uint8_t[16], const uint8_t[32], rns_status_t);
} rns_embedded_link_callbacks;
/* Single-owner, non-reentrant mutations. Callbacks may query state/identity;
 * spans expire at callback return. Identity and interface are borrowed. No
 * send completion is inferred from enqueue. Caller must forward completions.
 * Interface must already be started and implement send_with_id. Callback
 * mutations/destruction are forbidden. Close reclaims its slot after callback;
 * state query subsequently returns NOT_FOUND. Queued control packets cannot be
 * retracted from providers without cancellation support, but stale completions
 * are ignored. Backwards monotonic time closes existing links with TIMEOUT;
 * the next operation may establish a new link on the corrected clock.
 * IFAC-wrapped input must be authenticated/stripped before receive.
 * No transit routing or Resource support is provided. */
rns_status_t rns_embedded_link_create(const rns_identity *, const uint8_t[16],
    rns_interface_t *, const rns_embedded_link_callbacks *, void *, rns_embedded_link_manager **);
void rns_embedded_link_destroy(rns_embedded_link_manager *);
/* One manager serves one application protocol; connect may reuse an identified
 * inbound backchannel matching the complete peer identity within that service. */
rns_status_t rns_embedded_link_connect(rns_embedded_link_manager *, const uint8_t[16],
    const rns_identity *, uint64_t, uint8_t[16]);
rns_status_t rns_embedded_link_receive(rns_embedded_link_manager *, const uint8_t *, size_t, uint64_t);
void rns_embedded_link_poll(rns_embedded_link_manager *, uint64_t);
void rns_embedded_link_tx_complete(rns_embedded_link_manager *, uint32_t, rns_status_t, uint64_t);
rns_status_t rns_embedded_link_send(rns_embedded_link_manager *, const uint8_t[16],
    const uint8_t *, size_t, uint64_t, uint8_t[32]);
rns_status_t rns_embedded_link_close(rns_embedded_link_manager *, const uint8_t[16], uint64_t);
rns_status_t rns_embedded_link_state(const rns_embedded_link_manager *, const uint8_t[16], rns_link_state *);
rns_status_t rns_embedded_link_authenticated_peer(const rns_embedded_link_manager *, const uint8_t[16], rns_identity *);
#endif
