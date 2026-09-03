#include "reticulum/lxmf_router.h"
#include "reticulum/destination.h"
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

static lxmf_status_t receive_representation(
    lxmf_router_t *router, const uint8_t *packed, size_t packed_length,
    lxmf_delivery_method_t method);
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

typedef struct {
    lxmf_store_t *store;
    lxmf_status_t status;
} recovery_context_t;

static bool recover_interrupted(void *context,
                                const lxmf_store_message_t *message) {
    recovery_context_t *recovery = context;
    bool interrupted = message->status == LXMF_DELIVERY_SENDING ||
                       (message->status == LXMF_DELIVERY_SENT &&
                        message->delivery.has_proof_id);
    if (!interrupted) return true;
    lxmf_delivery_metadata_t delivery = message->delivery;
    delivery.queue_reason = LXMF_QUEUE_REASON_RETRY_BACKOFF;
    delivery.retry_at_ms = 0u;
    delivery.progress = 0u;
    delivery.has_proof_id = false;
    memset(delivery.proof_id, 0, sizeof delivery.proof_id);
    recovery->status = lxmf_store_update_delivery(
        recovery->store, message->message_id, &delivery);
    if (recovery->status == LXMF_OK)
        recovery->status = lxmf_store_update_status(
            recovery->store, message->message_id, LXMF_DELIVERY_QUEUED);
    return recovery->status == LXMF_OK;
}

static lxmf_status_t recover_interrupted_deliveries(lxmf_store_t *store) {
    recovery_context_t recovery = {.store = store, .status = LXMF_OK};
    lxmf_status_t status = lxmf_store_list(store, recover_interrupted,
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
        (config->runtime == NULL && config->send_packet == NULL))
        return LXMF_ERR_ARGUMENT;
    memset(router, 0, sizeof *router);
    router->config = *config;
    if (recover_interrupted_deliveries(config->store) != LXMF_OK) {
        memset(router, 0, sizeof *router);
        return LXMF_ERR_CRYPTO;
    }
    if (config->runtime != NULL && config->accept_inbound_links) {
        static const char *const aspects[] = {"delivery"};
        uint8_t hash[LXMF_DESTINATION_LENGTH];
        rns_runtime_link_options_t options = {
            .prove_data_packets = true,
            .state_callback = direct_link_state_changed,
            .packet_callback = direct_link_packet_received,
            .identified_callback = direct_link_identified,
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
        if (all && !slot->terminal) rns_packet_receipt_cancel(slot->receipt);
        rns_packet_receipt_destroy(slot->receipt);
        memset(slot, 0, sizeof *slot);
    }
}

void lxmf_router_destroy(lxmf_router_t *router) {
    if (router == NULL) return;
    release_receipts(router, true);
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
        delivery = LXMF_DELIVERY_FAILED;
        result = LXMF_ERR_CANCELLED;
    } else {
        delivery = LXMF_DELIVERY_FAILED;
        result = status == RNS_ERROR_TIMEOUT ? LXMF_ERR_TIMEOUT : LXMF_ERR_CRYPTO;
    }
    lxmf_delivery_metadata_t metadata;
    if (lxmf_store_read_delivery(router->config.store, slot->message_id,
                                 &metadata) == LXMF_OK) {
        metadata.actual_method = slot->method;
        metadata.queue_reason = delivery == LXMF_DELIVERY_DELIVERED
                                    ? LXMF_QUEUE_REASON_NONE
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
                     : LXMF_QUEUE_REASON_RETRY_BACKOFF,
                 result, slot->attempt);
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
    (void)link;
    (void)receive_representation(router, plaintext, plaintext_length,
                                 LXMF_DELIVERY_METHOD_DIRECT);
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
        .prove_data_packets = true,
        .state_callback = direct_link_state_changed,
        .packet_callback = direct_link_packet_received,
        .identified_callback = direct_link_identified,
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

static lxmf_status_t send_direct(
    lxmf_router_t *router, const lxmf_store_message_t *stored,
    const uint8_t id[LXMF_MESSAGE_ID_LENGTH], const rns_identity *destination,
    uint32_t attempt, lxmf_queue_reason_t *queue_reason,
    uint8_t proof_id[LXMF_MESSAGE_ID_LENGTH]) {
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
    uint8_t packed[LXMF_STORE_MAX_PACKED];
    size_t packed_length = 0U;
    status = lxmf_store_read_packed(router->config.store, id, packed,
                                    sizeof packed, &packed_length);
    bool retained_wire = status == LXMF_OK;
    if (status == LXMF_OK) {
        lxmf_message_t retained;
        status = lxmf_unpack(packed, packed_length, NULL, NULL, &retained);
        if (status == LXMF_OK &&
            (memcmp(retained.message_id, id, LXMF_MESSAGE_ID_LENGTH) != 0 ||
             memcmp(retained.destination, stored->destination,
                    LXMF_DESTINATION_LENGTH) != 0 ||
             memcmp(retained.source, stored->source,
                    LXMF_SOURCE_LENGTH) != 0))
            status = LXMF_ERR_FORMAT;
    }
    if (!retained_wire && status == LXMF_ERR_FORMAT) {
        /* Compatibility with stores written before outbound representations
         * were retained. New messages always take the exact durable bytes. */
        lxmf_message_t message = {0};
        message.content = stored->content;
        memcpy(message.destination, stored->destination,
               LXMF_DESTINATION_LENGTH);
        memcpy(message.source, stored->source, LXMF_SOURCE_LENGTH);
        message.timestamp = stored->timestamp;
        status = lxmf_pack(&message, lxmf_identity_signer,
                           router->config.identity, packed, sizeof packed,
                           &packed_length);
    }
    if (status != LXMF_OK) {
        *queue_reason = LXMF_QUEUE_REASON_RESOURCE;
        return status == LXMF_ERR_BOUNDS ? LXMF_ERR_PENDING : status;
    }
    lxmf_router_receipt_slot_t *receipt_slot = reserve_receipt(router);
    if (receipt_slot == NULL) {
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
    if (method == LXMF_DELIVERY_METHOD_PROPAGATED) {
        stored.delivery.queue_reason = LXMF_QUEUE_REASON_PROPAGATION_NODE;
        if (lxmf_store_update_delivery(router->config.store, id,
                                       &stored.delivery) != LXMF_OK)
            return LXMF_ERR_CRYPTO;
        report_event(router, id, method, LXMF_DELIVERY_QUEUED,
                     LXMF_QUEUE_REASON_PROPAGATION_NODE, LXMF_ERR_PENDING,
                     stored.delivery.attempts);
        return LXMF_ERR_PENDING;
    }
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
    lxmf_message_t message = {0};
    message.content = (lxmf_slice_t){content, stored.content.len};
    memcpy(message.destination, stored.destination, sizeof message.destination);
    memcpy(message.source, stored.source, sizeof message.source);
    message.timestamp = stored.timestamp;
    memcpy(message.message_id, stored.message_id, sizeof message.message_id);
    stored.delivery.actual_method = method;
    if (lxmf_store_update_delivery(router->config.store, id,
                                   &stored.delivery) != LXMF_OK)
        return LXMF_ERR_CRYPTO;
    if (lxmf_store_update_status(router->config.store, id,
                                 LXMF_DELIVERY_SENDING) != LXMF_OK)
        return LXMF_ERR_CRYPTO;
    report(router, id, LXMF_DELIVERY_SENDING, LXMF_OK);
    uint32_t attempt = stored.delivery.attempts == UINT32_MAX
                           ? UINT32_MAX
                           : stored.delivery.attempts + 1u;
    report_event(router, id, method,
                 LXMF_DELIVERY_SENDING, LXMF_QUEUE_REASON_NONE, LXMF_OK,
                 attempt);
    size_t packet_length = 0;
    uint8_t proof_id[LXMF_MESSAGE_ID_LENGTH] = {0};
    bool has_proof_id = false;
    lxmf_queue_reason_t pending_reason = LXMF_QUEUE_REASON_NONE;
    lxmf_status_t status;
    if (method == LXMF_DELIVERY_METHOD_DIRECT) {
        if (router->config.runtime == NULL)
            status = LXMF_ERR_ARGUMENT;
        else
            status = send_direct(router, &stored, id, destination, attempt,
                                 &pending_reason, proof_id);
        if (status == LXMF_OK) has_proof_id = true;
    } else {
        status = lxmf_opportunistic_packet_pack(
            &message, router->config.identity, destination, packet,
            sizeof packet, &packet_length);
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
        stored.delivery.queue_reason = LXMF_QUEUE_REASON_NONE;
        stored.delivery.progress = LXMF_DELIVERY_PROGRESS_COMPLETE;
        stored.delivery.has_proof_id = has_proof_id;
        if (has_proof_id)
            memcpy(stored.delivery.proof_id, proof_id,
                   sizeof stored.delivery.proof_id);
        if (lxmf_store_update_delivery(router->config.store, id,
                                       &stored.delivery) != LXMF_OK)
            status = LXMF_ERR_CRYPTO;
    }
    if (status == LXMF_OK && lxmf_store_update_status(router->config.store, id,
                                                       LXMF_DELIVERY_SENT) != LXMF_OK)
        status = LXMF_ERR_CRYPTO;
    if (status == LXMF_OK) {
        report(router, id, LXMF_DELIVERY_SENT, LXMF_OK);
        report_event(router, id, method,
                     LXMF_DELIVERY_SENT, LXMF_QUEUE_REASON_NONE, LXMF_OK,
                     stored.delivery.attempts);
    } else {
        stored.delivery.attempts = attempt;
        stored.delivery.queue_reason = LXMF_QUEUE_REASON_RETRY_BACKOFF;
        (void)lxmf_store_update_delivery(router->config.store, id,
                                         &stored.delivery);
        (void)lxmf_store_update_status(router->config.store, id,
                                       LXMF_DELIVERY_FAILED);
        report(router, id, LXMF_DELIVERY_FAILED, status);
        report_event(router, id, method,
                     LXMF_DELIVERY_FAILED, LXMF_QUEUE_REASON_RETRY_BACKOFF,
                     status,
                     stored.delivery.attempts);
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

    return receive_representation(router, plaintext, plaintext_length,
                                  LXMF_DELIVERY_METHOD_OPPORTUNISTIC);
}

static lxmf_status_t receive_representation(
    lxmf_router_t *router, const uint8_t *packed, size_t packed_length,
    lxmf_delivery_method_t method) {
    if (router == NULL || packed == NULL || packed_length == 0U ||
        packed_length > LXMF_STORE_MAX_PACKED)
        return LXMF_ERR_ARGUMENT;
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
    if (unverified) stored.packed = (lxmf_slice_t){packed, packed_length};
    bool inserted = false;
    status = lxmf_store_put(router->config.store, &stored, &inserted);
    if (status != LXMF_OK) return status;
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
