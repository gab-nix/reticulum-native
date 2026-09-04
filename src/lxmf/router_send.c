#include "reticulum/lxmf_router.h"
#include "reticulum/destination.h"
#include "reticulum/crypto.h"
#include "reticulum/lxmf_delivery.h"
#include "reticulum/packet.h"
#include "reticulum/hal.h"

#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <math.h>

static void report(lxmf_router_t *router,
                   const uint8_t id[LXMF_MESSAGE_ID_LENGTH],
                   lxmf_delivery_status_t state, lxmf_status_t result) {
    if (router->config.delivery_callback != NULL)
        router->config.delivery_callback(router->config.delivery_context, id,
                                         state, result);
}

static void report_event(lxmf_router_t *router,
                         const uint8_t id[LXMF_MESSAGE_ID_LENGTH],
                         lxmf_delivery_method_t method,
                         lxmf_delivery_status_t state,
                         lxmf_queue_reason_t queue_reason,
                         lxmf_status_t result, uint32_t attempt) {
    if (router->config.event_callback == NULL) return;
    lxmf_router_event_t event = {
        .method = method,
        .state = state,
        .queue_reason = queue_reason,
        .result = result,
        .attempt = attempt
    };
    memcpy(event.message_id, id, sizeof event.message_id);
    router->config.event_callback(router->config.event_context, &event);
}

static uint64_t router_monotonic_ms(const lxmf_router_t *router) {
    if (router != NULL && router->config.monotonic_clock != NULL)
        return router->config.monotonic_clock(
            router->config.monotonic_clock_context);
    uint64_t now = 0U;
    return rns_hal_monotonic_ms(&now) == RNS_OK ? now : 0U;
}

static void finish_direct_attempt(
    lxmf_router_t *router, const uint8_t id[LXMF_MESSAGE_ID_LENGTH],
    lxmf_delivery_method_t method, uint32_t attempt, lxmf_status_t result,
    bool cancelled, bool retryable) {
    lxmf_delivery_metadata_t metadata;
    if (lxmf_store_read_delivery(router->config.store, id, &metadata) !=
            LXMF_OK || metadata.attempts != attempt)
        return;
    lxmf_delivery_status_t state;
    if (cancelled) {
        state = LXMF_DELIVERY_FAILED;
        metadata.queue_reason = LXMF_QUEUE_REASON_CANCELLED;
        metadata.retry_at_ms = 0U;
    } else if (!retryable) {
        state = LXMF_DELIVERY_FAILED;
        metadata.queue_reason = LXMF_QUEUE_REASON_RETRY_EXHAUSTED;
        metadata.retry_at_ms = 0U;
    } else {
        uint32_t limit = router->config.direct_retry_limit != 0U
                             ? router->config.direct_retry_limit : 5U;
        if (attempt >= limit) {
            state = LXMF_DELIVERY_FAILED;
            metadata.queue_reason = LXMF_QUEUE_REASON_RETRY_EXHAUSTED;
            metadata.retry_at_ms = 0U;
        } else {
            state = LXMF_DELIVERY_QUEUED;
            metadata.queue_reason = LXMF_QUEUE_REASON_RETRY_BACKOFF;
            uint64_t base = router->config.direct_retry_base_ms != 0U
                                ? router->config.direct_retry_base_ms : 10000U;
            uint32_t shift = attempt > 7U
                                 ? 6U
                                 : (attempt == 0U ? 0U : attempt - 1U);
            uint64_t delay = base > (UINT64_MAX >> shift)
                                 ? UINT64_MAX : base << shift;
            uint64_t now = router_monotonic_ms(router);
            metadata.retry_at_ms = delay > UINT64_MAX - now
                                       ? UINT64_MAX : now + delay;
        }
    }
    metadata.has_proof_id = false;
    memset(metadata.proof_id, 0, sizeof metadata.proof_id);
    (void)lxmf_store_update_delivery(router->config.store, id, &metadata);
    (void)lxmf_store_update_status(router->config.store, id, state);
    report(router, id, state, result);
    report_event(router, id, method, state, metadata.queue_reason, result,
                 attempt);
}

static lxmf_status_t receive_representation(
    lxmf_router_t *router, const uint8_t *packed, size_t packed_length,
    lxmf_delivery_method_t method, bool exempt_stamp, bool *inserted_out);
static lxmf_status_t router_private_ratchets(
    const lxmf_router_t *router, const uint8_t *supplied, size_t supplied_count,
    const uint8_t **effective, size_t *effective_count, uint8_t **owned);
static void direct_link_state_changed(rns_runtime_link_t *link,
                                      rns_link_state state,
                                      rns_status_t reason, void *context);
static void direct_link_packet_received(rns_runtime_link_t *link,
                                        uint8_t packet_context,
                                        const uint8_t *plaintext,
                                        size_t plaintext_length,
                                        void *context);
static void direct_link_accepted(rns_runtime_destination_t *destination,
                                 rns_runtime_link_t *link, void *context);
static void direct_link_identified(rns_runtime_link_t *link,
                                   const rns_identity *identity,
                                   void *context);
static bool direct_resource_accept(
    rns_runtime_link_t *link,
    const rns_resource_advertisement_t *advertisement, void *context);
static void direct_resource_received(
    rns_runtime_link_t *link, const uint8_t resource_hash[32],
    rns_status_t status, const uint8_t *data, size_t data_length,
    void *context);
static lxmf_status_t prepare_outbound_representation(
    lxmf_router_t *router, const lxmf_store_message_t *stored,
    const uint8_t id[LXMF_MESSAGE_ID_LENGTH], uint8_t *packed,
    size_t packed_capacity, size_t *packed_length);

static uint64_t router_wall_time(const lxmf_router_t *router) {
    return router->config.wall_clock(router->config.wall_clock_context);
}

static size_t inbound_message_limit(const lxmf_router_t *router) {
    return router->config.max_incoming_message_size != 0u
        ? router->config.max_incoming_message_size : LXMF_STORE_MAX_PACKED;
}

static bool source_blocked(const lxmf_router_t *router,
                            const uint8_t source[LXMF_SOURCE_LENGTH]) {
    return router->config.is_source_blocked != NULL &&
        router->config.is_source_blocked(router->config.source_policy_context,
                                         source);
}

static lxmf_status_t validate_inbound_stamp(
    lxmf_router_t *router, const lxmf_message_t *message) {
    uint8_t cost = router->config.inbound_stamp_cost;
    if (cost == 0u) return LXMF_OK;
    if (!message->has_stamp) return LXMF_ERR_STAMP;
    lxmf_status_t status;
    if (message->stamp_len == LXMF_STAMP_LENGTH) {
        if (router->config.ticket_store == NULL) return LXMF_ERR_STAMP;
        status = lxmf_ticket_store_validate_inbound(
            router->config.ticket_store, message->source,
            router_wall_time(router), message->message_id, message->stamp);
    } else if (message->stamp_len == LXMF_POW_STAMP_LENGTH) {
        status = lxmf_pow_stamp_validate(message->message_id, cost,
                                         message->stamp, NULL);
    } else {
        return LXMF_ERR_STAMP;
    }
    if (status == LXMF_ERR_FORMAT || status == LXMF_ERR_PENDING)
        return LXMF_ERR_STAMP;
    return status;
}

static void propagation_clear(lxmf_router_t *router, bool cancel) {
    lxmf_router_propagation_slot_t *slot = &router->propagation;
    if (slot->session != NULL) {
        if (cancel) lxmf_pn_session_cancel(slot->session);
        lxmf_pn_session_destroy(slot->session);
    }
    if (slot->stamp_job != NULL) {
        if (cancel) lxmf_stamp_job_cancel(slot->stamp_job);
        lxmf_stamp_job_destroy(slot->stamp_job);
    }
    if (slot->encrypted != NULL) {
        rns_hal_secure_zero(slot->encrypted, slot->encrypted_capacity);
        free(slot->encrypted);
    }
    memset(slot, 0, sizeof *slot);
}

static void propagation_sync_clear(lxmf_router_t *router, bool cancel) {
    lxmf_router_propagation_sync_slot_t *slot = &router->propagation_sync;
    if (slot->session != NULL) {
        if (cancel) lxmf_pn_session_cancel(slot->session);
        lxmf_pn_session_destroy(slot->session);
        slot->session = NULL;
    }
    slot->status.active = false;
}

static void propagation_sync_reject(
    lxmf_router_propagation_sync_slot_t *slot, lxmf_status_t result) {
    slot->status.rejected++;
    if (slot->first_rejection == LXMF_OK) slot->first_rejection = result;
}

static bool propagation_sync_receive(
    const uint8_t transient_id[LXMF_MESSAGE_ID_LENGTH],
    const uint8_t *message, size_t message_length, void *context) {
    lxmf_router_propagation_sync_slot_t *slot = context;
    lxmf_router_t *router = slot != NULL ? slot->router : NULL;
    uint8_t digest[LXMF_MESSAGE_ID_LENGTH];
    uint8_t local_destination[LXMF_DESTINATION_LENGTH];
    static const char *const aspects[] = {"delivery"};
    if (router == NULL || message == NULL ||
        message_length <= LXMF_DESTINATION_LENGTH +
                              RNS_X25519_KEY_SIZE + RNS_TOKEN_OVERHEAD ||
        message_length > LXMF_PN_MAX_WIRE ||
        !rns_sha256(message, message_length, digest) ||
        memcmp(digest, transient_id, sizeof digest) != 0 ||
        !rns_destination_hash(router->config.identity, "lxmf", aspects, 1u,
                              local_destination) ||
        memcmp(message, local_destination, sizeof local_destination) != 0) {
        if (slot != NULL) propagation_sync_reject(slot, LXMF_ERR_FORMAT);
        return false;
    }

    const uint8_t *ratchets = NULL;
    size_t ratchet_count = 0u;
    uint8_t *owned_ratchets = NULL;
    lxmf_status_t status = router_private_ratchets(
        router, NULL, 0u, &ratchets, &ratchet_count, &owned_ratchets);
    if (status != LXMF_OK) {
        propagation_sync_reject(slot, status);
        return false;
    }
    uint8_t *packed = malloc(message_length);
    if (packed == NULL) {
        if (owned_ratchets != NULL) {
            rns_hal_secure_zero(owned_ratchets,
                ratchet_count * RNS_RATCHET_PRIVATE_SIZE);
            free(owned_ratchets);
        }
        propagation_sync_reject(slot, LXMF_ERR_BOUNDS);
        return false;
    }
    memcpy(packed, message, LXMF_DESTINATION_LENGTH);
    size_t plaintext_length = 0u;
    int decrypted = rns_identity_decrypt_with_ratchets(
        router->config.identity, ratchets, ratchet_count, 0,
        message + LXMF_DESTINATION_LENGTH,
        message_length - LXMF_DESTINATION_LENGTH,
        packed + LXMF_DESTINATION_LENGTH,
        message_length - LXMF_DESTINATION_LENGTH, &plaintext_length,
        NULL, NULL);
    if (owned_ratchets != NULL) {
        rns_hal_secure_zero(owned_ratchets,
            ratchet_count * RNS_RATCHET_PRIVATE_SIZE);
        free(owned_ratchets);
    }
    if (!decrypted) {
        rns_hal_secure_zero(packed, message_length);
        free(packed);
        propagation_sync_reject(slot, LXMF_ERR_CRYPTO);
        return false;
    }
    bool inserted = false;
    status = receive_representation(
        router, packed, LXMF_DESTINATION_LENGTH + plaintext_length,
        LXMF_DELIVERY_METHOD_PROPAGATED, false, &inserted);
    rns_hal_secure_zero(packed, message_length);
    free(packed);
    if (status != LXMF_OK) {
        propagation_sync_reject(slot, status);
        return false;
    }
    if (inserted) slot->status.accepted++;
    else slot->status.duplicates++;
    return true;
}

static lxmf_status_t propagation_begin(
    lxmf_router_t *router, const lxmf_store_message_t *stored,
    const uint8_t id[LXMF_MESSAGE_ID_LENGTH], const rns_identity *destination) {
    lxmf_router_propagation_slot_t *slot = &router->propagation;
    if (slot->used) return LXMF_ERR_PENDING;
    size_t packed_size = 0u;
    lxmf_status_t status = lxmf_store_packed_size(router->config.store, id,
                                                  &packed_size);
    if (status == LXMF_ERR_FORMAT) {
        packed_size = stored->content.len + 256u;
        status = LXMF_OK;
    }
    if (status != LXMF_OK || packed_size > LXMF_STORE_MAX_PACKED - 64u)
        return status != LXMF_OK ? status : LXMF_ERR_BOUNDS;
    size_t packed_capacity = packed_size + 64u;
    uint8_t *packed = malloc(packed_capacity);
    if (packed == NULL) return LXMF_ERR_BOUNDS;
    size_t packed_length = 0u;
    status = prepare_outbound_representation(router, stored, id, packed,
        packed_capacity, &packed_length);
    if (status != LXMF_OK || packed_length <= LXMF_DESTINATION_LENGTH) {
        free(packed);
        return status != LXMF_OK ? status : LXMF_ERR_FORMAT;
    }
    size_t cipher_capacity = rns_identity_encrypt_bound(
        packed_length - LXMF_DESTINATION_LENGTH);
    if (cipher_capacity == 0u ||
        cipher_capacity > LXMF_PN_MAX_WIRE - LXMF_DESTINATION_LENGTH -
                              LXMF_POW_STAMP_LENGTH) {
        rns_hal_secure_zero(packed, packed_capacity); free(packed);
        return LXMF_ERR_BOUNDS;
    }
    slot->encrypted_capacity = LXMF_DESTINATION_LENGTH + cipher_capacity +
                               LXMF_POW_STAMP_LENGTH;
    slot->encrypted = malloc(slot->encrypted_capacity);
    if (slot->encrypted == NULL) {
        rns_hal_secure_zero(packed, packed_capacity); free(packed);
        return LXMF_ERR_BOUNDS;
    }
    memcpy(slot->encrypted, packed, LXMF_DESTINATION_LENGTH);
    uint8_t ratchet[RNS_RATCHET_PUBLIC_SIZE];
    const uint8_t *selected_ratchet = NULL;
    if (router->config.resolve_ratchet != NULL &&
        router->config.resolve_ratchet(router->config.ratchet_context,
                                       stored->destination, ratchet))
        selected_ratchet = ratchet;
    size_t cipher_length = 0u;
    int encrypted = rns_identity_encrypt(destination, selected_ratchet,
        packed + LXMF_DESTINATION_LENGTH,
        packed_length - LXMF_DESTINATION_LENGTH,
        slot->encrypted + LXMF_DESTINATION_LENGTH, cipher_capacity,
        &cipher_length);
    rns_hal_secure_zero(ratchet, sizeof ratchet);
    rns_hal_secure_zero(packed, packed_capacity); free(packed);
    if (!encrypted) { propagation_clear(router, false); return LXMF_ERR_CRYPTO; }
    slot->encrypted_length = LXMF_DESTINATION_LENGTH + cipher_length;
    lxmf_sha256(slot->encrypted, slot->encrypted_length, slot->transient_id);
    status = lxmf_stamp_job_create_expanded(slot->transient_id,
        router->config.propagation_stamp_cost,
        LXMF_PROPAGATION_STAMP_WORKBLOCK_ROUNDS, NULL, &slot->stamp_job);
    if (status != LXMF_OK) { propagation_clear(router, false); return status; }
    slot->used = true;
    slot->attempt = stored->delivery.attempts == UINT32_MAX
        ? UINT32_MAX : stored->delivery.attempts + 1u;
    memcpy(slot->message_id, id, sizeof slot->message_id);
    return LXMF_ERR_PENDING;
}

static lxmf_status_t propagation_start_session(lxmf_router_t *router) {
    lxmf_router_propagation_slot_t *slot = &router->propagation;
    uint8_t stamp[LXMF_POW_STAMP_LENGTH];
    lxmf_status_t status = lxmf_stamp_job_result(slot->stamp_job, stamp, NULL);
    if (status != LXMF_OK) return status;
    memcpy(slot->encrypted + slot->encrypted_length, stamp, sizeof stamp);
    lxmf_pn_session_options_t options = {
        .runtime = router->config.runtime,
        .local_identity = router->config.identity,
        .node_identity = router->config.propagation_node_identity,
        .timeout_seconds = router->config.resource_timeout_seconds,
        .max_response_size = LXMF_PN_MAX_WIRE};
    memcpy(options.node_destination,
           router->config.propagation_node_destination, 16u);
    rns_status_t created = lxmf_pn_session_create(&slot->session, &options);
    if (created != RNS_OK) return LXMF_ERR_CRYPTO;
    lxmf_pn_upload_t upload = {.timebase = (double)router_wall_time(router),
                               .count = 1u};
    upload.messages[0] = (lxmf_slice_t){slot->encrypted,
        slot->encrypted_length + sizeof stamp};
    created = lxmf_pn_session_upload(slot->session, &upload,
                                     (double)router_monotonic_ms(router) /
                                         1000.0);
    if (created != RNS_OK) return created == RNS_ERROR_OVERFLOW
        ? LXMF_ERR_BOUNDS : LXMF_ERR_CRYPTO;
    lxmf_stamp_job_destroy(slot->stamp_job); slot->stamp_job = NULL;
    return LXMF_OK;
}

static void propagation_fail(lxmf_router_t *router, lxmf_status_t result) {
    lxmf_router_propagation_slot_t *slot = &router->propagation;
    lxmf_delivery_metadata_t metadata;
    if (lxmf_store_read_delivery(router->config.store, slot->message_id,
                                 &metadata) == LXMF_OK) {
        metadata.attempts = slot->attempt;
        uint32_t limit = router->config.propagation_retry_limit != 0u
            ? router->config.propagation_retry_limit : 5u;
        if (slot->attempt >= limit) {
            metadata.queue_reason = LXMF_QUEUE_REASON_RETRY_EXHAUSTED;
            metadata.retry_at_ms = 0u;
            (void)lxmf_store_update_status(router->config.store,
                                           slot->message_id,
                                           LXMF_DELIVERY_FAILED);
            report(router, slot->message_id, LXMF_DELIVERY_FAILED, result);
            report_event(router, slot->message_id,
                LXMF_DELIVERY_METHOD_PROPAGATED, LXMF_DELIVERY_FAILED,
                LXMF_QUEUE_REASON_RETRY_EXHAUSTED, result, slot->attempt);
        } else {
            uint64_t base = router->config.propagation_retry_base_ms != 0u
                ? router->config.propagation_retry_base_ms : 10000u;
            uint32_t shift = slot->attempt > 7u ? 6u : slot->attempt - 1u;
            uint64_t delay = base > UINT64_MAX >> shift
                ? UINT64_MAX : base << shift;
            uint64_t now = router_monotonic_ms(router);
            metadata.retry_at_ms = delay > UINT64_MAX - now
                ? UINT64_MAX : now + delay;
            metadata.queue_reason = LXMF_QUEUE_REASON_RETRY_BACKOFF;
            (void)lxmf_store_update_status(router->config.store,
                                           slot->message_id,
                                           LXMF_DELIVERY_QUEUED);
            report(router, slot->message_id, LXMF_DELIVERY_QUEUED, result);
            report_event(router, slot->message_id,
                LXMF_DELIVERY_METHOD_PROPAGATED, LXMF_DELIVERY_QUEUED,
                LXMF_QUEUE_REASON_RETRY_BACKOFF, result, slot->attempt);
        }
        (void)lxmf_store_update_delivery(router->config.store,
                                         slot->message_id, &metadata);
    }
    propagation_clear(router, false);
}

static void propagation_poll(lxmf_router_t *router) {
    lxmf_router_propagation_slot_t *slot = &router->propagation;
    if (!slot->used) return;
    lxmf_delivery_metadata_t metadata;
    if (lxmf_store_read_delivery(router->config.store, slot->message_id,
                                 &metadata) != LXMF_OK) {
        propagation_clear(router, true); return;
    }
    if (slot->stamp_job != NULL) {
        uint32_t units = router->config.stamp_work_units != 0u
            ? router->config.stamp_work_units : LXMF_STAMP_POLL_MAX_UNITS;
        lxmf_status_t status = lxmf_stamp_job_poll(slot->stamp_job, units);
        lxmf_stamp_job_progress_t progress;
        (void)lxmf_stamp_job_progress(slot->stamp_job, &progress);
        metadata.queue_reason = LXMF_QUEUE_REASON_STAMP;
        metadata.progress = progress.prepared_rounds * 100000u /
            LXMF_PROPAGATION_STAMP_WORKBLOCK_ROUNDS;
        (void)lxmf_store_update_delivery(router->config.store,
                                         slot->message_id, &metadata);
        if (status == LXMF_OK) {
            status = propagation_start_session(router);
            if (status != LXMF_OK) { propagation_fail(router, status); return; }
            metadata.queue_reason = LXMF_QUEUE_REASON_PROPAGATION_NODE;
            metadata.attempts = slot->attempt;
            (void)lxmf_store_update_delivery(router->config.store,
                                             slot->message_id, &metadata);
        } else if (status != LXMF_ERR_PENDING) {
            propagation_fail(router, status); return;
        }
        return;
    }
    rns_status_t polled = lxmf_pn_session_poll(
        slot->session, (double)router_monotonic_ms(router) / 1000.0);
    const lxmf_pn_session_progress_t *progress =
        lxmf_pn_session_progress(slot->session);
    if (polled != RNS_OK || progress == NULL ||
        progress->state == LXMF_PN_FAILED) {
        propagation_fail(router, polled == RNS_ERROR_TIMEOUT
            ? LXMF_ERR_TIMEOUT : LXMF_ERR_CRYPTO); return;
    }
    if (progress->state == LXMF_PN_CANCELLED) {
        propagation_fail(router, LXMF_ERR_CANCELLED); return;
    }
    if (progress->state == LXMF_PN_UPLOAD) {
        metadata.queue_reason = LXMF_QUEUE_REASON_RESOURCE;
        metadata.actual_method = LXMF_DELIVERY_METHOD_PROPAGATED;
        uint64_t scaled = progress->total_parts != 0u
            ? (uint64_t)progress->transferred_parts * 900000u /
                  progress->total_parts : 0u;
        metadata.progress = (uint32_t)(100000u + scaled);
        if (metadata.progress >= LXMF_DELIVERY_PROGRESS_COMPLETE)
            metadata.progress = LXMF_DELIVERY_PROGRESS_COMPLETE - 1u;
        (void)lxmf_store_update_delivery(router->config.store,
                                         slot->message_id, &metadata);
        (void)lxmf_store_update_status(router->config.store, slot->message_id,
                                       LXMF_DELIVERY_SENDING);
        report_event(router, slot->message_id,
            LXMF_DELIVERY_METHOD_PROPAGATED, LXMF_DELIVERY_SENDING,
            LXMF_QUEUE_REASON_RESOURCE, LXMF_OK, slot->attempt);
    } else if (progress->state == LXMF_PN_COMPLETE) {
        metadata.actual_method = LXMF_DELIVERY_METHOD_PROPAGATED;
        metadata.queue_reason = LXMF_QUEUE_REASON_NONE;
        metadata.retry_at_ms = 0u;
        metadata.progress = LXMF_DELIVERY_PROGRESS_COMPLETE;
        metadata.attempts = slot->attempt;
        (void)lxmf_store_update_delivery(router->config.store,
                                         slot->message_id, &metadata);
        (void)lxmf_store_update_status(router->config.store, slot->message_id,
                                       LXMF_DELIVERY_SENT);
        report(router, slot->message_id, LXMF_DELIVERY_SENT, LXMF_OK);
        report_event(router, slot->message_id,
            LXMF_DELIVERY_METHOD_PROPAGATED, LXMF_DELIVERY_SENT,
            LXMF_QUEUE_REASON_NONE, LXMF_OK, slot->attempt);
        propagation_clear(router, false);
    }
}

static lxmf_status_t propagation_sync_result(
    rns_status_t transport, uint8_t remote_error) {
    if (remote_error == LXMF_PN_ERROR_NO_ACCESS) return LXMF_ERR_BLOCKED;
    if (remote_error == LXMF_PN_ERROR_INVALID_STAMP) return LXMF_ERR_STAMP;
    if (transport == RNS_ERROR_TIMEOUT) return LXMF_ERR_TIMEOUT;
    if (transport == RNS_ERROR_CRYPTO) return LXMF_ERR_CRYPTO;
    if (transport == RNS_ERROR_OVERFLOW || transport == RNS_ERROR_NO_MEMORY)
        return LXMF_ERR_BOUNDS;
    return transport == RNS_OK ? LXMF_OK : LXMF_ERR_FORMAT;
}

static void propagation_sync_poll(lxmf_router_t *router) {
    lxmf_router_propagation_sync_slot_t *slot = &router->propagation_sync;
    if (!slot->status.active || slot->session == NULL) return;
    rns_status_t polled = lxmf_pn_session_poll(
        slot->session, (double)router_monotonic_ms(router) / 1000.0);
    const lxmf_pn_session_progress_t *progress =
        lxmf_pn_session_progress(slot->session);
    if (progress == NULL) {
        slot->status.state = LXMF_PN_FAILED;
        slot->status.transport_error = RNS_ERROR_INVALID_STATE;
        slot->status.result = LXMF_ERR_FORMAT;
        propagation_sync_clear(router, false);
        return;
    }
    slot->status.state = progress->state;
    slot->status.transport_error = progress->error;
    slot->status.remote_error = progress->remote_error;
    slot->status.available = progress->available;
    slot->status.received = progress->received;
    slot->status.acknowledged = progress->acknowledged;
    if (progress->state == LXMF_PN_COMPLETE) {
        slot->status.result = slot->first_rejection != LXMF_OK
            ? slot->first_rejection : LXMF_OK;
        propagation_sync_clear(router, false);
    } else if (progress->state == LXMF_PN_CANCELLED) {
        slot->status.result = LXMF_ERR_CANCELLED;
        propagation_sync_clear(router, false);
    } else if (progress->state == LXMF_PN_FAILED || polled != RNS_OK) {
        rns_status_t failure = polled != RNS_OK ? polled : progress->error;
        slot->status.transport_error = failure;
        slot->status.result = propagation_sync_result(
            failure, progress->remote_error);
        propagation_sync_clear(router, false);
    }
}

static void remember_verified_ticket(lxmf_router_t *router,
                                     const lxmf_message_t *message) {
    if (router->config.ticket_store == NULL) return;
    lxmf_ticket_field_t field;
    if (lxmf_fields_parse_ticket(message->fields_msgpack.data,
                                 message->fields_msgpack.len,
                                 &field) != LXMF_OK || !field.present)
        return;
    uint64_t now = router_wall_time(router);
    if (field.expires_at <= now) return;
    lxmf_ticket_entry_t entry = {.expires_at = field.expires_at};
    memcpy(entry.ticket, field.ticket, sizeof entry.ticket);
    /* Ticket persistence must not turn an otherwise valid incoming message
     * into a delivery failure. The sender can advertise it again later. */
    (void)lxmf_ticket_store_remember_outbound(
        router->config.ticket_store, message->source, &entry, now);
}

typedef struct {
    lxmf_router_t *router;
    lxmf_status_t status;
} recovery_context_t;

static bool recover_interrupted(void *context,
                                const lxmf_store_message_t *message) {
    recovery_context_t *recovery = context;
    bool interrupted = message->status == LXMF_DELIVERY_SENDING ||
                       (message->status == LXMF_DELIVERY_SENT &&
                        message->delivery.has_proof_id);
    /* Monotonic deadlines cannot be compared across a machine restart. Rebase
     * durable backoff records as well as interrupted transfers so a clock-epoch
     * change cannot strand a queued message indefinitely or retry it in a hot
     * loop. */
    bool stale_backoff = message->status == LXMF_DELIVERY_QUEUED &&
                         message->delivery.queue_reason ==
                             LXMF_QUEUE_REASON_RETRY_BACKOFF &&
                         message->delivery.retry_at_ms != 0U;
    if (!interrupted && !stale_backoff) return true;
    lxmf_delivery_metadata_t delivery = message->delivery;
    uint32_t limit = delivery.desired_method == LXMF_DELIVERY_METHOD_PROPAGATED
                         ? (recovery->router->config.propagation_retry_limit != 0U
                                ? recovery->router->config.propagation_retry_limit
                                : 5U)
                         : (recovery->router->config.direct_retry_limit != 0U
                                ? recovery->router->config.direct_retry_limit
                                : 5U);
    if (delivery.attempts >= limit) {
        delivery.queue_reason = LXMF_QUEUE_REASON_RETRY_EXHAUSTED;
        delivery.retry_at_ms = 0U;
    } else {
        uint64_t base = delivery.desired_method == LXMF_DELIVERY_METHOD_PROPAGATED
                            ? recovery->router->config.propagation_retry_base_ms
                            : recovery->router->config.direct_retry_base_ms;
        if (base == 0U) base = 10000U;
        uint32_t shift = delivery.attempts > 7U
                             ? 6U
                             : (delivery.attempts == 0U
                                    ? 0U : delivery.attempts - 1U);
        uint64_t delay = base > (UINT64_MAX >> shift)
                             ? UINT64_MAX : base << shift;
        uint64_t now = router_monotonic_ms(recovery->router);
        delivery.queue_reason = LXMF_QUEUE_REASON_RETRY_BACKOFF;
        delivery.retry_at_ms = delay > UINT64_MAX - now
                                   ? UINT64_MAX : now + delay;
    }
    delivery.progress = 0u;
    delivery.has_proof_id = false;
    memset(delivery.proof_id, 0, sizeof delivery.proof_id);
    recovery->status = lxmf_store_update_delivery(
        recovery->router->config.store, message->message_id, &delivery);
    if (recovery->status == LXMF_OK)
        recovery->status = lxmf_store_update_status(
            recovery->router->config.store, message->message_id,
            delivery.queue_reason == LXMF_QUEUE_REASON_RETRY_EXHAUSTED
                ? LXMF_DELIVERY_FAILED : LXMF_DELIVERY_QUEUED);
    return recovery->status == LXMF_OK;
}

static lxmf_status_t recover_interrupted_deliveries(lxmf_router_t *router) {
    recovery_context_t recovery = {.router = router, .status = LXMF_OK};
    lxmf_status_t status = lxmf_store_list(router->config.store, recover_interrupted,
                                           &recovery);
    return status == LXMF_OK ? recovery.status : status;
}

const char *lxmf_delivery_method_string(lxmf_delivery_method_t method) {
    switch (method) {
        case LXMF_DELIVERY_METHOD_UNKNOWN: return "unknown";
        case LXMF_DELIVERY_METHOD_DIRECT: return "direct";
        case LXMF_DELIVERY_METHOD_OPPORTUNISTIC: return "opportunistic";
        case LXMF_DELIVERY_METHOD_PROPAGATED: return "propagated";
        default: return "invalid";
    }
}

const char *lxmf_queue_reason_string(lxmf_queue_reason_t reason) {
    switch (reason) {
        case LXMF_QUEUE_REASON_NONE: return "none";
        case LXMF_QUEUE_REASON_PEER_IDENTITY: return "peer identity";
        case LXMF_QUEUE_REASON_PATH: return "path";
        case LXMF_QUEUE_REASON_STAMP: return "stamp";
        case LXMF_QUEUE_REASON_LINK: return "link";
        case LXMF_QUEUE_REASON_RESOURCE: return "resource";
        case LXMF_QUEUE_REASON_PROPAGATION_NODE: return "propagation node";
        case LXMF_QUEUE_REASON_RETRY_BACKOFF: return "retry backoff";
        case LXMF_QUEUE_REASON_RETRY_EXHAUSTED: return "retry exhausted";
        case LXMF_QUEUE_REASON_CANCELLED: return "cancelled";
        default: return "invalid";
    }
}

lxmf_status_t lxmf_router_init(lxmf_router_t *router,
                               const lxmf_router_config_t *config) {
    if (router == NULL || config == NULL || config->identity == NULL ||
        !config->identity->has_private || config->store == NULL ||
        config->resolve_identity == NULL ||
        config->preferred_delivery_method < LXMF_DELIVERY_METHOD_UNKNOWN ||
        config->preferred_delivery_method > LXMF_DELIVERY_METHOD_PROPAGATED ||
        (config->runtime == NULL && config->send_packet == NULL) ||
        (config->ticket_store != NULL && config->wall_clock == NULL) ||
        config->inbound_stamp_cost == UINT8_MAX ||
        config->max_incoming_message_size > LXMF_STORE_MAX_PACKED ||
        config->stamp_work_units > LXMF_STAMP_POLL_MAX_UNITS ||
        config->direct_retry_limit > LXMF_ROUTER_DIRECT_MAX_RETRIES ||
        config->direct_retry_base_ms > LXMF_ROUTER_DIRECT_MAX_RETRY_BASE_MS ||
        (config->propagation_node_identity != NULL &&
         (config->runtime == NULL || config->wall_clock == NULL ||
          config->propagation_stamp_cost == 0u ||
          config->propagation_stamp_cost == UINT8_MAX ||
          !isfinite(config->resource_timeout_seconds) ||
          config->resource_timeout_seconds < 0.0 ||
          config->propagation_retry_limit >
              LXMF_ROUTER_PROPAGATION_MAX_RETRIES ||
          config->propagation_retry_base_ms >
              LXMF_ROUTER_PROPAGATION_MAX_RETRY_BASE_MS)))
        return LXMF_ERR_ARGUMENT;
    memset(router, 0, sizeof *router);
    router->config = *config;
    if (config->propagation_node_identity != NULL) {
        uint8_t public_key[RNS_IDENTITY_PUBLIC_SIZE], expected[16];
        const char *const aspects[] = {"propagation"};
        rns_identity_export_public(config->propagation_node_identity, public_key);
        if (!rns_identity_from_public(&router->propagation_node, public_key) ||
            !rns_destination_hash(&router->propagation_node, "lxmf", aspects,
                                  1u, expected) ||
            memcmp(expected, config->propagation_node_destination, 16u) != 0) {
            memset(router, 0, sizeof *router);
            return LXMF_ERR_ARGUMENT;
        }
        router->config.propagation_node_identity = &router->propagation_node;
    }
    if (recover_interrupted_deliveries(router) != LXMF_OK) {
        memset(router, 0, sizeof *router);
        return LXMF_ERR_CRYPTO;
    }
    if (config->runtime != NULL && config->accept_inbound_links) {
        static const char *const aspects[] = {"delivery"};
        uint8_t hash[LXMF_DESTINATION_LENGTH];
        rns_runtime_link_options_t options = {
            .prove_data_packets = false,
            .state_callback = direct_link_state_changed,
            .packet_callback = direct_link_packet_received,
            .identified_callback = direct_link_identified,
            .resource_accept_callback = direct_resource_accept,
            .resource_receive_callback = direct_resource_received,
            .max_incoming_resource_size =
                config->max_incoming_resource_size != 0U
                    ? config->max_incoming_resource_size
                    : LXMF_STORE_MAX_PACKED,
            .callback_context = router};
        if (!rns_destination_hash(config->identity, "lxmf", aspects, 1U,
                                  hash) ||
            rns_runtime_register_link_destination(
                config->runtime, hash, config->identity, &options,
                direct_link_accepted, router,
                &router->inbound_destination) != RNS_OK) {
            memset(router, 0, sizeof *router);
            return LXMF_ERR_CRYPTO;
        }
    }
    return LXMF_OK;
}

static void release_receipts(lxmf_router_t *router, bool all) {
    for (size_t i = 0u; i < LXMF_ROUTER_MAX_RECEIPTS; ++i) {
        lxmf_router_receipt_slot_t *slot = &router->receipts[i];
        if (!slot->used || (!all && !slot->terminal)) continue;
        /* Router teardown is not a user cancellation. Leave the durable
         * SENT/SENDING state intact so the next router instance can recover
         * it with a bounded retry. */
        rns_packet_receipt_destroy(slot->receipt);
        memset(slot, 0, sizeof *slot);
    }
}

static void release_resources(lxmf_router_t *router, bool all) {
    for (size_t i = 0U; i < LXMF_ROUTER_MAX_RESOURCES; ++i) {
        lxmf_router_resource_slot_t *slot = &router->resources[i];
        if (!slot->used || (!all && !slot->terminal)) continue;
        /* Destroy detaches the transfer without invoking its cancellation
         * callback. Explicit user cancellation uses the public cancel path. */
        rns_runtime_resource_transfer_destroy(slot->transfer);
        memset(slot, 0, sizeof *slot);
    }
}

void lxmf_router_destroy(lxmf_router_t *router) {
    if (router == NULL) return;
    propagation_clear(router, true);
    propagation_sync_clear(router, true);
    lxmf_stamp_job_destroy(router->stamp_job);
    release_receipts(router, true);
    release_resources(router, true);
    for (size_t i = 0U; i < LXMF_ROUTER_MAX_LINKS; ++i) {
        if (router->links[i].used && router->links[i].link != NULL)
            rns_runtime_link_destroy(router->links[i].link);
    }
    rns_runtime_destination_destroy(router->inbound_destination);
    memset(router, 0, sizeof *router);
}

static lxmf_router_receipt_slot_t *reserve_receipt(lxmf_router_t *router) {
    release_receipts(router, false);
    for (size_t i = 0u; i < LXMF_ROUTER_MAX_RECEIPTS; ++i) {
        if (router->receipts[i].used) continue;
        router->receipts[i].used = true;
        router->receipts[i].router = router;
        return &router->receipts[i];
    }
    return NULL;
}

static lxmf_router_resource_slot_t *reserve_resource(lxmf_router_t *router) {
    release_resources(router, false);
    for (size_t i = 0U; i < LXMF_ROUTER_MAX_RESOURCES; ++i) {
        if (router->resources[i].used) continue;
        router->resources[i].used = true;
        router->resources[i].router = router;
        return &router->resources[i];
    }
    return NULL;
}

static void receipt_changed(rns_packet_receipt_t *receipt,
                            rns_packet_receipt_state_t state,
                            rns_status_t status, void *context) {
    lxmf_router_receipt_slot_t *slot = context;
    lxmf_router_t *router;
    lxmf_delivery_status_t delivery;
    lxmf_status_t result;
    if (slot == NULL || !slot->used || slot->receipt != receipt) return;
    router = slot->router;
    if (state == RNS_PACKET_RECEIPT_PENDING) return;
    slot->terminal = true;
    if (state == RNS_PACKET_RECEIPT_DELIVERED) {
        delivery = LXMF_DELIVERY_DELIVERED;
        result = LXMF_OK;
        if (slot->method == LXMF_DELIVERY_METHOD_DIRECT && slot->link != NULL)
            (void)rns_runtime_link_identify(slot->link,
                                            router->config.identity);
    } else if (state == RNS_PACKET_RECEIPT_CANCELLED) {
        finish_direct_attempt(router, slot->message_id, slot->method,
                              slot->attempt, LXMF_ERR_CANCELLED, true, false);
        return;
    } else {
        result = status == RNS_ERROR_TIMEOUT ? LXMF_ERR_TIMEOUT : LXMF_ERR_CRYPTO;
        finish_direct_attempt(router, slot->message_id, slot->method,
                              slot->attempt, result, false, true);
        return;
    }
    lxmf_delivery_metadata_t metadata;
    if (lxmf_store_read_delivery(router->config.store, slot->message_id,
                                 &metadata) == LXMF_OK) {
        metadata.actual_method = slot->method;
        metadata.queue_reason = delivery == LXMF_DELIVERY_DELIVERED
                                    ? LXMF_QUEUE_REASON_NONE
                                : state == RNS_PACKET_RECEIPT_CANCELLED
                                    ? LXMF_QUEUE_REASON_CANCELLED
                                    : LXMF_QUEUE_REASON_RETRY_BACKOFF;
        metadata.progress = delivery == LXMF_DELIVERY_DELIVERED
                                ? LXMF_DELIVERY_PROGRESS_COMPLETE
                                : metadata.progress;
        (void)lxmf_store_update_delivery(router->config.store,
                                         slot->message_id, &metadata);
    }
    (void)lxmf_store_update_status(router->config.store, slot->message_id,
                                   delivery);
    report(router, slot->message_id, delivery, result);
    report_event(router, slot->message_id,
                 slot->method, delivery,
                 delivery == LXMF_DELIVERY_DELIVERED
                     ? LXMF_QUEUE_REASON_NONE
                 : state == RNS_PACKET_RECEIPT_CANCELLED
                     ? LXMF_QUEUE_REASON_CANCELLED
                     : LXMF_QUEUE_REASON_RETRY_BACKOFF,
                 result, slot->attempt);
}

static lxmf_status_t resource_result(rns_runtime_resource_state_t state,
                                     rns_status_t status) {
    if (state == RNS_RUNTIME_RESOURCE_CANCELLED) return LXMF_ERR_CANCELLED;
    if (status == RNS_ERROR_TIMEOUT) return LXMF_ERR_TIMEOUT;
    return LXMF_ERR_CRYPTO;
}

static void resource_changed(rns_runtime_resource_transfer_t *transfer,
                             rns_runtime_resource_state_t state,
                             rns_status_t status, size_t sent_parts,
                             size_t total_parts, void *context) {
    lxmf_router_resource_slot_t *slot = context;
    if (slot == NULL || !slot->used || slot->transfer != transfer) return;
    lxmf_router_t *router = slot->router;
    bool terminal = state != RNS_RUNTIME_RESOURCE_ADVERTISED &&
                    state != RNS_RUNTIME_RESOURCE_TRANSFERRING;
    if (terminal) slot->terminal = true;
    lxmf_delivery_metadata_t metadata;
    if (lxmf_store_read_delivery(router->config.store, slot->message_id,
                                 &metadata) != LXMF_OK)
        return;
    if (state == RNS_RUNTIME_RESOURCE_TRANSFERRING) {
        rns_runtime_resource_progress_t progress;
        uint64_t scaled = 0U;
        if (rns_runtime_resource_transfer_progress(transfer, &progress) ==
                RNS_OK && progress.total != 0U)
            scaled = progress.transferred * 900000U / progress.total;
        else if (total_parts != 0U)
            scaled = (uint64_t)sent_parts * 900000U / total_parts;
        metadata.progress = (uint32_t)(100000U + scaled);
        if (metadata.progress >= LXMF_DELIVERY_PROGRESS_COMPLETE)
            metadata.progress = LXMF_DELIVERY_PROGRESS_COMPLETE - 1U;
        metadata.queue_reason = LXMF_QUEUE_REASON_RESOURCE;
        (void)lxmf_store_update_delivery(router->config.store,
                                         slot->message_id, &metadata);
        report_event(router, slot->message_id, LXMF_DELIVERY_METHOD_DIRECT,
                     LXMF_DELIVERY_SENDING, LXMF_QUEUE_REASON_RESOURCE,
                     LXMF_OK, slot->attempt);
        return;
    }
    if (state == RNS_RUNTIME_RESOURCE_ADVERTISED) return;
    if (state == RNS_RUNTIME_RESOURCE_COMPLETE) {
        metadata.actual_method = LXMF_DELIVERY_METHOD_DIRECT;
        metadata.queue_reason = LXMF_QUEUE_REASON_NONE;
        metadata.progress = LXMF_DELIVERY_PROGRESS_COMPLETE;
        metadata.has_proof_id = true;
        memcpy(metadata.proof_id,
               rns_runtime_resource_transfer_hash(transfer),
               sizeof metadata.proof_id);
        (void)lxmf_store_update_delivery(router->config.store,
                                         slot->message_id, &metadata);
        /* Pinned LXMF maps a Resource COMPLETE callback (which follows its
         * completion proof) to DELIVERED. Persist SENT first so it never
         * denotes a merely advertised or partially transferred resource. */
        (void)lxmf_store_update_status(router->config.store, slot->message_id,
                                       LXMF_DELIVERY_SENT);
        report(router, slot->message_id, LXMF_DELIVERY_SENT, LXMF_OK);
        report_event(router, slot->message_id, LXMF_DELIVERY_METHOD_DIRECT,
                     LXMF_DELIVERY_SENT, LXMF_QUEUE_REASON_NONE, LXMF_OK,
                     slot->attempt);
        (void)lxmf_store_update_status(router->config.store, slot->message_id,
                                       LXMF_DELIVERY_DELIVERED);
        report(router, slot->message_id, LXMF_DELIVERY_DELIVERED, LXMF_OK);
        report_event(router, slot->message_id, LXMF_DELIVERY_METHOD_DIRECT,
                     LXMF_DELIVERY_DELIVERED, LXMF_QUEUE_REASON_NONE, LXMF_OK,
                     slot->attempt);
        if (slot->link != NULL)
            (void)rns_runtime_link_identify(slot->link,
                                            router->config.identity);
    } else {
        lxmf_status_t result = resource_result(state, status);
        finish_direct_attempt(router, slot->message_id,
                              LXMF_DELIVERY_METHOD_DIRECT, slot->attempt,
                              result,
                              state == RNS_RUNTIME_RESOURCE_CANCELLED,
                              state != RNS_RUNTIME_RESOURCE_REJECTED);
    }
}

static lxmf_status_t send_with_receipt(
    lxmf_router_t *router, const uint8_t id[LXMF_MESSAGE_ID_LENGTH],
    const uint8_t destination_hash[LXMF_DESTINATION_LENGTH],
    const rns_identity *destination, const uint8_t *packet,
    size_t packet_length, uint32_t attempt,
    lxmf_queue_reason_t *queue_reason,
    uint8_t proof_id[LXMF_MESSAGE_ID_LENGTH]) {
    *queue_reason = LXMF_QUEUE_REASON_RETRY_BACKOFF;
    lxmf_router_receipt_slot_t *slot = reserve_receipt(router);
    if (slot == NULL) return LXMF_ERR_PENDING;
    memcpy(slot->message_id, id, sizeof slot->message_id);
    slot->method = LXMF_DELIVERY_METHOD_OPPORTUNISTIC;
    slot->attempt = attempt;
    rns_packet_receipt_options_t options = {
        .timeout_seconds = 30.0,
        .callback = receipt_changed,
        .callback_context = slot
    };
    rns_status_t sent = rns_runtime_send_routed_with_receipt(
        router->config.runtime, packet, packet_length, destination, &options,
        &slot->receipt);
    if (sent != RNS_OK) {
        memset(slot, 0, sizeof *slot);
        if (sent == RNS_ERROR_NOT_FOUND) {
            *queue_reason = LXMF_QUEUE_REASON_PATH;
            (void)rns_runtime_request_path(router->config.runtime,
                                           destination_hash);
            return LXMF_ERR_PENDING;
        }
        return LXMF_ERR_CRYPTO;
    }
    memcpy(proof_id, rns_packet_receipt_hash(slot->receipt),
           LXMF_MESSAGE_ID_LENGTH);
    return LXMF_OK;
}

static lxmf_router_link_slot_t *find_direct_link(
    lxmf_router_t *router,
    const uint8_t destination[LXMF_DESTINATION_LENGTH]) {
    for (size_t i = 0U; i < LXMF_ROUTER_MAX_LINKS; ++i)
        if (router->links[i].used && !router->links[i].inbound &&
            memcmp(router->links[i].destination, destination,
                   LXMF_DESTINATION_LENGTH) == 0)
            return &router->links[i];
    return NULL;
}

static lxmf_router_link_slot_t *reserve_direct_link(lxmf_router_t *router) {
    for (size_t i = 0U; i < LXMF_ROUTER_MAX_LINKS; ++i)
        if (!router->links[i].used) {
            router->links[i].used = true;
            router->links[i].router = router;
            return &router->links[i];
        }
    return NULL;
}

static void direct_link_state_changed(rns_runtime_link_t *link,
                                      rns_link_state state,
                                      rns_status_t reason, void *context) {
    lxmf_router_t *router = context;
    (void)reason;
    if (router == NULL || link == NULL || state != RNS_LINK_CLOSED) return;
    /* Destruction is deferred to router polling; runtime callbacks are not a
     * safe place to release the currently dispatching link. */
}

static void direct_link_packet_received(rns_runtime_link_t *link,
                                        uint8_t packet_context,
                                        const uint8_t *plaintext,
                                        size_t plaintext_length,
                                        void *context) {
    lxmf_router_t *router = context;
    if (router == NULL || packet_context != 0U) return;
    lxmf_status_t status = receive_representation(
        router, plaintext, plaintext_length, LXMF_DELIVERY_METHOD_DIRECT,
        false, NULL);
    if (status == LXMF_OK)
        (void)rns_runtime_link_prove_current_packet(link);
}

static bool direct_resource_accept(
    rns_runtime_link_t *link,
    const rns_resource_advertisement_t *advertisement, void *context) {
    lxmf_router_t *router = context;
    size_t maximum;
    (void)link;
    if (router == NULL || advertisement == NULL) return false;
    maximum = router->config.max_incoming_resource_size != 0U
                  ? router->config.max_incoming_resource_size
                  : LXMF_STORE_MAX_PACKED;
    return advertisement->data_size > 0U &&
           advertisement->data_size <= maximum &&
           advertisement->data_size <= inbound_message_limit(router);
}

static void direct_resource_received(
    rns_runtime_link_t *link, const uint8_t resource_hash[32],
    rns_status_t status, const uint8_t *data, size_t data_length,
    void *context) {
    lxmf_router_t *router = context;
    (void)link;
    (void)resource_hash;
    if (router == NULL || status != RNS_OK || data == NULL ||
        data_length == 0U)
        return;
    (void)receive_representation(router, data, data_length,
                                 LXMF_DELIVERY_METHOD_DIRECT, false, NULL);
}

static void direct_link_accepted(rns_runtime_destination_t *destination,
                                 rns_runtime_link_t *link, void *context) {
    lxmf_router_t *router = context;
    (void)destination;
    if (router == NULL || link == NULL) return;
    lxmf_router_link_slot_t *slot = reserve_direct_link(router);
    if (slot == NULL) return;
    slot->inbound = true;
    slot->link = link;
}

static void direct_link_identified(rns_runtime_link_t *link,
                                   const rns_identity *identity,
                                   void *context) {
    lxmf_router_t *router = context;
    static const char *const aspects[] = {"delivery"};
    uint8_t destination[LXMF_DESTINATION_LENGTH];
    if (router == NULL || link == NULL || identity == NULL ||
        !rns_destination_hash(identity, "lxmf", aspects, 1U, destination))
        return;
    for (size_t i = 0U; i < LXMF_ROUTER_MAX_LINKS; ++i) {
        lxmf_router_link_slot_t *slot = &router->links[i];
        if (!slot->used || slot->link != link) continue;
        memcpy(slot->destination, destination, sizeof slot->destination);
        slot->inbound = false;
        return;
    }
}

static lxmf_status_t ensure_direct_link(
    lxmf_router_t *router,
    const uint8_t destination_hash[LXMF_DESTINATION_LENGTH],
    const rns_identity *destination, rns_runtime_link_t **active) {
    *active = NULL;
    lxmf_router_link_slot_t *slot = find_direct_link(router, destination_hash);
    if (slot != NULL) {
        rns_link_state state = rns_runtime_link_state(slot->link);
        if (state == RNS_LINK_ACTIVE) {
            *active = slot->link;
            return LXMF_OK;
        }
        if (state != RNS_LINK_CLOSED) return LXMF_ERR_PENDING;
        rns_runtime_link_destroy(slot->link);
        memset(slot, 0, sizeof *slot);
    }
    if (rns_runtime_path_lookup(router->config.runtime, destination_hash,
                                &(rns_path_entry){0}) != RNS_OK) {
        (void)rns_runtime_request_path(router->config.runtime,
                                       destination_hash);
        return LXMF_ERR_PENDING;
    }
    slot = reserve_direct_link(router);
    if (slot == NULL) return LXMF_ERR_PENDING;
    memcpy(slot->destination, destination_hash, LXMF_DESTINATION_LENGTH);
    rns_runtime_link_options_t options = {
        .prove_data_packets = false,
        .state_callback = direct_link_state_changed,
        .packet_callback = direct_link_packet_received,
        .identified_callback = direct_link_identified,
        .resource_accept_callback = direct_resource_accept,
        .resource_receive_callback = direct_resource_received,
        .max_incoming_resource_size =
            router->config.max_incoming_resource_size != 0U
                ? router->config.max_incoming_resource_size
                : LXMF_STORE_MAX_PACKED,
        .callback_context = router};
    rns_status_t status = rns_runtime_link_open(
        router->config.runtime, destination_hash, destination, &options,
        &slot->link);
    if (status != RNS_OK) {
        memset(slot, 0, sizeof *slot);
        if (status == RNS_ERROR_NOT_FOUND)
            (void)rns_runtime_request_path(router->config.runtime,
                                           destination_hash);
        return status == RNS_ERROR_NOT_FOUND ? LXMF_ERR_PENDING
                                             : LXMF_ERR_CRYPTO;
    }
    return LXMF_ERR_PENDING;
}

lxmf_status_t lxmf_router_stamp_progress(
    const lxmf_router_t *router, const uint8_t id[32],
    lxmf_stamp_job_progress_t *progress) {
    if (router == NULL || id == NULL || progress == NULL)
        return LXMF_ERR_ARGUMENT;
    if (router->stamp_job == NULL ||
        memcmp(router->stamp_message_id, id, 32u) != 0)
        return LXMF_ERR_PENDING;
    return lxmf_stamp_job_progress(router->stamp_job, progress);
}

/* Stamp work runs before SENDING and does not consume network retry attempts.
 * The worker keeps only an ID and hash state, never pointers into the store. */
static lxmf_status_t prepare_outbound_stamp(
    lxmf_router_t *router, lxmf_store_message_t *stored, const uint8_t id[32]) {
    uint8_t cost = 0u;
    if (router->config.resolve_stamp_cost == NULL ||
        !router->config.resolve_stamp_cost(router->config.stamp_cost_context,
                                           stored->destination, &cost) ||
        cost == 0u) {
        if (router->stamp_job != NULL &&
            memcmp(router->stamp_message_id, id, 32u) == 0) {
            lxmf_stamp_job_destroy(router->stamp_job);
            router->stamp_job = NULL;
        }
        return LXMF_OK;
    }
    if (cost == UINT8_MAX) return LXMF_ERR_ARGUMENT;
    lxmf_stamp_job_progress_t progress = {0};
    if (router->stamp_job != NULL) {
        lxmf_delivery_metadata_t active;
        if (lxmf_store_read_delivery(router->config.store,
            router->stamp_message_id, &active) != LXMF_OK ||
            active.queue_reason == LXMF_QUEUE_REASON_CANCELLED) {
            lxmf_stamp_job_destroy(router->stamp_job);
            router->stamp_job = NULL;
        }
    }
    if (router->stamp_job != NULL) {
        (void)lxmf_stamp_job_progress(router->stamp_job, &progress);
        bool same = memcmp(router->stamp_message_id, id, 32u) == 0;
        if (!same && progress.state != LXMF_STAMP_COMPLETE &&
            progress.state != LXMF_STAMP_FAILED &&
            progress.state != LXMF_STAMP_CANCELLED)
            return LXMF_ERR_PENDING;
        if (!same || router->stamp_cost != cost) {
            lxmf_stamp_job_destroy(router->stamp_job);
            router->stamp_job = NULL;
        }
    }
    size_t retained_length = 0u;
    lxmf_status_t status = lxmf_store_packed_size(router->config.store, id,
                                                 &retained_length);
    /* Legacy content-only journals cannot be safely stamped under the old ID. */
    if (status != LXMF_OK) return status;
    uint8_t *retained = malloc(retained_length);
    if (retained == NULL) return LXMF_ERR_BOUNDS;
    status = lxmf_store_read_packed(router->config.store, id, retained,
                                    retained_length, &retained_length);
    lxmf_message_t message;
    if (status == LXMF_OK)
        status = lxmf_unpack(retained, retained_length, NULL, NULL, &message);
    static const char *const aspects[] = {"delivery"};
    uint8_t local_hash[16];
    if (status == LXMF_OK &&
        (!rns_destination_hash(router->config.identity, "lxmf", aspects, 1u,
                               local_hash) ||
         memcmp(message.message_id, id, 32u) != 0 ||
         memcmp(message.destination, stored->destination, 16u) != 0 ||
         memcmp(message.source, stored->source, 16u) != 0 ||
         memcmp(message.source, local_hash, 16u) != 0))
        status = LXMF_ERR_FORMAT;
    if (status != LXMF_OK) { free(retained); return status; }

    uint8_t stamp[LXMF_POW_STAMP_LENGTH];
    size_t stamp_length = LXMF_POW_STAMP_LENGTH;
    bool ticket = false;
    if (router->config.ticket_store != NULL) {
        status = lxmf_ticket_store_stamp_outbound(router->config.ticket_store,
            stored->destination, router_wall_time(router), id, stamp);
        ticket = status == LXMF_OK;
        if (status != LXMF_OK && status != LXMF_ERR_PENDING) {
            free(retained);
            return status;
        }
    }
    if (ticket) {
        stamp_length = LXMF_STAMP_LENGTH;
        /* A newly learned ticket supersedes unfinished expensive work. */
        if (router->stamp_job != NULL) {
            lxmf_stamp_job_destroy(router->stamp_job);
            router->stamp_job = NULL;
        }
    } else {
        if (router->stamp_job == NULL) {
            const uint8_t *nonce = message.has_stamp &&
                message.stamp_len == LXMF_POW_STAMP_LENGTH
                    ? message.stamp : NULL;
            status = lxmf_stamp_job_create(id, cost, nonce,
                                            &router->stamp_job);
            if (status != LXMF_OK) { free(retained); return status; }
            memcpy(router->stamp_message_id, id, 32u);
            router->stamp_cost = cost;
        }
        uint32_t units = router->config.stamp_work_units != 0u
            ? router->config.stamp_work_units : LXMF_STAMP_POLL_MAX_UNITS;
        status = lxmf_stamp_job_poll(router->stamp_job, units);
        (void)lxmf_stamp_job_progress(router->stamp_job, &progress);
        /* Preparation has a known bound; nonce search deliberately has no
         * fabricated percentage. Detailed attempts are exposed separately. */
        stored->delivery.progress = progress.prepared_rounds * 500000u /
                                    LXMF_STAMP_WORKBLOCK_ROUNDS;
        if (status != LXMF_OK && status != LXMF_ERR_PENDING) {
            free(retained);
            return status;
        }
        status = lxmf_stamp_job_result(router->stamp_job, stamp, NULL);
        if (status != LXMF_OK) { free(retained); return status; }
    }
    if (message.has_stamp && message.stamp_len == stamp_length &&
        memcmp(message.stamp, stamp, stamp_length) == 0) {
        free(retained);
        return LXMF_OK;
    }
    message.has_stamp = true;
    message.stamp_len = stamp_length;
    memcpy(message.stamp, stamp, stamp_length);
    size_t capacity = lxmf_pack_bound(&message);
    if (capacity > LXMF_STORE_MAX_PACKED) capacity = LXMF_STORE_MAX_PACKED;
    uint8_t *packed = malloc(capacity);
    if (packed == NULL) { free(retained); return LXMF_ERR_BOUNDS; }
    size_t packed_length = 0u;
    status = lxmf_pack(&message, lxmf_identity_signer, router->config.identity,
                       packed, capacity, &packed_length);
    if (status == LXMF_OK)
        status = lxmf_store_update_packed(router->config.store, id, packed,
                                          packed_length);
    free(packed);
    free(retained);
    return status;
}

static lxmf_status_t prepare_outbound_representation(
    lxmf_router_t *router, const lxmf_store_message_t *stored,
    const uint8_t id[LXMF_MESSAGE_ID_LENGTH], uint8_t *packed,
    size_t packed_capacity, size_t *packed_length) {
    uint8_t *retained = NULL;
    size_t retained_length = 0u;
    lxmf_status_t status = lxmf_store_packed_size(
        router->config.store, id, &retained_length);
    if (status == LXMF_OK) {
        if (retained_length > packed_capacity) return LXMF_ERR_BOUNDS;
        retained = malloc(retained_length);
        if (retained == NULL) return LXMF_ERR_BOUNDS;
        status = lxmf_store_read_packed(router->config.store, id, retained,
                                         retained_length, &retained_length);
    }
    lxmf_message_t message = {0};
    bool has_retained_wire = status == LXMF_OK;
    if (status == LXMF_OK) {
        status = lxmf_unpack(retained, retained_length, NULL, NULL, &message);
        if (status == LXMF_OK &&
            (memcmp(message.message_id, id, LXMF_MESSAGE_ID_LENGTH) != 0 ||
             memcmp(message.destination, stored->destination,
                    LXMF_DESTINATION_LENGTH) != 0 ||
             memcmp(message.source, stored->source,
                    LXMF_SOURCE_LENGTH) != 0))
            status = LXMF_ERR_FORMAT;
    } else if (status == LXMF_ERR_FORMAT) {
        /* Compatibility with stores written before outbound representations
         * were retained. */
        message.content = stored->content;
        memcpy(message.destination, stored->destination,
               LXMF_DESTINATION_LENGTH);
        memcpy(message.source, stored->source, LXMF_SOURCE_LENGTH);
        message.timestamp = stored->timestamp;
        status = LXMF_OK;
    }
    if (status != LXMF_OK) { free(retained); return status; }

    bool added_stamp = false;
    if (has_retained_wire && !message.has_stamp &&
        router->config.ticket_store != NULL) {
        status = lxmf_ticket_store_stamp_outbound(
            router->config.ticket_store, message.destination,
            router_wall_time(router), id, message.stamp);
        if (status == LXMF_OK) {
            message.has_stamp = true;
            message.stamp_len = LXMF_STAMP_LENGTH;
            added_stamp = true;
        } else if (status != LXMF_ERR_PENDING) {
            free(retained);
            return status;
        }
    }
    if (has_retained_wire && !added_stamp) {
        memcpy(packed, retained, retained_length);
        *packed_length = retained_length;
        free(retained);
        return LXMF_OK;
    }
    status = lxmf_pack(&message, lxmf_identity_signer,
                       router->config.identity, packed, packed_capacity,
                       packed_length);
    if (status == LXMF_OK) {
        lxmf_message_t check;
        status = lxmf_unpack(packed, *packed_length, NULL, NULL, &check);
        if (status == LXMF_OK && has_retained_wire &&
            memcmp(check.message_id, id, LXMF_MESSAGE_ID_LENGTH) != 0)
            status = LXMF_ERR_FORMAT;
    }
    free(retained);
    return status;
}

static lxmf_status_t send_direct(
    lxmf_router_t *router, const lxmf_store_message_t *stored,
    const uint8_t id[LXMF_MESSAGE_ID_LENGTH], const rns_identity *destination,
    uint32_t attempt, lxmf_queue_reason_t *queue_reason,
    uint8_t proof_id[LXMF_MESSAGE_ID_LENGTH], bool *resource_started) {
    *resource_started = false;
    rns_runtime_link_t *link = NULL;
    lxmf_status_t status = ensure_direct_link(
        router, stored->destination, destination, &link);
    if (status != LXMF_OK) {
        *queue_reason = rns_runtime_path_lookup(
                            router->config.runtime, stored->destination,
                            &(rns_path_entry){0}) == RNS_OK
                            ? LXMF_QUEUE_REASON_LINK
                            : LXMF_QUEUE_REASON_PATH;
        return status;
    }
    size_t packed_capacity = 0U;
    status = lxmf_store_packed_size(router->config.store, id, &packed_capacity);
    if (status == LXMF_ERR_FORMAT)
        packed_capacity = stored->content.len + 256U;
    else if (status != LXMF_OK)
        return status;
    /* Optional ticket stamps add a bounded suffix. */
    if (packed_capacity <= LXMF_STORE_MAX_PACKED - 64U)
        packed_capacity += 64U;
    else
        packed_capacity = LXMF_STORE_MAX_PACKED;
    uint8_t *packed = malloc(packed_capacity);
    if (packed == NULL) return LXMF_ERR_BOUNDS;
    size_t packed_length = 0U;
    status = prepare_outbound_representation(
        router, stored, id, packed, packed_capacity, &packed_length);
    if (status != LXMF_OK) {
        free(packed);
        *queue_reason = LXMF_QUEUE_REASON_RESOURCE;
        return status == LXMF_ERR_BOUNDS ? LXMF_ERR_PENDING : status;
    }
    lxmf_router_receipt_slot_t *receipt_slot = reserve_receipt(router);
    if (receipt_slot == NULL) {
        free(packed);
        *queue_reason = LXMF_QUEUE_REASON_RETRY_BACKOFF;
        return LXMF_ERR_PENDING;
    }
    memcpy(receipt_slot->message_id, id, LXMF_MESSAGE_ID_LENGTH);
    receipt_slot->method = LXMF_DELIVERY_METHOD_DIRECT;
    receipt_slot->link = link;
    receipt_slot->attempt = attempt;
    rns_packet_receipt_options_t options = {
        .timeout_seconds = 30.0,
        .callback = receipt_changed,
        .callback_context = receipt_slot};
    rns_status_t sent = rns_runtime_link_send_with_receipt(
        link, 0U, packed, packed_length, &options, &receipt_slot->receipt);
    if (sent == RNS_ERROR_OVERFLOW) {
        memset(receipt_slot, 0, sizeof *receipt_slot);
        lxmf_router_resource_slot_t *resource_slot = reserve_resource(router);
        if (resource_slot == NULL) {
            free(packed);
            *queue_reason = LXMF_QUEUE_REASON_RESOURCE;
            return LXMF_ERR_PENDING;
        }
        memcpy(resource_slot->message_id, id, LXMF_MESSAGE_ID_LENGTH);
        resource_slot->link = link;
        resource_slot->attempt = attempt;
        rns_runtime_resource_options_t resource_options = {
            .timeout_seconds = router->config.resource_timeout_seconds,
            .auto_compress = true,
            .callback = resource_changed,
            .callback_context = resource_slot};
        sent = rns_runtime_link_send_resource(
            link, packed, packed_length, &resource_options,
            &resource_slot->transfer);
        free(packed);
        if (sent != RNS_OK) {
            memset(resource_slot, 0, sizeof *resource_slot);
            *queue_reason = sent == RNS_ERROR_OVERFLOW
                                ? LXMF_QUEUE_REASON_RESOURCE
                                : LXMF_QUEUE_REASON_LINK;
            return sent == RNS_ERROR_OVERFLOW ? LXMF_ERR_PENDING
                                              : LXMF_ERR_CRYPTO;
        }
        memcpy(proof_id,
               rns_runtime_resource_transfer_hash(resource_slot->transfer),
               LXMF_MESSAGE_ID_LENGTH);
        *queue_reason = LXMF_QUEUE_REASON_RESOURCE;
        *resource_started = true;
        return LXMF_OK;
    }
    free(packed);
    if (sent != RNS_OK) {
        memset(receipt_slot, 0, sizeof *receipt_slot);
        *queue_reason = sent == RNS_ERROR_OVERFLOW
                            ? LXMF_QUEUE_REASON_RESOURCE
                            : LXMF_QUEUE_REASON_LINK;
        return sent == RNS_ERROR_OVERFLOW ? LXMF_ERR_PENDING : LXMF_ERR_CRYPTO;
    }
    memcpy(proof_id, rns_packet_receipt_hash(receipt_slot->receipt),
           LXMF_MESSAGE_ID_LENGTH);
    return LXMF_OK;
}

lxmf_status_t lxmf_router_send_message(
    lxmf_router_t *router, const uint8_t id[LXMF_MESSAGE_ID_LENGTH]) {
    if (router == NULL || id == NULL) return LXMF_ERR_ARGUMENT;
    uint8_t content[LXMF_STORE_MAX_CONTENT], packet[RNS_MTU];
    lxmf_store_message_t stored;
    if (lxmf_store_read(router->config.store, id, &stored, content,
                        sizeof content) != LXMF_OK)
        return LXMF_ERR_FORMAT;
    if (stored.status != LXMF_DELIVERY_QUEUED &&
        stored.status != LXMF_DELIVERY_FAILED)
        return LXMF_ERR_ARGUMENT;
    lxmf_delivery_method_t method = stored.delivery.desired_method;
    if (method == LXMF_DELIVERY_METHOD_UNKNOWN) {
        method = router->config.preferred_delivery_method;
        if (method == LXMF_DELIVERY_METHOD_UNKNOWN)
            method = LXMF_DELIVERY_METHOD_OPPORTUNISTIC;
    }
    if (stored.delivery.queue_reason == LXMF_QUEUE_REASON_CANCELLED ||
        stored.delivery.queue_reason == LXMF_QUEUE_REASON_RETRY_EXHAUSTED) {
        /* This call is an explicit resend: terminal records are excluded from
         * automatic polling, so begin a fresh bounded attempt budget before
         * method-specific availability checks alter the actionable reason. */
        stored.delivery.attempts = 0U;
        stored.delivery.retry_at_ms = 0U;
    }
    if (method == LXMF_DELIVERY_METHOD_PROPAGATED) {
        if (router->propagation.used || router->propagation_sync.status.active) {
            stored.delivery.queue_reason = LXMF_QUEUE_REASON_PROPAGATION_NODE;
            (void)lxmf_store_update_delivery(router->config.store, id,
                                             &stored.delivery);
            return LXMF_ERR_PENDING;
        }
        uint64_t now = router_monotonic_ms(router);
        if (stored.delivery.queue_reason == LXMF_QUEUE_REASON_RETRY_BACKOFF &&
            stored.delivery.retry_at_ms != 0u && now < stored.delivery.retry_at_ms)
            return LXMF_ERR_PENDING;
        if (router->config.propagation_node_identity == NULL) {
            stored.delivery.queue_reason = LXMF_QUEUE_REASON_PROPAGATION_NODE;
            (void)lxmf_store_update_delivery(router->config.store, id,
                                             &stored.delivery);
            report_event(router, id, method, LXMF_DELIVERY_QUEUED,
                LXMF_QUEUE_REASON_PROPAGATION_NODE, LXMF_ERR_PENDING,
                stored.delivery.attempts);
            return LXMF_ERR_PENDING;
        }
    }
    if (stored.delivery.queue_reason == LXMF_QUEUE_REASON_RETRY_BACKOFF &&
        stored.delivery.retry_at_ms != 0U &&
        router_monotonic_ms(router) < stored.delivery.retry_at_ms)
        return LXMF_ERR_PENDING;
    stored.delivery.desired_method = method;
    stored.delivery.actual_method = LXMF_DELIVERY_METHOD_UNKNOWN;
    stored.delivery.queue_reason = LXMF_QUEUE_REASON_NONE;
    stored.delivery.retry_at_ms = 0u;
    stored.delivery.progress = 0u;
    stored.delivery.has_proof_id = false;
    memset(stored.delivery.proof_id, 0, sizeof stored.delivery.proof_id);
    if (lxmf_store_update_delivery(router->config.store, id,
                                   &stored.delivery) != LXMF_OK)
        return LXMF_ERR_CRYPTO;
    const rns_identity *destination = router->config.resolve_identity(
        router->config.resolve_context, stored.destination);
    /* A missing announce is a normal discovery state, not a delivery failure. */
    if (destination == NULL) {
        stored.delivery.queue_reason = LXMF_QUEUE_REASON_PEER_IDENTITY;
        if (lxmf_store_update_delivery(router->config.store, id,
                                       &stored.delivery) != LXMF_OK)
            return LXMF_ERR_CRYPTO;
        report_event(router, id, method,
                     LXMF_DELIVERY_QUEUED, LXMF_QUEUE_REASON_PEER_IDENTITY,
                     LXMF_ERR_PENDING, stored.delivery.attempts);
        return LXMF_ERR_PENDING;
    }
    lxmf_status_t stamp_status = prepare_outbound_stamp(router, &stored, id);
    if (stamp_status != LXMF_OK) {
        stored.delivery.queue_reason = LXMF_QUEUE_REASON_STAMP;
        lxmf_delivery_status_t state = stamp_status == LXMF_ERR_PENDING
            ? LXMF_DELIVERY_QUEUED : LXMF_DELIVERY_FAILED;
        if (lxmf_store_update_delivery(router->config.store, id,
                                       &stored.delivery) != LXMF_OK ||
            lxmf_store_update_status(router->config.store, id, state) != LXMF_OK)
            return LXMF_ERR_CRYPTO;
        report_event(router, id, method, state, LXMF_QUEUE_REASON_STAMP,
                     stamp_status, stored.delivery.attempts);
        return stamp_status;
    }
    if (method == LXMF_DELIVERY_METHOD_PROPAGATED) {
        lxmf_status_t begun = propagation_begin(router, &stored, id,
                                                destination);
        stored.delivery.queue_reason = begun == LXMF_ERR_PENDING
            ? LXMF_QUEUE_REASON_STAMP : LXMF_QUEUE_REASON_RETRY_BACKOFF;
        if (lxmf_store_update_delivery(router->config.store, id,
                                       &stored.delivery) != LXMF_OK)
            return LXMF_ERR_CRYPTO;
        report_event(router, id, method, LXMF_DELIVERY_QUEUED,
            stored.delivery.queue_reason, begun, stored.delivery.attempts);
        return begun;
    }
    stored.delivery.progress = 0u;
    stored.delivery.actual_method = method;
    uint32_t previous_attempts = stored.delivery.attempts;
    uint32_t attempt = previous_attempts == UINT32_MAX
                           ? UINT32_MAX
                           : previous_attempts + 1u;
    /* Persist the attempt before any packet or Resource can leave the
     * process. A crash may conservatively consume one budget unit, but can no
     * longer repeat an on-wire attempt without accounting for it. */
    stored.delivery.attempts = attempt;
    if (lxmf_store_update_delivery(router->config.store, id,
                                   &stored.delivery) != LXMF_OK)
        return LXMF_ERR_CRYPTO;
    if (lxmf_store_update_status(router->config.store, id,
                                 LXMF_DELIVERY_SENDING) != LXMF_OK)
        return LXMF_ERR_CRYPTO;
    report(router, id, LXMF_DELIVERY_SENDING, LXMF_OK);
    report_event(router, id, method,
                 LXMF_DELIVERY_SENDING, LXMF_QUEUE_REASON_NONE, LXMF_OK,
                 attempt);
    size_t packet_length = 0;
    uint8_t proof_id[LXMF_MESSAGE_ID_LENGTH] = {0};
    bool has_proof_id = false;
    bool resource_started = false;
    lxmf_queue_reason_t pending_reason = LXMF_QUEUE_REASON_NONE;
    lxmf_status_t status;
    if (method == LXMF_DELIVERY_METHOD_DIRECT) {
        if (router->config.runtime == NULL)
            status = LXMF_ERR_ARGUMENT;
        else
            status = send_direct(router, &stored, id, destination, attempt,
                                 &pending_reason, proof_id,
                                 &resource_started);
        if (status == LXMF_OK) has_proof_id = true;
    } else {
        /* Opportunistic delivery is intentionally constrained to one packet. */
        uint8_t representation[RNS_MTU];
        size_t representation_length = 0u;
        lxmf_message_t message;
        status = prepare_outbound_representation(
            router, &stored, id, representation, sizeof representation,
            &representation_length);
        if (status == LXMF_OK)
            status = lxmf_unpack(representation, representation_length,
                                 NULL, NULL, &message);
        if (status == LXMF_OK) {
            uint8_t ratchet[RNS_RATCHET_PUBLIC_SIZE];
            const uint8_t *selected_ratchet = NULL;
            if (router->config.resolve_ratchet != NULL &&
                router->config.resolve_ratchet(
                    router->config.ratchet_context, stored.destination,
                    ratchet))
                selected_ratchet = ratchet;
            status = lxmf_opportunistic_packet_pack_ratchet(
                &message, router->config.identity, destination,
                selected_ratchet, packet, sizeof packet, &packet_length);
            rns_hal_secure_zero(ratchet, sizeof ratchet);
        }
        if (status == LXMF_OK) {
            if (router->config.runtime != NULL)
                status = send_with_receipt(router, id, stored.destination,
                                           destination, packet, packet_length,
                                           attempt,
                                           &pending_reason, proof_id);
            else
                status = router->config.send_packet(
                    router->config.send_context, packet, packet_length);
            if (status == LXMF_OK && router->config.runtime != NULL)
                has_proof_id = true;
        }
    }
    if (status == LXMF_ERR_PENDING) {
        stored.delivery.attempts = previous_attempts;
        stored.delivery.queue_reason = pending_reason;
        if (lxmf_store_update_delivery(router->config.store, id,
                                       &stored.delivery) != LXMF_OK)
            return LXMF_ERR_CRYPTO;
        (void)lxmf_store_update_status(router->config.store, id,
                                       LXMF_DELIVERY_QUEUED);
        report(router, id, LXMF_DELIVERY_QUEUED, status);
        report_event(router, id, method,
                     LXMF_DELIVERY_QUEUED,
                     pending_reason, status, stored.delivery.attempts);
        return status;
    }
    if (status == LXMF_OK) {
        stored.delivery.attempts = attempt;
        stored.delivery.queue_reason = resource_started
                                           ? LXMF_QUEUE_REASON_RESOURCE
                                           : LXMF_QUEUE_REASON_NONE;
        stored.delivery.progress = resource_started
                                       ? 100000U
                                       : LXMF_DELIVERY_PROGRESS_COMPLETE;
        stored.delivery.has_proof_id = has_proof_id;
        if (has_proof_id)
            memcpy(stored.delivery.proof_id, proof_id,
                   sizeof stored.delivery.proof_id);
        if (lxmf_store_update_delivery(router->config.store, id,
                                       &stored.delivery) != LXMF_OK)
            status = LXMF_ERR_CRYPTO;
    }
    if (status == LXMF_OK && !resource_started &&
        lxmf_store_update_status(router->config.store, id,
                                 LXMF_DELIVERY_SENT) != LXMF_OK)
        status = LXMF_ERR_CRYPTO;
    if (status == LXMF_OK) {
        if (!resource_started) {
            report(router, id, LXMF_DELIVERY_SENT, LXMF_OK);
            report_event(router, id, method,
                         LXMF_DELIVERY_SENT, LXMF_QUEUE_REASON_NONE, LXMF_OK,
                         stored.delivery.attempts);
        } else {
            report_event(router, id, method, LXMF_DELIVERY_SENDING,
                         LXMF_QUEUE_REASON_RESOURCE, LXMF_OK,
                         stored.delivery.attempts);
        }
    } else {
        stored.delivery.attempts = attempt;
        (void)lxmf_store_update_delivery(router->config.store, id,
                                         &stored.delivery);
        finish_direct_attempt(router, id, method, attempt, status, false, true);
    }
    return status;
}

lxmf_status_t lxmf_router_cancel_message(
    lxmf_router_t *router,
    const uint8_t id[LXMF_MESSAGE_ID_LENGTH]) {
    if (router == NULL || id == NULL) return LXMF_ERR_ARGUMENT;
    if (router->propagation.used &&
        memcmp(router->propagation.message_id, id, 32u) == 0) {
        lxmf_delivery_metadata_t metadata;
        if (lxmf_store_read_delivery(router->config.store, id, &metadata) !=
            LXMF_OK) return LXMF_ERR_FORMAT;
        metadata.queue_reason = LXMF_QUEUE_REASON_CANCELLED;
        metadata.retry_at_ms = 0u; metadata.progress = 0u;
        if (lxmf_store_update_delivery(router->config.store, id, &metadata) !=
                LXMF_OK ||
            lxmf_store_update_status(router->config.store, id,
                                     LXMF_DELIVERY_FAILED) != LXMF_OK)
            return LXMF_ERR_CRYPTO;
        propagation_clear(router, true);
        report_event(router, id, LXMF_DELIVERY_METHOD_PROPAGATED,
            LXMF_DELIVERY_FAILED, LXMF_QUEUE_REASON_CANCELLED,
            LXMF_ERR_CANCELLED, metadata.attempts);
        return LXMF_OK;
    }
    lxmf_delivery_metadata_t delivery;
    if (lxmf_store_read_delivery(router->config.store, id, &delivery) == LXMF_OK &&
        delivery.queue_reason == LXMF_QUEUE_REASON_STAMP) {
        delivery.queue_reason = LXMF_QUEUE_REASON_CANCELLED;
        delivery.progress = 0u;
        if (lxmf_store_update_delivery(router->config.store, id, &delivery) !=
                LXMF_OK ||
            lxmf_store_update_status(router->config.store, id,
                                     LXMF_DELIVERY_FAILED) != LXMF_OK)
            return LXMF_ERR_CRYPTO;
        if (router->stamp_job != NULL &&
            memcmp(router->stamp_message_id, id, 32u) == 0) {
            lxmf_stamp_job_cancel(router->stamp_job);
            lxmf_stamp_job_destroy(router->stamp_job);
            router->stamp_job = NULL;
        }
        report_event(router, id, delivery.desired_method, LXMF_DELIVERY_FAILED,
                     LXMF_QUEUE_REASON_CANCELLED, LXMF_ERR_CANCELLED,
                     delivery.attempts);
        return LXMF_OK;
    }
    uint8_t content[LXMF_STORE_MAX_CONTENT];
    lxmf_store_message_t stored;
    if (lxmf_store_read(router->config.store, id, &stored, content,
                        sizeof content) != LXMF_OK)
        return LXMF_ERR_FORMAT;
    if (stored.status != LXMF_DELIVERY_QUEUED &&
        stored.status != LXMF_DELIVERY_FAILED &&
        stored.status != LXMF_DELIVERY_SENDING &&
        !(stored.status == LXMF_DELIVERY_SENT &&
          stored.delivery.has_proof_id))
        return LXMF_ERR_ARGUMENT;
    delivery = stored.delivery;
    delivery.queue_reason = LXMF_QUEUE_REASON_CANCELLED;
    delivery.retry_at_ms = 0U;
    delivery.progress = 0U;
    delivery.has_proof_id = false;
    memset(delivery.proof_id, 0, sizeof delivery.proof_id);
    if (lxmf_store_update_delivery(router->config.store, id, &delivery) !=
            LXMF_OK ||
        lxmf_store_update_status(router->config.store, id,
                                 LXMF_DELIVERY_FAILED) != LXMF_OK)
        return LXMF_ERR_CRYPTO;
    for (size_t i = 0U; i < LXMF_ROUTER_MAX_RESOURCES; ++i) {
        lxmf_router_resource_slot_t *slot = &router->resources[i];
        if (!slot->used || slot->terminal ||
            memcmp(slot->message_id, id, LXMF_MESSAGE_ID_LENGTH) != 0)
            continue;
        rns_runtime_resource_transfer_cancel(slot->transfer);
        return LXMF_OK;
    }
    for (size_t i = 0U; i < LXMF_ROUTER_MAX_RECEIPTS; ++i) {
        lxmf_router_receipt_slot_t *slot = &router->receipts[i];
        if (!slot->used || slot->terminal ||
            memcmp(slot->message_id, id, LXMF_MESSAGE_ID_LENGTH) != 0)
            continue;
        rns_packet_receipt_cancel(slot->receipt);
        return LXMF_OK;
    }
    report(router, id, LXMF_DELIVERY_FAILED, LXMF_ERR_CANCELLED);
    report_event(router, id, delivery.desired_method, LXMF_DELIVERY_FAILED,
                 LXMF_QUEUE_REASON_CANCELLED, LXMF_ERR_CANCELLED,
                 delivery.attempts);
    return LXMF_OK;
}

typedef struct {
    uint8_t ids[LXMF_STORE_MAX_MESSAGES][LXMF_MESSAGE_ID_LENGTH];
    size_t count;
    size_t limit;
} pending_messages_t;

static bool collect_pending(void *context, const lxmf_store_message_t *message) {
    pending_messages_t *pending = context;
    if (message->delivery.queue_reason != LXMF_QUEUE_REASON_CANCELLED &&
        message->delivery.queue_reason != LXMF_QUEUE_REASON_RETRY_EXHAUSTED &&
        (message->status == LXMF_DELIVERY_QUEUED ||
         message->status == LXMF_DELIVERY_FAILED) && pending->count < pending->limit)
        memcpy(pending->ids[pending->count++], message->message_id,
               LXMF_MESSAGE_ID_LENGTH);
    return pending->count < pending->limit;
}

lxmf_status_t lxmf_router_poll(lxmf_router_t *router, size_t max_messages,
                               lxmf_router_poll_result_t *result) {
    if (router == NULL || result == NULL || max_messages > LXMF_STORE_MAX_MESSAGES)
        return LXMF_ERR_ARGUMENT;
    memset(result, 0, sizeof *result);
    release_receipts(router, false);
    release_resources(router, false);
    propagation_poll(router);
    propagation_sync_poll(router);
    for (size_t i = 0U; i < LXMF_ROUTER_MAX_LINKS; ++i) {
        lxmf_router_link_slot_t *slot = &router->links[i];
        if (!slot->used || slot->link == NULL ||
            rns_runtime_link_state(slot->link) != RNS_LINK_CLOSED)
            continue;
        rns_runtime_link_destroy(slot->link);
        memset(slot, 0, sizeof *slot);
    }
    if (max_messages == 0u) return LXMF_OK;
    pending_messages_t pending = {.limit = max_messages};
    lxmf_status_t status = lxmf_store_list(router->config.store, collect_pending,
                                           &pending);
    if (status != LXMF_OK) return status;
    for (size_t i = 0; i < pending.count; i++) {
        status = lxmf_router_send_message(router, pending.ids[i]);
        result->attempted++;
        if (status == LXMF_OK) result->sent++;
        else if (status == LXMF_ERR_PENDING) result->deferred++;
        else result->failed++;
    }
    return LXMF_OK;
}

lxmf_status_t lxmf_router_set_inbound_stamp_cost(lxmf_router_t *router,
                                                  uint8_t cost) {
    if (router == NULL || cost == UINT8_MAX) return LXMF_ERR_ARGUMENT;
    router->config.inbound_stamp_cost = cost;
    return LXMF_OK;
}

lxmf_status_t lxmf_router_set_propagation_node(
    lxmf_router_t *router, const rns_identity *identity,
    const uint8_t destination[LXMF_DESTINATION_LENGTH], uint8_t stamp_cost) {
    if (router == NULL) return LXMF_ERR_ARGUMENT;
    if (identity == NULL) {
        if (destination != NULL || stamp_cost != 0u) return LXMF_ERR_ARGUMENT;
        if (router->propagation.used || router->propagation_sync.status.active)
            return LXMF_ERR_PENDING;
        memset(&router->propagation_node, 0, sizeof router->propagation_node);
        router->config.propagation_node_identity = NULL;
        memset(router->config.propagation_node_destination, 0,
               sizeof router->config.propagation_node_destination);
        router->config.propagation_stamp_cost = 0u;
        return LXMF_OK;
    }
    if (destination == NULL || router->config.runtime == NULL ||
        router->config.wall_clock == NULL ||
        stamp_cost == 0u || stamp_cost == UINT8_MAX)
        return LXMF_ERR_ARGUMENT;
    uint8_t expected[LXMF_DESTINATION_LENGTH];
    const char *const aspects[] = {"propagation"};
    if (!rns_destination_hash(identity, "lxmf", aspects, 1u, expected) ||
        memcmp(expected, destination, sizeof expected) != 0)
        return LXMF_ERR_ARGUMENT;
    if (router->propagation.used || router->propagation_sync.status.active) {
        if (router->config.propagation_node_identity != NULL &&
            router->config.propagation_stamp_cost == stamp_cost &&
            memcmp(router->config.propagation_node_destination, destination,
                   sizeof expected) == 0)
            return LXMF_OK;
        return LXMF_ERR_PENDING;
    }
    uint8_t public_key[RNS_IDENTITY_PUBLIC_SIZE];
    rns_identity candidate;
    rns_identity_export_public(identity, public_key);
    if (!rns_identity_from_public(&candidate, public_key))
        return LXMF_ERR_ARGUMENT;
    router->propagation_node = candidate;
    router->config.propagation_node_identity = &router->propagation_node;
    memcpy(router->config.propagation_node_destination, destination,
           sizeof router->config.propagation_node_destination);
    router->config.propagation_stamp_cost = stamp_cost;
    return LXMF_OK;
}

lxmf_status_t lxmf_router_propagation_sync_start(lxmf_router_t *router,
                                                  bool retain_on_node) {
    if (router == NULL || router->config.runtime == NULL ||
        router->config.propagation_node_identity == NULL)
        return LXMF_ERR_ARGUMENT;
    lxmf_router_propagation_sync_slot_t *slot = &router->propagation_sync;
    if (slot->status.active || router->propagation.used)
        return LXMF_ERR_PENDING;
    if (slot->session != NULL) propagation_sync_clear(router, false);
    memset(slot, 0, sizeof *slot);
    slot->router = router;
    slot->first_rejection = LXMF_OK;
    slot->status.state = LXMF_PN_IDLE;
    slot->status.result = LXMF_ERR_PENDING;
    slot->status.transport_error = RNS_OK;
    slot->status.retain_on_node = retain_on_node;
    lxmf_pn_session_options_t options = {
        .runtime = router->config.runtime,
        .local_identity = router->config.identity,
        .node_identity = router->config.propagation_node_identity,
        .timeout_seconds = router->config.resource_timeout_seconds,
        .max_response_size = LXMF_PN_MAX_WIRE,
        .retain_on_node = retain_on_node,
        .message_callback = propagation_sync_receive,
        .callback_context = slot};
    memcpy(options.node_destination,
           router->config.propagation_node_destination,
           sizeof options.node_destination);
    rns_status_t created = lxmf_pn_session_create(&slot->session, &options);
    if (created != RNS_OK) {
        slot->status.result = propagation_sync_result(created, 0u);
        slot->status.transport_error = created;
        slot->status.state = LXMF_PN_FAILED;
        return slot->status.result;
    }
    created = lxmf_pn_session_sync(
        slot->session, (double)router_monotonic_ms(router) / 1000.0);
    if (created != RNS_OK) {
        lxmf_pn_session_destroy(slot->session);
        slot->session = NULL;
        slot->status.result = propagation_sync_result(created, 0u);
        slot->status.transport_error = created;
        slot->status.state = LXMF_PN_FAILED;
        return slot->status.result;
    }
    const lxmf_pn_session_progress_t *progress =
        lxmf_pn_session_progress(slot->session);
    slot->status.state = progress != NULL ? progress->state : LXMF_PN_PATH;
    slot->status.active = true;
    return LXMF_OK;
}

lxmf_status_t lxmf_router_propagation_sync_cancel(lxmf_router_t *router) {
    if (router == NULL) return LXMF_ERR_ARGUMENT;
    lxmf_router_propagation_sync_slot_t *slot = &router->propagation_sync;
    if (!slot->status.active || slot->session == NULL) return LXMF_ERR_FORMAT;
    lxmf_pn_session_cancel(slot->session);
    const lxmf_pn_session_progress_t *progress =
        lxmf_pn_session_progress(slot->session);
    slot->status.state = progress != NULL
        ? progress->state : LXMF_PN_CANCELLED;
    slot->status.transport_error = progress != NULL
        ? progress->error : RNS_OK;
    slot->status.result = LXMF_ERR_CANCELLED;
    propagation_sync_clear(router, false);
    return LXMF_OK;
}

lxmf_status_t lxmf_router_propagation_sync_status(
    const lxmf_router_t *router,
    lxmf_router_propagation_sync_status_t *status) {
    if (router == NULL || status == NULL) return LXMF_ERR_ARGUMENT;
    *status = router->propagation_sync.status;
    return LXMF_OK;
}

lxmf_status_t lxmf_router_receive_packet(lxmf_router_t *router,
                                         const uint8_t *packet,
                                         size_t packet_length) {
    if (router == NULL || packet == NULL || packet_length == 0u ||
        packet_length > RNS_MTU)
        return LXMF_ERR_ARGUMENT;
    uint8_t plaintext[RNS_MTU];
    size_t plaintext_length = 0;
    lxmf_message_t message;
    lxmf_identity_verifier_context_t verifier = {
        .resolve = router->config.resolve_identity,
        .resolve_context = router->config.resolve_context};
    uint8_t *private_ratchets = NULL;
    size_t ratchet_count = 0u;
    if (router->config.ratchet_store != NULL) {
        ratchet_count = rns_ratchet_store_count(router->config.ratchet_store);
        if (ratchet_count != 0u) {
            if (ratchet_count > RNS_RATCHET_STORE_MAX_RETAINED ||
                ratchet_count > SIZE_MAX / RNS_RATCHET_PRIVATE_SIZE)
                return LXMF_ERR_BOUNDS;
            private_ratchets = malloc(
                ratchet_count * RNS_RATCHET_PRIVATE_SIZE);
            if (private_ratchets == NULL) return LXMF_ERR_BOUNDS;
            size_t copied = 0u;
            if (rns_ratchet_store_copy_private(
                    router->config.ratchet_store, private_ratchets,
                    ratchet_count, &copied) != RNS_OK ||
                copied != ratchet_count) {
                rns_hal_secure_zero(
                    private_ratchets,
                    ratchet_count * RNS_RATCHET_PRIVATE_SIZE);
                free(private_ratchets);
                return LXMF_ERR_CRYPTO;
            }
        }
    }
    lxmf_status_t status = lxmf_opportunistic_packet_unpack_ratchets(
        packet, packet_length, router->config.identity, private_ratchets,
        ratchet_count, 0, lxmf_identity_verifier, &verifier, plaintext,
        sizeof plaintext, &plaintext_length, &message, NULL, NULL);
    if (private_ratchets != NULL) {
        rns_hal_secure_zero(private_ratchets,
                            ratchet_count * RNS_RATCHET_PRIVATE_SIZE);
        free(private_ratchets);
    }
    /* An identity we do not hold yet cannot condemn the message: retain it
     * flagged so it can be shown and checked again after its announce. A
     * signature that fails against an identity we do hold is forged. */
    bool unverified = status == LXMF_ERR_UNKNOWN_SIGNER;
    if (status != LXMF_OK && !unverified) return status;
    if (unverified && plaintext_length > LXMF_STORE_MAX_PACKED)
        return LXMF_ERR_BOUNDS;

    return receive_representation(router, plaintext, plaintext_length,
                                  LXMF_DELIVERY_METHOD_OPPORTUNISTIC, false,
                                  NULL);
}

static bool paper_transient_seen(
    const lxmf_router_t *router,
    const uint8_t transient_id[LXMF_MESSAGE_ID_LENGTH],
    uint8_t message_id[LXMF_MESSAGE_ID_LENGTH]) {
    for (size_t i = 0u; i < router->paper_transient_count; ++i)
        if (memcmp(router->paper_transient_ids[i], transient_id,
                   LXMF_MESSAGE_ID_LENGTH) == 0) {
            memcpy(message_id, router->paper_message_ids[i],
                   LXMF_MESSAGE_ID_LENGTH);
            return true;
        }
    return false;
}

static void remember_paper_transient(
    lxmf_router_t *router,
    const uint8_t transient_id[LXMF_MESSAGE_ID_LENGTH],
    const uint8_t message_id[LXMF_MESSAGE_ID_LENGTH]) {
    size_t slot;
    if (router->paper_transient_count < LXMF_ROUTER_MAX_PAPER_TRANSIENTS) {
        slot = router->paper_transient_count++;
    } else {
        slot = router->next_paper_transient;
        router->next_paper_transient =
            (router->next_paper_transient + 1u) %
            LXMF_ROUTER_MAX_PAPER_TRANSIENTS;
    }
    memcpy(router->paper_transient_ids[slot], transient_id,
           LXMF_MESSAGE_ID_LENGTH);
    memcpy(router->paper_message_ids[slot], message_id,
           LXMF_MESSAGE_ID_LENGTH);
}

static lxmf_status_t router_private_ratchets(
    const lxmf_router_t *router, const uint8_t *supplied, size_t supplied_count,
    const uint8_t **effective, size_t *effective_count, uint8_t **owned) {
    *effective = supplied;
    *effective_count = supplied_count;
    *owned = NULL;
    if (supplied_count > RNS_RATCHET_STORE_MAX_RETAINED ||
        (supplied_count != 0u && supplied == NULL))
        return LXMF_ERR_ARGUMENT;
    /* A non-NULL pointer with a zero count explicitly selects an empty
     * history. NULL selects the configured durable history, when present. */
    if (supplied != NULL || router->config.ratchet_store == NULL)
        return LXMF_OK;
    size_t count = rns_ratchet_store_count(router->config.ratchet_store);
    if (count == 0u) return LXMF_OK;
    if (count > RNS_RATCHET_STORE_MAX_RETAINED ||
        count > SIZE_MAX / RNS_RATCHET_PRIVATE_SIZE)
        return LXMF_ERR_BOUNDS;
    uint8_t *copy = malloc(count * RNS_RATCHET_PRIVATE_SIZE);
    if (copy == NULL) return LXMF_ERR_BOUNDS;
    size_t copied = 0u;
    if (rns_ratchet_store_copy_private(router->config.ratchet_store, copy,
                                       count, &copied) != RNS_OK ||
        copied != count) {
        rns_hal_secure_zero(copy, count * RNS_RATCHET_PRIVATE_SIZE);
        free(copy);
        return LXMF_ERR_CRYPTO;
    }
    *effective = copy;
    *effective_count = count;
    *owned = copy;
    return LXMF_OK;
}

lxmf_status_t lxmf_router_receive_paper(
    lxmf_router_t *router, const uint8_t *paper, size_t paper_length,
    const uint8_t *ratchet_private_keys, size_t ratchet_count,
    bool enforce_ratchets, lxmf_router_paper_result_t *result) {
    if (router == NULL || router->config.identity == NULL ||
        router->config.store == NULL || paper == NULL || result == NULL)
        return LXMF_ERR_ARGUMENT;
    memset(result, 0, sizeof *result);
    if (paper_length <= LXMF_DESTINATION_LENGTH ||
        paper_length > LXMF_PAPER_MAX_SIZE)
        return LXMF_ERR_BOUNDS;
    lxmf_sha256(paper, paper_length, result->transient_id);
    if (paper_transient_seen(router, result->transient_id,
                             result->message_id)) {
        result->duplicate = true;
        return LXMF_OK;
    }

    const uint8_t *effective_ratchets = NULL;
    size_t effective_count = 0u;
    uint8_t *owned_ratchets = NULL;
    lxmf_status_t status = router_private_ratchets(
        router, ratchet_private_keys, ratchet_count, &effective_ratchets,
        &effective_count, &owned_ratchets);
    if (status != LXMF_OK) return status;

    uint8_t plaintext[LXMF_PAPER_MAX_SIZE];
    size_t plaintext_length = 0u;
    lxmf_message_t parsed;
    int used_ratchet = 0;
    status = lxmf_paper_unpack(
        paper, paper_length, router->config.identity, effective_ratchets,
        effective_count, enforce_ratchets ? 1 : 0, NULL, NULL, plaintext,
        sizeof plaintext, &plaintext_length, &parsed, result->transient_id,
        result->ratchet_id, &used_ratchet);
    if (owned_ratchets != NULL) {
        rns_hal_secure_zero(
            owned_ratchets, effective_count * RNS_RATCHET_PRIVATE_SIZE);
        free(owned_ratchets);
    }
    result->used_ratchet = used_ratchet != 0;
    if (status != LXMF_OK) {
        rns_hal_secure_zero(plaintext, sizeof plaintext);
        return status;
    }
    memcpy(result->message_id, parsed.message_id, sizeof result->message_id);
    bool inserted = false;
    /* Pinned LXMF feeds paper messages through propagated receive and disables
     * only stamp enforcement. Every other inbound policy remains shared. */
    status = receive_representation(
        router, plaintext, plaintext_length, LXMF_DELIVERY_METHOD_PROPAGATED,
        true, &inserted);
    rns_hal_secure_zero(plaintext, sizeof plaintext);
    if (status != LXMF_OK) return status;
    result->duplicate = !inserted;
    remember_paper_transient(router, result->transient_id,
                             result->message_id);
    return LXMF_OK;
}

lxmf_status_t lxmf_router_receive_uri(
    lxmf_router_t *router, const char *uri, size_t uri_length,
    const uint8_t *ratchet_private_keys, size_t ratchet_count,
    bool enforce_ratchets, lxmf_router_paper_result_t *result) {
    if (router == NULL || uri == NULL || result == NULL)
        return LXMF_ERR_ARGUMENT;
    memset(result, 0, sizeof *result);
    uint8_t paper[LXMF_PAPER_MAX_SIZE];
    size_t paper_length = 0u;
    uint8_t transient_id[LXMF_MESSAGE_ID_LENGTH];
    lxmf_status_t status = lxmf_uri_decode(
        uri, uri_length, paper, sizeof paper, &paper_length, transient_id);
    if (status != LXMF_OK) return status;
    status = lxmf_router_receive_paper(
        router, paper, paper_length, ratchet_private_keys, ratchet_count,
        enforce_ratchets, result);
    rns_hal_secure_zero(paper, sizeof paper);
    return status;
}

static lxmf_status_t receive_representation(
    lxmf_router_t *router, const uint8_t *packed, size_t packed_length,
    lxmf_delivery_method_t method, bool exempt_stamp, bool *inserted_out) {
    if (inserted_out != NULL) *inserted_out = false;
    if (router == NULL || packed == NULL || packed_length == 0U ||
        packed_length > LXMF_STORE_MAX_PACKED)
        return LXMF_ERR_ARGUMENT;
    if (packed_length > inbound_message_limit(router)) return LXMF_ERR_BOUNDS;
    lxmf_message_t message;
    lxmf_identity_verifier_context_t verifier = {
        .resolve = router->config.resolve_identity,
        .resolve_context = router->config.resolve_context};
    lxmf_status_t status = lxmf_unpack(
        packed, packed_length, lxmf_identity_verifier, &verifier, &message);
    bool unverified = status == LXMF_ERR_UNKNOWN_SIGNER;
    if (status != LXMF_OK && !unverified) return status;
    static const char *const aspects[] = {"delivery"};
    uint8_t local_destination[LXMF_DESTINATION_LENGTH];
    if (!rns_destination_hash(router->config.identity, "lxmf", aspects, 1U,
                              local_destination))
        return LXMF_ERR_CRYPTO;
    if (memcmp(message.destination, local_destination,
               LXMF_DESTINATION_LENGTH) != 0)
        return LXMF_ERR_FORMAT;

    /* Reject before learning tickets, recording replay/history state, invoking
     * the application or returning an application-level link proof. */
    if (source_blocked(router, message.source)) {
        report_event(router, message.message_id, method,
                     LXMF_DELIVERY_FAILED, LXMF_QUEUE_REASON_NONE,
                     LXMF_ERR_BLOCKED, 0u);
        return LXMF_ERR_BLOCKED;
    }

    if (!exempt_stamp) {
        status = validate_inbound_stamp(router, &message);
        if (status != LXMF_OK) {
            report_event(router, message.message_id, method,
                         LXMF_DELIVERY_FAILED, LXMF_QUEUE_REASON_STAMP, status,
                         0u);
            return status;
        }
    }
    if (!unverified) remember_verified_ticket(router, &message);

    lxmf_store_message_t stored = {0};
    memcpy(stored.message_id, message.message_id, sizeof stored.message_id);
    memcpy(stored.destination, message.destination, sizeof stored.destination);
    memcpy(stored.source, message.source, sizeof stored.source);
    stored.timestamp = message.timestamp;
    stored.status = LXMF_DELIVERY_DELIVERED;
    stored.content = message.content;
    stored.signature_state =
        unverified ? LXMF_SIGNATURE_UNVERIFIED : LXMF_SIGNATURE_VERIFIED;
    stored.delivery.actual_method = method;
    stored.delivery.progress = LXMF_DELIVERY_PROGRESS_COMPLETE;
    /* Keep the complete representation regardless of signature state. The
     * preview alone cannot preserve title, extensions, media or stamp data. */
    stored.packed = (lxmf_slice_t){packed, packed_length};
    bool inserted = false;
    status = lxmf_store_put(router->config.store, &stored, &inserted);
    if (status != LXMF_OK) return status;
    if (inserted_out != NULL) *inserted_out = inserted;
    if (inserted && router->config.message_callback != NULL)
        router->config.message_callback(router->config.message_context, &stored);
    if (inserted)
        report_event(router, stored.message_id, method,
                     LXMF_DELIVERY_DELIVERED, LXMF_QUEUE_REASON_NONE, LXMF_OK,
                     stored.delivery.attempts);
    return LXMF_OK;
}

typedef struct {
    uint8_t ids[LXMF_STORE_MAX_UNVERIFIED][LXMF_MESSAGE_ID_LENGTH];
    size_t count;
    const uint8_t *source;
} pending_signatures_t;

static bool collect_unverified(void *context,
                               const lxmf_store_message_t *message) {
    pending_signatures_t *pending = context;
    if (message->signature_state != LXMF_SIGNATURE_UNVERIFIED) return true;
    if (pending->source != NULL &&
        memcmp(message->source, pending->source, LXMF_SOURCE_LENGTH) != 0)
        return true;
    if (pending->count < LXMF_STORE_MAX_UNVERIFIED)
        memcpy(pending->ids[pending->count++], message->message_id,
               LXMF_MESSAGE_ID_LENGTH);
    return pending->count < LXMF_STORE_MAX_UNVERIFIED;
}

static void report_signature(lxmf_router_t *router,
                             const uint8_t id[LXMF_MESSAGE_ID_LENGTH],
                             lxmf_signature_state_t state) {
    if (router->config.signature_callback != NULL)
        router->config.signature_callback(router->config.signature_context, id,
                                          state);
}

lxmf_status_t lxmf_router_verify_pending(lxmf_router_t *router,
                                         const uint8_t source[LXMF_SOURCE_LENGTH],
                                         lxmf_router_verify_result_t *result) {
    if (router == NULL || result == NULL || router->config.store == NULL)
        return LXMF_ERR_ARGUMENT;
    memset(result, 0, sizeof *result);
    pending_signatures_t pending = {.source = source};
    lxmf_status_t status =
        lxmf_store_list(router->config.store, collect_unverified, &pending);
    if (status != LXMF_OK) return status;
    lxmf_identity_verifier_context_t verifier = {
        .resolve = router->config.resolve_identity,
        .resolve_context = router->config.resolve_context};
    for (size_t i = 0; i < pending.count; i++) {
        uint8_t *retained = NULL;
        size_t retained_length = 0;
        lxmf_message_t message;
        result->examined++;
        if (lxmf_store_packed_size(router->config.store, pending.ids[i],
                                    &retained_length) != LXMF_OK) {
            result->pending++;
            continue;
        }
        if (retained_length > inbound_message_limit(router)) {
            if (lxmf_store_remove(router->config.store, pending.ids[i]) != LXMF_OK)
                return LXMF_ERR_CRYPTO;
            result->rejected++;
            report_event(router, pending.ids[i], LXMF_DELIVERY_METHOD_UNKNOWN,
                         LXMF_DELIVERY_FAILED, LXMF_QUEUE_REASON_NONE,
                         LXMF_ERR_BOUNDS, 0u);
            continue;
        }
        retained = malloc(retained_length);
        if (retained == NULL) return LXMF_ERR_BOUNDS;
        if (lxmf_store_read_packed(router->config.store, pending.ids[i], retained,
                                    retained_length, &retained_length) != LXMF_OK) {
            free(retained);
            result->pending++;
            continue;
        }
        lxmf_status_t checked =
            lxmf_unpack(retained, retained_length, lxmf_identity_verifier,
                        &verifier, &message);
        if ((checked == LXMF_OK || checked == LXMF_ERR_UNKNOWN_SIGNER) &&
            memcmp(message.message_id, pending.ids[i], LXMF_MESSAGE_ID_LENGTH) == 0 &&
            source_blocked(router, message.source)) {
            free(retained);
            if (lxmf_store_remove(router->config.store, pending.ids[i]) != LXMF_OK)
                return LXMF_ERR_CRYPTO;
            result->rejected++;
            /* A changed policy is not evidence of a bad cryptographic signature. */
            report_event(router, pending.ids[i], LXMF_DELIVERY_METHOD_UNKNOWN,
                         LXMF_DELIVERY_FAILED, LXMF_QUEUE_REASON_NONE,
                         LXMF_ERR_BLOCKED, 0u);
            continue;
        }
        if (checked == LXMF_ERR_UNKNOWN_SIGNER) {
            free(retained);
            result->pending++;
            continue;
        }
        /* Retained bytes that no longer hash to the stored identifier are as
         * untrustworthy as a bad signature. */
        if (checked == LXMF_OK &&
            memcmp(message.message_id, pending.ids[i], LXMF_MESSAGE_ID_LENGTH) != 0)
            checked = LXMF_ERR_FORMAT;
        if (checked == LXMF_OK) checked = validate_inbound_stamp(router, &message);
        if (checked == LXMF_OK) {
            remember_verified_ticket(router, &message);
            free(retained);
            if (lxmf_store_update_signature(router->config.store, pending.ids[i],
                                            LXMF_SIGNATURE_VERIFIED) != LXMF_OK)
                return LXMF_ERR_CRYPTO;
            result->verified++;
            report_signature(router, pending.ids[i], LXMF_SIGNATURE_VERIFIED);
        } else {
            free(retained);
            if (lxmf_store_remove(router->config.store, pending.ids[i]) != LXMF_OK)
                return LXMF_ERR_CRYPTO;
            result->rejected++;
            report_signature(router, pending.ids[i], LXMF_SIGNATURE_FAILED);
            report_event(router, pending.ids[i],
                         LXMF_DELIVERY_METHOD_UNKNOWN, LXMF_DELIVERY_FAILED,
                         checked == LXMF_ERR_STAMP ? LXMF_QUEUE_REASON_STAMP
                                                   : LXMF_QUEUE_REASON_NONE,
                         checked, 0u);
        }
    }
    return LXMF_OK;
}
