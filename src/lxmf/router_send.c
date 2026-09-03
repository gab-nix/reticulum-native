#include "reticulum/lxmf_router.h"
#include "reticulum/lxmf_delivery.h"
#include "reticulum/packet.h"

#include <string.h>

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
                         lxmf_status_t result) {
    if (router->config.event_callback == NULL) return;
    lxmf_router_event_t event = {
        .method = method,
        .state = state,
        .queue_reason = queue_reason,
        .result = result,
        .attempt = 0u
    };
    memcpy(event.message_id, id, sizeof event.message_id);
    router->config.event_callback(router->config.event_context, &event);
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
        default: return "invalid";
    }
}

lxmf_status_t lxmf_router_init(lxmf_router_t *router,
                               const lxmf_router_config_t *config) {
    if (router == NULL || config == NULL || config->identity == NULL ||
        !config->identity->has_private || config->store == NULL ||
        config->resolve_identity == NULL ||
        (config->runtime == NULL && config->send_packet == NULL))
        return LXMF_ERR_ARGUMENT;
    memset(router, 0, sizeof *router);
    router->config = *config;
    return LXMF_OK;
}

static void release_receipts(lxmf_router_t *router, bool all) {
    for (size_t i = 0u; i < LXMF_ROUTER_MAX_RECEIPTS; ++i) {
        lxmf_router_receipt_slot_t *slot = &router->receipts[i];
        if (!slot->used || (!all && !slot->terminal)) continue;
        if (all && !slot->terminal) rns_packet_receipt_cancel(slot->receipt);
        rns_packet_receipt_destroy(slot->receipt);
        memset(slot, 0, sizeof *slot);
    }
}

void lxmf_router_destroy(lxmf_router_t *router) {
    if (router == NULL) return;
    release_receipts(router, true);
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
    } else if (state == RNS_PACKET_RECEIPT_CANCELLED) {
        delivery = LXMF_DELIVERY_FAILED;
        result = LXMF_ERR_CANCELLED;
    } else {
        delivery = LXMF_DELIVERY_FAILED;
        result = status == RNS_ERROR_TIMEOUT ? LXMF_ERR_TIMEOUT : LXMF_ERR_CRYPTO;
    }
    (void)lxmf_store_update_status(router->config.store, slot->message_id,
                                   delivery);
    report(router, slot->message_id, delivery, result);
    report_event(router, slot->message_id,
                 LXMF_DELIVERY_METHOD_OPPORTUNISTIC, delivery,
                 result == LXMF_ERR_TIMEOUT ? LXMF_QUEUE_REASON_RETRY_BACKOFF
                                            : LXMF_QUEUE_REASON_NONE,
                 result);
}

static lxmf_status_t send_with_receipt(
    lxmf_router_t *router, const uint8_t id[LXMF_MESSAGE_ID_LENGTH],
    const uint8_t destination_hash[LXMF_DESTINATION_LENGTH],
    const rns_identity *destination, const uint8_t *packet,
    size_t packet_length, lxmf_queue_reason_t *queue_reason) {
    *queue_reason = LXMF_QUEUE_REASON_RETRY_BACKOFF;
    lxmf_router_receipt_slot_t *slot = reserve_receipt(router);
    if (slot == NULL) return LXMF_ERR_PENDING;
    memcpy(slot->message_id, id, sizeof slot->message_id);
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
    const rns_identity *destination = router->config.resolve_identity(
        router->config.resolve_context, stored.destination);
    /* A missing announce is a normal discovery state, not a delivery failure. */
    if (destination == NULL) {
        report_event(router, id, LXMF_DELIVERY_METHOD_OPPORTUNISTIC,
                     LXMF_DELIVERY_QUEUED, LXMF_QUEUE_REASON_PEER_IDENTITY,
                     LXMF_ERR_PENDING);
        return LXMF_ERR_PENDING;
    }
    lxmf_message_t message = {0};
    message.content = (lxmf_slice_t){content, stored.content.len};
    memcpy(message.destination, stored.destination, sizeof message.destination);
    memcpy(message.source, stored.source, sizeof message.source);
    message.timestamp = stored.timestamp;
    memcpy(message.message_id, stored.message_id, sizeof message.message_id);
    if (lxmf_store_update_status(router->config.store, id,
                                 LXMF_DELIVERY_SENDING) != LXMF_OK)
        return LXMF_ERR_CRYPTO;
    report(router, id, LXMF_DELIVERY_SENDING, LXMF_OK);
    report_event(router, id, LXMF_DELIVERY_METHOD_OPPORTUNISTIC,
                 LXMF_DELIVERY_SENDING, LXMF_QUEUE_REASON_NONE, LXMF_OK);
    size_t packet_length = 0;
    lxmf_queue_reason_t pending_reason = LXMF_QUEUE_REASON_NONE;
    lxmf_status_t status = lxmf_opportunistic_packet_pack(
        &message, router->config.identity, destination, packet, sizeof packet,
        &packet_length);
    if (status == LXMF_OK) {
        if (router->config.runtime != NULL)
            status = send_with_receipt(router, id, stored.destination,
                                       destination, packet, packet_length,
                                       &pending_reason);
        else
            status = router->config.send_packet(router->config.send_context,
                                                packet, packet_length);
    }
    if (status == LXMF_ERR_PENDING) {
        (void)lxmf_store_update_status(router->config.store, id,
                                       LXMF_DELIVERY_QUEUED);
        report(router, id, LXMF_DELIVERY_QUEUED, status);
        report_event(router, id, LXMF_DELIVERY_METHOD_OPPORTUNISTIC,
                     LXMF_DELIVERY_QUEUED,
                     pending_reason, status);
        return status;
    }
    if (status == LXMF_OK && lxmf_store_update_status(router->config.store, id,
                                                       LXMF_DELIVERY_SENT) != LXMF_OK)
        status = LXMF_ERR_CRYPTO;
    if (status == LXMF_OK) {
        report(router, id, LXMF_DELIVERY_SENT, LXMF_OK);
        report_event(router, id, LXMF_DELIVERY_METHOD_OPPORTUNISTIC,
                     LXMF_DELIVERY_SENT, LXMF_QUEUE_REASON_NONE, LXMF_OK);
    } else {
        (void)lxmf_store_update_status(router->config.store, id,
                                       LXMF_DELIVERY_FAILED);
        report(router, id, LXMF_DELIVERY_FAILED, status);
        report_event(router, id, LXMF_DELIVERY_METHOD_OPPORTUNISTIC,
                     LXMF_DELIVERY_FAILED, LXMF_QUEUE_REASON_NONE, status);
    }
    return status;
}

typedef struct {
    uint8_t ids[LXMF_STORE_MAX_MESSAGES][LXMF_MESSAGE_ID_LENGTH];
    size_t count;
    size_t limit;
} pending_messages_t;

static bool collect_pending(void *context, const lxmf_store_message_t *message) {
    pending_messages_t *pending = context;
    if ((message->status == LXMF_DELIVERY_QUEUED ||
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
    lxmf_status_t status = lxmf_opportunistic_packet_unpack(
        packet, packet_length, router->config.identity, lxmf_identity_verifier,
        &verifier, plaintext, sizeof plaintext, &plaintext_length, &message);
    /* An identity we do not hold yet cannot condemn the message: retain it
     * flagged so it can be shown and checked again after its announce. A
     * signature that fails against an identity we do hold is forged. */
    bool unverified = status == LXMF_ERR_UNKNOWN_SIGNER;
    if (status != LXMF_OK && !unverified) return status;
    if (unverified && plaintext_length > LXMF_STORE_MAX_PACKED)
        return LXMF_ERR_BOUNDS;

    lxmf_store_message_t stored = {0};
    memcpy(stored.message_id, message.message_id, sizeof stored.message_id);
    memcpy(stored.destination, message.destination, sizeof stored.destination);
    memcpy(stored.source, message.source, sizeof stored.source);
    stored.timestamp = message.timestamp;
    stored.status = LXMF_DELIVERY_DELIVERED;
    stored.content = message.content;
    stored.signature_state =
        unverified ? LXMF_SIGNATURE_UNVERIFIED : LXMF_SIGNATURE_VERIFIED;
    if (unverified) stored.packed = (lxmf_slice_t){plaintext, plaintext_length};
    bool inserted = false;
    status = lxmf_store_put(router->config.store, &stored, &inserted);
    if (status != LXMF_OK) return status;
    if (inserted && router->config.message_callback != NULL)
        router->config.message_callback(router->config.message_context, &stored);
    if (inserted)
        report_event(router, stored.message_id,
                     LXMF_DELIVERY_METHOD_OPPORTUNISTIC,
                     LXMF_DELIVERY_DELIVERED, LXMF_QUEUE_REASON_NONE, LXMF_OK);
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
        uint8_t retained[LXMF_STORE_MAX_PACKED];
        size_t retained_length = 0;
        lxmf_message_t message;
        result->examined++;
        if (lxmf_store_read_packed(router->config.store, pending.ids[i], retained,
                                   sizeof retained, &retained_length) != LXMF_OK) {
            result->pending++;
            continue;
        }
        lxmf_status_t checked =
            lxmf_unpack(retained, retained_length, lxmf_identity_verifier,
                        &verifier, &message);
        if (checked == LXMF_ERR_UNKNOWN_SIGNER) {
            result->pending++;
            continue;
        }
        /* Retained bytes that no longer hash to the stored identifier are as
         * untrustworthy as a bad signature. */
        if (checked == LXMF_OK &&
            memcmp(message.message_id, pending.ids[i], LXMF_MESSAGE_ID_LENGTH) != 0)
            checked = LXMF_ERR_FORMAT;
        if (checked == LXMF_OK) {
            if (lxmf_store_update_signature(router->config.store, pending.ids[i],
                                            LXMF_SIGNATURE_VERIFIED) != LXMF_OK)
                return LXMF_ERR_CRYPTO;
            result->verified++;
            report_signature(router, pending.ids[i], LXMF_SIGNATURE_VERIFIED);
        } else {
            if (lxmf_store_remove(router->config.store, pending.ids[i]) != LXMF_OK)
                return LXMF_ERR_CRYPTO;
            result->rejected++;
            report_signature(router, pending.ids[i], LXMF_SIGNATURE_FAILED);
        }
    }
    return LXMF_OK;
}
