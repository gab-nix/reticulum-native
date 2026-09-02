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

lxmf_status_t lxmf_router_init(lxmf_router_t *router,
                               const lxmf_router_config_t *config) {
    if (router == NULL || config == NULL || config->identity == NULL ||
        !config->identity->has_private || config->store == NULL ||
        config->resolve_identity == NULL || config->send_packet == NULL)
        return LXMF_ERR_ARGUMENT;
    router->config = *config;
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
    if (destination == NULL) return LXMF_ERR_FORMAT;
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
    size_t packet_length = 0;
    lxmf_status_t status = lxmf_opportunistic_packet_pack(
        &message, router->config.identity, destination, packet, sizeof packet,
        &packet_length);
    if (status == LXMF_OK)
        status = router->config.send_packet(router->config.send_context, packet,
                                            packet_length);
    if (status == LXMF_OK && lxmf_store_update_status(router->config.store, id,
                                                       LXMF_DELIVERY_SENT) != LXMF_OK)
        status = LXMF_ERR_CRYPTO;
    if (status == LXMF_OK) {
        report(router, id, LXMF_DELIVERY_SENT, LXMF_OK);
    } else {
        (void)lxmf_store_update_status(router->config.store, id,
                                       LXMF_DELIVERY_FAILED);
        report(router, id, LXMF_DELIVERY_FAILED, status);
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
    if (max_messages == 0u) return LXMF_OK;
    pending_messages_t pending = {.limit = max_messages};
    lxmf_status_t status = lxmf_store_list(router->config.store, collect_pending,
                                           &pending);
    if (status != LXMF_OK) return status;
    for (size_t i = 0; i < pending.count; i++) {
        status = lxmf_router_send_message(router, pending.ids[i]);
        result->attempted++;
        if (status == LXMF_OK) result->sent++;
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
    if (status != LXMF_OK) return status;

    lxmf_store_message_t stored = {0};
    memcpy(stored.message_id, message.message_id, sizeof stored.message_id);
    memcpy(stored.destination, message.destination, sizeof stored.destination);
    memcpy(stored.source, message.source, sizeof stored.source);
    stored.timestamp = message.timestamp;
    stored.status = LXMF_DELIVERY_DELIVERED;
    stored.content = message.content;
    bool inserted = false;
    status = lxmf_store_put(router->config.store, &stored, &inserted);
    if (status != LXMF_OK) return status;
    if (inserted && router->config.message_callback != NULL)
        router->config.message_callback(router->config.message_context, &stored);
    return LXMF_OK;
}
