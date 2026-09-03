#ifndef RETICULUM_LXMF_PROPAGATION_SESSION_H
#define RETICULUM_LXMF_PROPAGATION_SESSION_H

#include "reticulum/lxmf_propagation.h"
#include "reticulum/runtime.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct lxmf_pn_session lxmf_pn_session_t;
typedef enum {
    LXMF_PN_IDLE = 0, LXMF_PN_PATH, LXMF_PN_LINK, LXMF_PN_LIST,
    LXMF_PN_DOWNLOAD, LXMF_PN_ACK, LXMF_PN_UPLOAD,
    LXMF_PN_COMPLETE, LXMF_PN_FAILED, LXMF_PN_CANCELLED
} lxmf_pn_session_state_t;
typedef struct {
    lxmf_pn_session_state_t state;
    rns_status_t error;
    uint8_t remote_error;
    size_t available, received, acknowledged;
    size_t transferred_parts, total_parts;
} lxmf_pn_session_progress_t;
/* Return true ONLY after authenticated/decrypted content has been durably
 * accepted, or a durable duplicate is confirmed. False retains the server copy.
 * This layer transports opaque encrypted LXMF, not plaintext or trusted data. */
typedef bool (*lxmf_pn_message_callback_t)(const uint8_t transient_id[32],
    const uint8_t *message, size_t length, void *context);
typedef void (*lxmf_pn_session_callback_t)(
    const lxmf_pn_session_progress_t *progress, void *context);
typedef struct {
    rns_runtime_t *runtime; /* Borrowed; must outlive the session. */
    const rns_identity *local_identity; /* Copied; private identity required. */
    const rns_identity *node_identity; /* Copied public identity, verified by caller. */
    uint8_t node_destination[16]; /* Must match lxmf.propagation identity hash. */
    double timeout_seconds; /* Zero selects 60 seconds per network operation. */
    size_t max_response_size; /* Zero selects 8 MiB. */
    bool retain_on_node;
    lxmf_pn_message_callback_t message_callback;
    lxmf_pn_session_callback_t state_callback;
    void *callback_context;
} lxmf_pn_session_options_t;

rns_status_t lxmf_pn_session_create(lxmf_pn_session_t **session,
    const lxmf_pn_session_options_t *options);
void lxmf_pn_session_destroy(lxmf_pn_session_t *session);
/* Caller polls runtime separately, then this service with a monotonic time.
 * Callbacks are synchronous and spans callback-scoped. Do not destroy, start,
 * cancel or recursively poll the session from either callback. */
rns_status_t lxmf_pn_session_poll(lxmf_pn_session_t *session, double now);
rns_status_t lxmf_pn_session_sync(lxmf_pn_session_t *session, double now);
/* Copies a bounded upstream upload. Caller provides already destination-
 * encrypted, propagation-stamped messages. COMPLETE confirms node transfer,
 * never final-recipient delivery. Always uses a proof-tracked Resource. */
rns_status_t lxmf_pn_session_upload(lxmf_pn_session_t *session,
    const lxmf_pn_upload_t *upload, double now);
void lxmf_pn_session_cancel(lxmf_pn_session_t *session);
const lxmf_pn_session_progress_t *lxmf_pn_session_progress(
    const lxmf_pn_session_t *session);

#ifdef __cplusplus
}
#endif
#endif
