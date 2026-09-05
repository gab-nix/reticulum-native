#include "reticulum/runtime.h"

#include "reticulum/announce.h"
#include "reticulum/crypto.h"
#include "reticulum/destination.h"
#include "reticulum/hal.h"
#include "reticulum/kiss.h"
#include "reticulum/local.h"
#include "reticulum/packet.h"
#include "reticulum/path_store.h"
#include "reticulum/request.h"
#include "reticulum/resource.h"
#include "reticulum/proof.h"
#include "reticulum/rnode.h"
#include "reticulum/tcp.h"
#include "reticulum/udp.h"

#include "../interfaces/auto_posix.h"
#include "../platform/interface_internal.h"

#include <stdlib.h>
#include <string.h>

#define RUNTIME_DEFAULT_WORK 32U
/* Each active link can have one outgoing Resource and one incoming
 * RESOURCE_REQ can ask for the pinned fast-link maximum of 75 parts. A single
 * TCP receive batch may dispatch requests for every runtime link before the
 * endpoint flushes. Each packet can double under HDLC escaping; retain two
 * additional bounded control frames. */
#define RUNTIME_TCP_FRAME_MAX ((RNS_MTU * 2U) + 2U)
#define RUNTIME_TCP_QUEUE \
    (RUNTIME_TCP_FRAME_MAX * \
     ((RNS_RUNTIME_MAX_LINKS * RNS_RESOURCE_WINDOW_MAX) + 2U))
#define RUNTIME_TCP_RECONNECT_INITIAL 1.0
#define RUNTIME_TCP_RECONNECT_MAX 30.0

typedef struct runtime_interface {
    rns_runtime_interface_info_t info;
    rns_interface_t *provider;
    bool same_interface_rebroadcast;
    rns_udp_endpoint_t *udp;
    rns_udp_address_t udp_forward;
    bool has_udp_forward;
    rns_tcp_endpoint_t *tcp;
    rns_tcp_endpoint_t *accepted;
    rns_local_instance_t *local;
    rns_kiss_endpoint_t *kiss;
    rns_rnode_endpoint_t *rnode;
    rns_auto_posix_t *auto_interface;
    bool local_server;
    uint64_t kiss_reported_drops;
    uint64_t rnode_reported_drops;
    double reconnect_at;
    double reconnect_delay;
} runtime_interface_t;

struct rns_runtime_link {
    rns_runtime_t *runtime;
    rns_link protocol;
    rns_runtime_link_options_t options;
    size_t interface_index;
    double last_inbound;
    double last_outbound;
    double keepalive_seconds;
    bool registered;
    rns_request_receipt_t *requests[RNS_RUNTIME_MAX_REQUESTS];
    rns_resource_t *resource;
    rns_request_receipt_t *resource_receipt;
    bool resource_application;
    double resource_deadline;
    double resource_request_retry_at;
    uint8_t resource_hash[RNS_RESOURCE_HASH_SIZE];
    uint8_t resource_original_hash[RNS_RESOURCE_HASH_SIZE];
    uint8_t *resource_assembled;
    size_t resource_assembled_length;
    size_t resource_assembled_capacity;
    size_t resource_next_segment;
    size_t resource_total_segments;
    rns_runtime_resource_transfer_t *outgoing_resource;
    uint8_t callback_packet_hash[RNS_PROOF_HASH_SIZE];
    bool callback_packet_provable;
    bool remote_identified;
    rns_runtime_destination_t *inbound_destination;
};

struct rns_runtime_resource_transfer {
    rns_runtime_link_t *link;
    rns_resource_sender_t *sender;
    rns_runtime_resource_options_t options;
    rns_runtime_resource_state_t state;
    double deadline;
    size_t sent_parts;
    size_t sent_segment_bytes;
    bool part_sent[RNS_RESOURCE_MAX_PARTS];
    bool runtime_owned;
};

struct rns_runtime_request_handler {
    rns_runtime_destination_t *destination;
    char path[RNS_REQUEST_PATH_MAX + 1U];
    uint8_t path_hash[RNS_REQUEST_ID_LENGTH];
    rns_runtime_request_handler_options_t options;
    uint8_t allow_identity_hashes[RNS_RUNTIME_MAX_REQUEST_ALLOWLIST][16];
};

struct rns_runtime_destination {
    rns_runtime_t *runtime;
    rns_identity identity;
    uint8_t hash[16];
    rns_runtime_link_options_t link_options;
    rns_runtime_inbound_link_callback_t accepted_callback;
    void *callback_context;
    rns_runtime_request_handler_t *request_handlers[
        RNS_RUNTIME_MAX_REQUEST_HANDLERS];
};

struct rns_request_receipt {
    rns_runtime_link_t *link;
    rns_request_options_t options;
    rns_request_state_t state;
    uint8_t request_id[RNS_REQUEST_ID_LENGTH];
    double deadline;
};

struct rns_packet_receipt {
    rns_runtime_t *runtime;
    rns_runtime_link_t *link;
    rns_packet_receipt_options_t options;
    rns_packet_receipt_state_t state;
    rns_identity destination_identity;
    uint8_t packet_hash[RNS_PROOF_HASH_SIZE];
    double sent_at;
    double concluded_at;
    double deadline;
    bool link_proof;
    uint8_t link_id[16];
    uint8_t link_signing_public[32];
};

typedef struct {
    bool used;
    uint8_t hash[16];
    rns_identity identity;
    uint8_t body[RNS_ANNOUNCE_MAX_BODY_SIZE];
    size_t body_length;
    uint8_t context_flag;
    double next_response_at;
} runtime_local_announce_t;

struct rns_runtime {
    rns_config_t config;
    rns_node node;
    runtime_interface_t interfaces[RNS_CONFIG_MAX_INTERFACES + 1U];
    size_t interface_count;
    size_t poll_cursor;
    rns_runtime_packet_callback_t packet_callback;
    rns_runtime_announce_callback_t announce_callback;
    void *callback_context;
    rns_runtime_link_t *links[RNS_RUNTIME_MAX_LINKS];
    rns_runtime_destination_t *destinations[RNS_RUNTIME_MAX_DESTINATIONS];
    uint8_t *plain_destinations;
    size_t plain_destination_capacity;
    size_t plain_destination_count;
    runtime_local_announce_t *local_announces;
    rns_packet_receipt_t *packet_receipts[RNS_RUNTIME_MAX_PACKET_RECEIPTS];
    rns_runtime_clock_callback_t reconnect_clock;
    void *reconnect_clock_context;
    double tcp_reconnect_initial;
    double tcp_reconnect_max;
    bool shared_server;
};

typedef struct receive_context {
    rns_runtime_t *runtime;
    size_t interface_index;
    size_t remaining;
    size_t processed;
    bool provider_budget_exceeded;
} receive_context_t;

static double runtime_clock(void *context) {
    uint64_t milliseconds = 0U;
    (void)context;
    (void)rns_hal_monotonic_ms(&milliseconds);
    return (double)milliseconds / 1000.0;
}

static double reconnect_clock(const rns_runtime_t *runtime) {
    return runtime->reconnect_clock != NULL
               ? runtime->reconnect_clock(runtime->reconnect_clock_context)
               : runtime_clock(NULL);
}

static void tcp_schedule_reconnect(rns_runtime_t *runtime,
                                   runtime_interface_t *interface) {
    if (interface->reconnect_delay <= 0.0)
        interface->reconnect_delay = runtime->tcp_reconnect_initial;
    interface->reconnect_at = reconnect_clock(runtime) +
                              interface->reconnect_delay;
    if (interface->reconnect_delay < runtime->tcp_reconnect_max) {
        interface->reconnect_delay *= 2.0;
        if (interface->reconnect_delay > runtime->tcp_reconnect_max)
            interface->reconnect_delay = runtime->tcp_reconnect_max;
    }
}

static void tcp_mark_connected(rns_runtime_t *runtime,
                               runtime_interface_t *interface) {
    (void)runtime;
    interface->info.state = RNS_RUNTIME_INTERFACE_UP;
    interface->info.last_error = RNS_OK;
    interface->info.connections_established++;
    interface->reconnect_at = 0.0;
    interface->reconnect_delay = 0.0;
}

static void tcp_mark_client_down(rns_runtime_t *runtime,
                                 runtime_interface_t *interface,
                                 rns_status_t reason) {
    if (rns_tcp_state(interface->tcp) != RNS_TCP_DISCONNECTED)
        rns_tcp_disconnect(interface->tcp);
    interface->info.state = RNS_RUNTIME_INTERFACE_DOWN;
    interface->info.last_error = reason;
    tcp_schedule_reconnect(runtime, interface);
}

static void packet_receipt_notify(rns_packet_receipt_t *receipt,
                                  rns_status_t status) {
    if (receipt->options.callback != NULL)
        receipt->options.callback(receipt, receipt->state, status,
                                  receipt->options.callback_context);
}

static bool packet_receipt_ingress(rns_runtime_t *runtime, const uint8_t *raw,
                                   size_t raw_length) {
    rns_packet packet;
    if (!rns_packet_decode(&packet, raw, raw_length) || packet.packet_type != 3U ||
        packet.context == RNS_LINK_CONTEXT_PROOF ||
        packet.context == RNS_LINK_CONTEXT_RESOURCE_PRF)
        return false;
    for (size_t i = 0U; i < RNS_RUNTIME_MAX_PACKET_RECEIPTS; ++i) {
        rns_packet_receipt_t *receipt = runtime->packet_receipts[i];
        if (receipt == NULL || receipt->state != RNS_PACKET_RECEIPT_PENDING)
            continue;
        if (receipt->link_proof) {
            rns_identity verifier = {0};
            if (memcmp(packet.destination_hash, receipt->link_id, 16U) != 0)
                continue;
            memcpy(verifier.signing_public, receipt->link_signing_public, 32U);
            if (!rns_proof_validate(&verifier, receipt->packet_hash, packet.data,
                                    packet.data_length))
                continue;
        } else {
            if (memcmp(packet.destination_hash, receipt->packet_hash, 16U) != 0 ||
                !rns_proof_validate(&receipt->destination_identity,
                                    receipt->packet_hash, packet.data,
                                    packet.data_length))
                continue;
        }
        receipt->state = RNS_PACKET_RECEIPT_DELIVERED;
        receipt->concluded_at = runtime_clock(NULL);
        packet_receipt_notify(receipt, RNS_OK);
        return true;
    }
    return false;
}

static rns_status_t send_internal(rns_runtime_t *runtime, size_t index,
                                  const uint8_t *packet, size_t length) {
    runtime_interface_t *interface;
    rns_status_t status = RNS_ERROR_INVALID_STATE;
    size_t transmissions = 1u;
    if (runtime == NULL || index >= runtime->interface_count || packet == NULL ||
        length == 0U || length > RNS_MTU) return RNS_ERROR_INVALID_ARGUMENT;
    interface = &runtime->interfaces[index];
    if (interface->provider != NULL) {
        rns_interface_stats_t stats;
        rns_status_t check = rns_interface_get_stats(interface->provider, &stats);
        if (check != RNS_OK) return check;
        if (!stats.online || !stats.outbound) return RNS_ERROR_INVALID_STATE;
        if (length > stats.effective_mtu) return RNS_ERROR_OVERFLOW;
    }
    if (interface->info.state != RNS_RUNTIME_INTERFACE_UP)
        return interface->info.last_error != RNS_OK ? interface->info.last_error
                                                    : RNS_ERROR_INVALID_STATE;
    if (interface->provider != NULL)
        status = rns_interface_send(interface->provider, packet, length);
    else if (interface->udp != NULL && interface->has_udp_forward)
        status = rns_udp_send_to(interface->udp, &interface->udp_forward, packet, length);
    else if (interface->accepted != NULL)
        status = rns_tcp_queue_frame(interface->accepted, packet, length);
    else if (interface->tcp != NULL && rns_tcp_state(interface->tcp) == RNS_TCP_CONNECTED)
        status = rns_tcp_queue_frame(interface->tcp, packet, length);
    else if (interface->local != NULL)
        status = rns_local_instance_send(interface->local, packet, length);
    else if (interface->kiss != NULL)
        status = rns_kiss_endpoint_send(interface->kiss, packet, length);
    else if (interface->rnode != NULL)
        status = rns_rnode_endpoint_send(interface->rnode, packet, length);
    else if (interface->auto_interface != NULL)
        status = rns_auto_posix_send(interface->auto_interface, packet, length,
                                     &transmissions);
    if (status == RNS_OK) {
        interface->info.packets_sent += transmissions;
        interface->info.bytes_sent += transmissions * length;
    } else interface->info.last_error = status;
    return status;
}

static void link_notify(rns_runtime_link_t *link, rns_link_state state,
                        rns_status_t reason) {
    if (link->options.state_callback != NULL)
        link->options.state_callback(link, state, reason,
                                     link->options.callback_context);
}

static void link_update_keepalive(rns_runtime_link_t *link) {
    double scaled = link->protocol.rtt * (360.0 / 1.75);
    if (scaled < 5.0) scaled = 5.0;
    if (scaled > 360.0) scaled = 360.0;
    link->keepalive_seconds = scaled;
}

static rns_status_t link_send_wire(rns_runtime_link_t *link, uint8_t context,
                                   const uint8_t *data, size_t data_length,
                                   uint8_t packet_id[16]) {
    uint8_t raw[RNS_MTU];
    size_t raw_length = 0U;
    rns_packet packet = {0};
    packet.destination_type = 3U;
    packet.packet_type = 0U;
    packet.context = context;
    memcpy(packet.destination_hash, link->protocol.link_id,
           sizeof packet.destination_hash);
    packet.data = data;
    packet.data_length = data_length;
    if (!rns_packet_encode(&packet, raw, sizeof raw, &raw_length))
        return RNS_ERROR_OVERFLOW;
    if (packet_id != NULL && !rns_packet_truncated_hash(raw, raw_length, packet_id))
        return RNS_ERROR_CRYPTO;
    rns_status_t status = send_internal(link->runtime, link->interface_index,
                                        raw, raw_length);
    if (status == RNS_OK) link->last_outbound = runtime_clock(NULL);
    return status;
}

static rns_status_t link_send_wire_hash(rns_runtime_link_t *link,
                                        uint8_t context, const uint8_t *data,
                                        size_t data_length,
                                        uint8_t packet_hash[32]) {
    uint8_t raw[RNS_MTU];
    size_t raw_length = 0U;
    rns_packet packet = {0};
    packet.destination_type = 3U;
    packet.packet_type = 0U;
    packet.context = context;
    memcpy(packet.destination_hash, link->protocol.link_id,
           sizeof packet.destination_hash);
    packet.data = data;
    packet.data_length = data_length;
    if (!rns_packet_encode(&packet, raw, sizeof raw, &raw_length))
        return RNS_ERROR_OVERFLOW;
    if (!rns_packet_hash(raw, raw_length, packet_hash))
        return RNS_ERROR_CRYPTO;
    rns_status_t status = send_internal(link->runtime, link->interface_index,
                                        raw, raw_length);
    if (status == RNS_OK) link->last_outbound = runtime_clock(NULL);
    return status;
}

static rns_status_t link_send_plain(rns_runtime_link_t *link, uint8_t context,
                                    const uint8_t *plaintext,
                                    size_t plaintext_length,
                                    uint8_t packet_id[16]);

static rns_status_t link_send_plain2(rns_runtime_link_t *link, uint8_t context,
                                     const uint8_t *plaintext,
                                     size_t plaintext_length) {
    return link_send_plain(link, context, plaintext, plaintext_length, NULL);
}

static rns_status_t link_send_proof(rns_runtime_link_t *link, uint8_t context,
                                    const uint8_t *data, size_t data_length) {
    uint8_t raw[RNS_MTU];
    size_t raw_length = 0U;
    rns_packet packet = {0};
    packet.destination_type = 3U;
    packet.packet_type = 3U;
    packet.context = context;
    memcpy(packet.destination_hash, link->protocol.link_id,
           sizeof packet.destination_hash);
    packet.data = data;
    packet.data_length = data_length;
    if (!rns_packet_encode(&packet, raw, sizeof raw, &raw_length))
        return RNS_ERROR_OVERFLOW;
    rns_status_t status = send_internal(link->runtime, link->interface_index,
                                        raw, raw_length);
    if (status == RNS_OK) link->last_outbound = runtime_clock(NULL);
    return status;
}

static void request_notify(rns_request_receipt_t *receipt, rns_status_t status,
                           const uint8_t *response, size_t response_length) {
    if (receipt->options.callback != NULL)
        receipt->options.callback(receipt, receipt->state, status, response,
                                  response_length,
                                  receipt->options.callback_context);
}

static void fail_pending_requests(rns_runtime_link_t *link,
                                  rns_status_t reason) {
    for (size_t i = 0U; i < RNS_RUNTIME_MAX_REQUESTS; i++) {
        rns_request_receipt_t *receipt = link->requests[i];
        if (receipt == NULL || receipt->state != RNS_REQUEST_PENDING) continue;
        receipt->state = RNS_REQUEST_FAILED;
        request_notify(receipt, reason, NULL, 0U);
    }
}

static bool handle_response(rns_runtime_link_t *link, const uint8_t *plaintext,
                            size_t plaintext_length) {
    rns_response_view_t response;
    if (rns_response_decode(plaintext, plaintext_length, &response) != RNS_OK)
        return false;
    for (size_t i = 0U; i < RNS_RUNTIME_MAX_REQUESTS; i++) {
        rns_request_receipt_t *receipt = link->requests[i];
        if (receipt == NULL || receipt->state != RNS_REQUEST_PENDING ||
            memcmp(receipt->request_id, response.request_id,
                   RNS_REQUEST_ID_LENGTH) != 0) continue;
        if (response.response_length > receipt->options.max_response_size) {
            receipt->state = RNS_REQUEST_FAILED;
            request_notify(receipt, RNS_ERROR_OVERFLOW, NULL, 0U);
        } else {
            receipt->state = RNS_REQUEST_COMPLETE;
            request_notify(receipt, RNS_OK, response.response,
                           response.response_length);
        }
        return true;
    }
    return false;
}

static rns_request_receipt_t *find_receipt(rns_runtime_link_t *link,
                                          const uint8_t *request_id) {
    if (request_id == NULL) return NULL;
    for (size_t i = 0U; i < RNS_RUNTIME_MAX_REQUESTS; i++) {
        rns_request_receipt_t *receipt = link->requests[i];
        if (receipt != NULL && receipt->state == RNS_REQUEST_PENDING &&
            memcmp(receipt->request_id, request_id, RNS_REQUEST_ID_LENGTH) == 0)
            return receipt;
    }
    return NULL;
}

static void resource_release(rns_runtime_link_t *link) {
    rns_resource_destroy(link->resource);
    link->resource = NULL;
    free(link->resource_assembled);
    link->resource_assembled = NULL;
    link->resource_assembled_length = 0U;
    link->resource_assembled_capacity = 0U;
    link->resource_next_segment = 0U;
    link->resource_total_segments = 0U;
    link->resource_receipt = NULL;
    link->resource_application = false;
    link->resource_deadline = 0.0;
    link->resource_request_retry_at = 0.0;
    memset(link->resource_hash, 0, sizeof link->resource_hash);
    memset(link->resource_original_hash, 0,
           sizeof link->resource_original_hash);
}

static void resource_release_segment(rns_runtime_link_t *link) {
    rns_resource_destroy(link->resource);
    link->resource = NULL;
    memset(link->resource_hash, 0, sizeof link->resource_hash);
}

static void outgoing_resource_finish(rns_runtime_resource_transfer_t *transfer,
                                     rns_runtime_resource_state_t state,
                                     rns_status_t status) {
    if (transfer == NULL || transfer->link == NULL) return;
    rns_runtime_link_t *link = transfer->link;
    if (link->outgoing_resource == transfer) link->outgoing_resource = NULL;
    transfer->link = NULL;
    transfer->state = state;
    if (transfer->options.callback != NULL)
        transfer->options.callback(
            transfer, state, status, transfer->sent_parts,
            rns_resource_sender_total_data_parts(transfer->sender),
            transfer->options.callback_context);
    if (transfer->runtime_owned) {
        rns_resource_sender_destroy(transfer->sender);
        free(transfer);
    }
}

static rns_runtime_request_handler_t *find_request_handler(
    const rns_runtime_destination_t *destination,
    const uint8_t path_hash[RNS_REQUEST_ID_LENGTH]) {
    if (destination == NULL) return NULL;
    for (size_t i = 0U; i < RNS_RUNTIME_MAX_REQUEST_HANDLERS; ++i) {
        rns_runtime_request_handler_t *handler =
            destination->request_handlers[i];
        if (handler != NULL &&
            memcmp(handler->path_hash, path_hash,
                   RNS_REQUEST_ID_LENGTH) == 0)
            return handler;
    }
    return NULL;
}

static bool request_handler_authorized(
    const rns_runtime_request_handler_t *handler,
    const rns_runtime_link_t *link) {
    if (handler->options.access == RNS_REQUEST_ALLOW_ALL) return true;
    if (handler->options.access == RNS_REQUEST_ALLOW_NONE) return false;
    if (!link->remote_identified) return false;
    if (handler->options.access == RNS_REQUEST_ALLOW_IDENTIFIED) return true;
    if (handler->options.access != RNS_REQUEST_ALLOW_LIST) return false;
    uint8_t public_key[RNS_IDENTITY_PUBLIC_SIZE], digest[32];
    memcpy(public_key, link->protocol.remote_identity.encryption_public, 32U);
    memcpy(public_key + 32U, link->protocol.remote_identity.signing_public,
           32U);
    if (!rns_sha256(public_key, sizeof public_key, digest)) return false;
    for (size_t i = 0U; i < handler->options.allow_identity_count; ++i)
        if (memcmp(handler->allow_identity_hashes[i], digest, 16U) == 0)
            return true;
    return false;
}

static bool handle_request(rns_runtime_link_t *link, const uint8_t *raw,
                           size_t raw_length, const uint8_t *plaintext,
                           size_t plaintext_length) {
    rns_request_view_t request;
    if (rns_request_decode(plaintext, plaintext_length, &request) != RNS_OK)
        return false;
    rns_runtime_request_handler_t *handler = find_request_handler(
        link->inbound_destination, request.path_hash);
    if (handler == NULL || !request_handler_authorized(handler, link))
        return true;
    size_t response_capacity = handler->options.max_response_size;
    uint8_t *response = malloc(response_capacity);
    if (response == NULL) return true;
    size_t response_length = 0U;
    const rns_identity *remote = link->remote_identified
                                     ? &link->protocol.remote_identity
                                     : NULL;
    rns_status_t status = handler->options.callback(
        handler, link, &request, remote, response, response_capacity,
        &response_length, handler->options.callback_context);
    if (status != RNS_OK || response_length == 0U ||
        response_length > response_capacity) {
        free(response);
        return true;
    }
    uint8_t request_id[RNS_REQUEST_ID_LENGTH];
    if (!rns_packet_truncated_hash(raw, raw_length, request_id)) {
        free(response);
        return true;
    }
    if (response_length > SIZE_MAX - 19U) {
        free(response);
        return true;
    }
    size_t wire_capacity = response_length + 19U;
    uint8_t *wire = malloc(wire_capacity);
    if (wire == NULL) {
        free(response);
        return true;
    }
    size_t wire_length = 0U;
    status = rns_response_encode(request_id, response, response_length, wire,
                                 wire_capacity, &wire_length);
    free(response);
    if (status == RNS_OK)
        status = link_send_plain2(link, RNS_LINK_CONTEXT_RESPONSE, wire,
                                  wire_length);
    if (status == RNS_ERROR_OVERFLOW) {
        rns_runtime_resource_options_t options = {
            .timeout_seconds = link->options.timeout_seconds,
            .auto_compress = true,
            .is_response = true,
            .request_id = request_id};
        rns_runtime_resource_transfer_t *transfer = NULL;
        status = rns_runtime_link_send_resource(link, wire, wire_length,
                                                 &options, &transfer);
        if (status == RNS_OK) transfer->runtime_owned = true;
    }
    free(wire);
    return true;
}

static void incoming_resource_abort(rns_runtime_link_t *link,
                                    rns_status_t status) {
    bool application = link->resource_application;
    uint8_t hash[RNS_RESOURCE_HASH_SIZE];
    memcpy(hash,
           link->resource != NULL ? link->resource_hash
                                  : link->resource_original_hash,
           sizeof hash);
    rns_request_receipt_t *receipt = link->resource_receipt;
    resource_release(link);
    if (receipt != NULL && receipt->state == RNS_REQUEST_PENDING) {
        receipt->state = RNS_REQUEST_FAILED;
        request_notify(receipt, status, NULL, 0U);
    } else if (application && link->options.resource_receive_callback != NULL) {
        link->options.resource_receive_callback(
            link, hash, status, NULL, 0U, link->options.callback_context);
    }
}

static double resource_retry_delay(const rns_runtime_link_t *link) {
    return rns_resource_retry_timeout(
        link->protocol.rtt, link->options.resource_part_airtime_seconds,
        rns_resource_outstanding_parts(link->resource),
        rns_resource_retries_used(link->resource));
}

static void resource_refresh_deadlines(rns_runtime_link_t *link, double now) {
    double retry_delay = resource_retry_delay(link);
    double inactivity = link->options.timeout_seconds > 0.0
                            ? link->options.timeout_seconds : 30.0;
    if (inactivity < retry_delay + 1.0) inactivity = retry_delay + 1.0;
    link->resource_request_retry_at = now + retry_delay;
    link->resource_deadline = now + inactivity;
}

static rns_status_t resource_request_parts(rns_runtime_link_t *link,
                                           bool retry) {
    uint8_t body[RNS_MTU];
    size_t body_length = 0U;
    rns_status_t status = retry
        ? rns_resource_build_retry_request(link->resource, body, sizeof body,
                                           &body_length)
        : rns_resource_build_request(link->resource, body, sizeof body,
                                     &body_length);
    if (status == RNS_ERROR_INVALID_STATE && !retry) return RNS_OK;
    if (status != RNS_OK) return status;
    status = link_send_plain2(link, RNS_LINK_CONTEXT_RESOURCE_REQ, body,
                              body_length);
    if (status == RNS_OK) resource_refresh_deadlines(link, runtime_clock(NULL));
    return status;
}

/* Returns true when the advertisement was taken over by the resource layer. */
static bool resource_advertised(rns_runtime_link_t *link, const uint8_t *plaintext,
                                size_t plaintext_length) {
    rns_resource_advertisement_t advertisement;
    if (rns_resource_advertisement_parse(plaintext, plaintext_length,
                                         &advertisement) != RNS_OK) return false;
    bool continuation = link->resource_assembled != NULL &&
                        link->resource == NULL;
    if (!continuation && advertisement.segment_index != 1U) {
        (void)link_send_plain2(link, RNS_LINK_CONTEXT_RESOURCE_RCL,
                               advertisement.hash, sizeof advertisement.hash);
        return true;
    }
    rns_request_receipt_t *receipt = continuation
        ? link->resource_receipt
        : (advertisement.has_request_id
               ? find_receipt(link, advertisement.request_id) : NULL);
    bool application = continuation ? link->resource_application
                                    : receipt == NULL &&
                                          !advertisement.is_response;
    if (continuation &&
        (advertisement.segment_index != link->resource_next_segment ||
         advertisement.total_segments != link->resource_total_segments ||
         memcmp(advertisement.original_hash, link->resource_original_hash,
                RNS_RESOURCE_HASH_SIZE) != 0 ||
         advertisement.data_size != link->resource_assembled_capacity ||
         (receipt != NULL &&
          (!advertisement.has_request_id ||
           memcmp(advertisement.request_id, receipt->request_id,
                  RNS_REQUEST_ID_LENGTH) != 0)))) {
        (void)link_send_plain2(link, RNS_LINK_CONTEXT_RESOURCE_RCL,
                               advertisement.hash, sizeof advertisement.hash);
        incoming_resource_abort(link, RNS_ERROR_PROTOCOL);
        return true;
    }
    if (!application && receipt == NULL) {
        (void)link_send_plain2(link, RNS_LINK_CONTEXT_RESOURCE_RCL,
                               advertisement.hash, sizeof advertisement.hash);
        return true;
    }
    if (link->resource != NULL) {
        if (memcmp(link->resource_hash, advertisement.hash,
                   RNS_RESOURCE_HASH_SIZE) == 0)
            (void)resource_request_parts(link, false);
        else
            (void)link_send_plain2(link, RNS_LINK_CONTEXT_RESOURCE_RCL,
                                   advertisement.hash,
                                   sizeof advertisement.hash);
        return true;
    }
    if (!continuation && application &&
        (link->options.resource_accept_callback == NULL ||
         !link->options.resource_accept_callback(
             link, &advertisement, link->options.callback_context))) {
        (void)link_send_plain2(link, RNS_LINK_CONTEXT_RESOURCE_RCL,
                               advertisement.hash, sizeof advertisement.hash);
        return true;
    }
    size_t maximum = application ? link->options.max_incoming_resource_size
                                 : receipt->options.max_response_size;
    if (maximum == 0U) maximum = RNS_RESOURCE_DEFAULT_MAX_SIZE;
    rns_status_t accept_status = rns_resource_accept(
        &link->resource, &advertisement, maximum);
    if (accept_status != RNS_OK) {
        link->resource = NULL;
        (void)link_send_plain2(link, RNS_LINK_CONTEXT_RESOURCE_RCL,
                               advertisement.hash, sizeof advertisement.hash);
        if (receipt != NULL) {
            receipt->state = RNS_REQUEST_FAILED;
            request_notify(receipt, accept_status, NULL, 0U);
        } else if (application &&
                   link->options.resource_receive_callback != NULL)
            link->options.resource_receive_callback(
                link, advertisement.hash, accept_status, NULL, 0U,
                link->options.callback_context);
        return true;
    }
    link->resource_receipt = receipt;
    link->resource_application = application;
    memcpy(link->resource_hash, advertisement.hash,
           sizeof link->resource_hash);
    if (!continuation) {
        link->resource_assembled = malloc(advertisement.data_size);
        if (link->resource_assembled == NULL) {
            incoming_resource_abort(link, RNS_ERROR_NO_MEMORY);
            return true;
        }
        link->resource_assembled_capacity = advertisement.data_size;
        link->resource_total_segments = advertisement.total_segments;
        link->resource_next_segment = advertisement.segment_index;
        memcpy(link->resource_original_hash, advertisement.original_hash,
               RNS_RESOURCE_HASH_SIZE);
    }
    link->resource_next_segment = advertisement.segment_index;
    if (resource_request_parts(link, false) != RNS_OK) {
        incoming_resource_abort(link, RNS_ERROR_IO);
        return false;
    }
    return true;
}

static void resource_complete(rns_runtime_link_t *link) {
    rns_request_receipt_t *receipt = link->resource_receipt;
    bool application = link->resource_application;
    uint8_t resource_hash[RNS_RESOURCE_HASH_SIZE];
    memcpy(resource_hash, link->resource_hash, sizeof resource_hash);
    uint8_t proof[RNS_RESOURCE_PROOF_SIZE];
    size_t remaining = link->resource_assembled_capacity -
                       link->resource_assembled_length;
    size_t assembled_length = 0U;
    rns_status_t status = rns_resource_assemble(link->resource, &link->protocol,
                                                link->resource_assembled +
                                                    link->resource_assembled_length,
                                                remaining,
                                                &assembled_length);
    if (status != RNS_OK) {
        incoming_resource_abort(link, status);
        return;
    }
    if (rns_resource_build_proof(link->resource, proof) == RNS_OK)
        (void)link_send_proof(link, RNS_LINK_CONTEXT_RESOURCE_PRF, proof,
                              sizeof proof);
    link->resource_assembled_length += assembled_length;
    if (link->resource_next_segment < link->resource_total_segments) {
        resource_release_segment(link);
        link->resource_next_segment++;
        resource_refresh_deadlines(link, runtime_clock(NULL));
        return;
    }
    if (link->resource_assembled_length != link->resource_assembled_capacity) {
        incoming_resource_abort(link, RNS_ERROR_PROTOCOL);
        return;
    }
    if (application) {
        if (link->options.resource_receive_callback != NULL)
            link->options.resource_receive_callback(
                link, resource_hash, RNS_OK, link->resource_assembled,
                link->resource_assembled_length,
                link->options.callback_context);
    } else {
        /* A response resource carries the same [request id, response] pair. */
        rns_response_view_t response;
        if (rns_response_decode(link->resource_assembled,
                                link->resource_assembled_length,
                                &response) == RNS_OK) {
            receipt->state = RNS_REQUEST_COMPLETE;
            request_notify(receipt, RNS_OK, response.response,
                           response.response_length);
        } else {
            receipt->state = RNS_REQUEST_FAILED;
            request_notify(receipt, RNS_ERROR_PROTOCOL, NULL, 0U);
        }
    }
    resource_release(link);
}

static bool resource_part(rns_runtime_link_t *link, const uint8_t *plaintext,
                          size_t plaintext_length) {
    if (link->resource == NULL) return false;
    if (link->resource_receipt != NULL &&
        link->resource_receipt->state != RNS_REQUEST_PENDING) {
        resource_release(link);
        return false;
    }
    size_t received_before = rns_resource_received_parts(link->resource);
    if (rns_resource_receive_part(link->resource, plaintext, plaintext_length) != RNS_OK)
        return false;
    link->last_inbound = runtime_clock(NULL);
    if (rns_resource_received_parts(link->resource) == received_before)
        return true;
    resource_refresh_deadlines(link, link->last_inbound);
    if (rns_resource_parts_complete(link->resource)) {
        resource_complete(link);
    } else if (resource_request_parts(link, false) != RNS_OK) {
        incoming_resource_abort(link, RNS_ERROR_IO);
    }
    return true;
}

static bool resource_hashmap_update(rns_runtime_link_t *link,
                                    const uint8_t *plaintext,
                                    size_t plaintext_length) {
    if (link->resource == NULL ||
        !rns_resource_waiting_for_hashmap(link->resource))
        return false;
    if (rns_resource_apply_hashmap_update(link->resource, plaintext,
                                          plaintext_length) != RNS_OK) {
        incoming_resource_abort(link, RNS_ERROR_PROTOCOL);
        return true;
    }
    resource_refresh_deadlines(link, runtime_clock(NULL));
    if (resource_request_parts(link, false) != RNS_OK)
        incoming_resource_abort(link, RNS_ERROR_IO);
    return true;
}

static bool outgoing_resource_request(rns_runtime_link_t *link,
                                      const uint8_t *request,
                                      size_t request_length) {
    rns_runtime_resource_transfer_t *transfer = link->outgoing_resource;
    size_t indexes[RNS_RESOURCE_MAX_PARTS];
    size_t count = 0U;
    if (transfer == NULL ||
        rns_resource_sender_requested_parts(transfer->sender, request,
                                            request_length, indexes,
                                            RNS_RESOURCE_MAX_PARTS,
                                            &count) != RNS_OK)
        return false;
    transfer->deadline = runtime_clock(NULL) +
        (transfer->options.timeout_seconds > 0.0
             ? transfer->options.timeout_seconds
             : 30.0);
    for (size_t i = 0U; i < count; ++i) {
        const uint8_t *part = NULL;
        size_t part_length = 0U;
        if (rns_resource_sender_part(transfer->sender, indexes[i], &part,
                                     &part_length) != RNS_OK ||
            link_send_wire(link, RNS_LINK_CONTEXT_RESOURCE, part, part_length,
                           NULL) != RNS_OK) {
            outgoing_resource_finish(transfer, RNS_RUNTIME_RESOURCE_FAILED,
                                     RNS_ERROR_IO);
            return true;
        }
        if (!transfer->part_sent[indexes[i]]) {
            transfer->part_sent[indexes[i]] = true;
            transfer->sent_parts++;
            transfer->sent_segment_bytes += part_length;
        }
    }
    uint8_t update[RNS_MTU];
    size_t update_length = 0U;
    rns_status_t update_status = rns_resource_sender_hashmap_update(
        transfer->sender, request, request_length, update, sizeof update,
        &update_length);
    if (update_status == RNS_OK &&
        link_send_plain2(link, RNS_LINK_CONTEXT_RESOURCE_HMU, update,
                         update_length) != RNS_OK) {
        outgoing_resource_finish(transfer, RNS_RUNTIME_RESOURCE_FAILED,
                                 RNS_ERROR_IO);
        return true;
    }
    if (update_status != RNS_OK && update_status != RNS_ERROR_NOT_FOUND) {
        outgoing_resource_finish(transfer, RNS_RUNTIME_RESOURCE_FAILED,
                                 update_status);
        return true;
    }
    if (count != 0U) {
        if (transfer->state == RNS_RUNTIME_RESOURCE_ADVERTISED)
            transfer->state = RNS_RUNTIME_RESOURCE_TRANSFERRING;
        if (transfer->options.callback != NULL)
            transfer->options.callback(
                transfer, transfer->state, RNS_OK, transfer->sent_parts,
                rns_resource_sender_total_data_parts(transfer->sender),
                transfer->options.callback_context);
    }
    return true;
}

static bool outgoing_resource_proof(rns_runtime_link_t *link,
                                    const uint8_t *proof,
                                    size_t proof_length) {
    rns_runtime_resource_transfer_t *transfer = link->outgoing_resource;
    if (transfer == NULL || proof_length != RNS_RESOURCE_PROOF_SIZE ||
        memcmp(proof, rns_resource_sender_hash(transfer->sender),
               RNS_RESOURCE_HASH_SIZE) != 0)
        return false;
    if (rns_resource_sender_validate_proof(transfer->sender, proof,
                                           proof_length) != RNS_OK)
        return false;
    if (rns_resource_sender_segment_index(transfer->sender) <
        rns_resource_sender_total_segments(transfer->sender)) {
        if (rns_resource_sender_advance_segment(transfer->sender,
                                                &link->protocol) != RNS_OK) {
            outgoing_resource_finish(transfer, RNS_RUNTIME_RESOURCE_FAILED,
                                     RNS_ERROR_CRYPTO);
            return true;
        }
        memset(transfer->part_sent, 0, sizeof transfer->part_sent);
        transfer->sent_segment_bytes = 0U;
        uint8_t advertisement[RNS_MTU];
        size_t advertisement_length = 0U;
        if (rns_resource_sender_advertisement(
                transfer->sender, advertisement, sizeof advertisement,
                &advertisement_length) != RNS_OK ||
            link_send_plain2(link, RNS_LINK_CONTEXT_RESOURCE_ADV,
                             advertisement, advertisement_length) != RNS_OK) {
            outgoing_resource_finish(transfer, RNS_RUNTIME_RESOURCE_FAILED,
                                     RNS_ERROR_IO);
            return true;
        }
        transfer->state = RNS_RUNTIME_RESOURCE_ADVERTISED;
        transfer->deadline = runtime_clock(NULL) +
            (transfer->options.timeout_seconds > 0.0
                 ? transfer->options.timeout_seconds : 30.0);
        return true;
    }
    outgoing_resource_finish(transfer, RNS_RUNTIME_RESOURCE_COMPLETE, RNS_OK);
    return true;
}

static void fail_link_resources(rns_runtime_link_t *link,
                                rns_status_t status) {
    if (link->resource != NULL || link->resource_assembled != NULL)
        incoming_resource_abort(link, status);
    if (link->outgoing_resource != NULL)
        outgoing_resource_finish(link->outgoing_resource,
                                 RNS_RUNTIME_RESOURCE_FAILED, status);
}

static rns_runtime_destination_t *find_link_destination(
    rns_runtime_t *runtime, const uint8_t hash[16]) {
    for (size_t i = 0U; i < RNS_RUNTIME_MAX_DESTINATIONS; ++i) {
        rns_runtime_destination_t *destination = runtime->destinations[i];
        if (destination != NULL && memcmp(destination->hash, hash, 16U) == 0)
            return destination;
    }
    return NULL;
}

static bool accept_inbound_link(rns_runtime_t *runtime, size_t interface_index,
                                const uint8_t *raw, size_t raw_length,
                                const rns_packet *packet, uint8_t received_hops) {
    if (packet->packet_type != 2U || packet->destination_type != 0U) return false;
    rns_runtime_destination_t *destination =
        find_link_destination(runtime, packet->destination_hash);
    if (destination == NULL) return false;
    size_t slot = RNS_RUNTIME_MAX_LINKS;
    for (size_t i = 0U; i < RNS_RUNTIME_MAX_LINKS; ++i)
        if (runtime->links[i] == NULL) { slot = i; break; }
    if (slot == RNS_RUNTIME_MAX_LINKS) return true;
    rns_runtime_link_t *link = calloc(1U, sizeof *link);
    if (link == NULL) return true;
    link->runtime = runtime;
    link->interface_index = interface_index;
    link->options = destination->link_options;
    link->inbound_destination = destination;
    double timeout = link->options.timeout_seconds > 0.0
                         ? link->options.timeout_seconds
                         : 360.0 + 6.0 * (double)(received_hops != 0U
                                                      ? received_hops : 1U);
    if (!rns_link_responder_accept(&link->protocol, &destination->identity, raw,
                                   raw_length, timeout, runtime_clock, NULL)) {
        free(link);
        return true;
    }
    if (!rns_node_register_destination(&runtime->node, link->protocol.link_id)) {
        free(link);
        return true;
    }
    link->registered = true;
    link->keepalive_seconds = 360.0;
    link->last_inbound = link->protocol.request_time;
    link->last_outbound = link->protocol.request_time;
    runtime->links[slot] = link;
    uint8_t proof[RNS_LINK_PROOF_BYTES];
    if (!rns_link_build_proof(&link->protocol, proof) ||
        link_send_proof(link, RNS_LINK_CONTEXT_PROOF, proof, sizeof proof) != RNS_OK) {
        runtime->links[slot] = NULL;
        (void)rns_node_unregister_destination(&runtime->node,
                                              link->protocol.link_id);
        free(link);
        return true;
    }
    destination->accepted_callback(destination, link,
                                   destination->callback_context);
    return true;
}

static bool link_ingress(rns_runtime_t *runtime, size_t interface_index,
                         const uint8_t *raw, size_t raw_length,
                         uint8_t received_hops) {
    rns_packet packet;
    if (!rns_packet_decode(&packet, raw, raw_length)) return false;
    for (size_t i = 0U; i < RNS_RUNTIME_MAX_LINKS; i++) {
        rns_runtime_link_t *link = runtime->links[i];
        if (link == NULL || memcmp(packet.destination_hash,
                                   link->protocol.link_id, 16U) != 0)
            continue;
        if (interface_index != link->interface_index) {
            link->protocol.state = RNS_LINK_CLOSED;
            link_notify(link, RNS_LINK_CLOSED, RNS_ERROR_PROTOCOL);
            return true;
        }
        if (link->protocol.state == RNS_LINK_PENDING && packet.packet_type == 3U &&
            packet.context == RNS_LINK_CONTEXT_PROOF) {
            if (!rns_link_initiator_accept_proof(&link->protocol, packet.data,
                                                 packet.data_length)) {
                link->protocol.state = RNS_LINK_CLOSED;
                fail_pending_requests(link, RNS_ERROR_CRYPTO);
                link_notify(link, RNS_LINK_CLOSED, RNS_ERROR_CRYPTO);
                return true;
            }
            uint8_t rtt[128];
            size_t rtt_length = 0U;
            if (!rns_link_build_rtt_confirm(&link->protocol, rtt, sizeof rtt,
                                            &rtt_length) ||
                link_send_wire(link, RNS_LINK_CONTEXT_RTT, rtt, rtt_length,
                               NULL) != RNS_OK) {
                link->protocol.state = RNS_LINK_CLOSED;
                fail_pending_requests(link, RNS_ERROR_IO);
                link_notify(link, RNS_LINK_CLOSED, RNS_ERROR_IO);
                return true;
            }
            link->last_inbound = runtime_clock(NULL);
            link_update_keepalive(link);
            link_notify(link, RNS_LINK_ACTIVE, RNS_OK);
            return true;
        }
        if (link->protocol.state == RNS_LINK_HANDSHAKE &&
            link->protocol.role == RNS_LINK_RESPONDER &&
            packet.packet_type == 0U && packet.context == RNS_LINK_CONTEXT_RTT) {
            if (!rns_link_responder_accept_rtt(&link->protocol, packet.data,
                                               packet.data_length)) {
                fail_pending_requests(link, RNS_ERROR_CRYPTO);
                link_notify(link, RNS_LINK_CLOSED, RNS_ERROR_CRYPTO);
                return true;
            }
            link->last_inbound = runtime_clock(NULL);
            link_update_keepalive(link);
            link_notify(link, RNS_LINK_ACTIVE, RNS_OK);
            return true;
        }
        if (link->protocol.state == RNS_LINK_ACTIVE && packet.packet_type == 3U &&
            packet.context == RNS_LINK_CONTEXT_RESOURCE_PRF) {
            (void)outgoing_resource_proof(link, packet.data,
                                          packet.data_length);
            return true;
        }
        if (link->protocol.state != RNS_LINK_ACTIVE || packet.packet_type != 0U)
            return true;
        if (packet.context == RNS_LINK_CONTEXT_KEEPALIVE) {
            if (packet.data_length != 1U) return true;
            uint8_t expected = link->protocol.role == RNS_LINK_INITIATOR
                                   ? 0xfeU : 0xffU;
            if (packet.data[0] != expected) return true;
            link->last_inbound = runtime_clock(NULL);
            if (link->protocol.role == RNS_LINK_RESPONDER &&
                link->last_inbound - link->last_outbound >=
                    link->keepalive_seconds) {
                static const uint8_t response = 0xfeU;
                (void)link_send_wire(link, RNS_LINK_CONTEXT_KEEPALIVE,
                                     &response, 1U, NULL);
            }
            return true;
        }
        /*
         * Resource parts, keepalives and cache requests travel unencrypted on
         * an established link: a resource encrypts its payload as a whole, and
         * the others carry no confidential data. Decrypting them fails.
         */
        uint8_t plaintext[RNS_MTU];
        size_t plaintext_length = 0U;
        const uint8_t *payload = packet.data;
        bool link_encrypted = packet.context != RNS_LINK_CONTEXT_RESOURCE &&
                              packet.context != RNS_LINK_CONTEXT_KEEPALIVE &&
                              packet.context != RNS_LINK_CONTEXT_CACHE_REQUEST;
        if (link_encrypted) {
            if (!rns_link_decrypt(&link->protocol, packet.data, packet.data_length,
                                  plaintext, sizeof plaintext, &plaintext_length)) {
                link_notify(link, RNS_LINK_ACTIVE, RNS_ERROR_CRYPTO);
                return true;
            }
            payload = plaintext;
            link->last_inbound = runtime_clock(NULL);
        } else plaintext_length = packet.data_length;
        if (packet.context == RNS_LINK_CONTEXT_CLOSE) {
            link->protocol.state = RNS_LINK_CLOSED;
            fail_pending_requests(link, RNS_ERROR_INVALID_STATE);
            fail_link_resources(link, RNS_ERROR_INVALID_STATE);
            link_notify(link, RNS_LINK_CLOSED, RNS_OK);
        } else if (packet.context == RNS_LINK_CONTEXT_IDENTIFY) {
            if (link->protocol.role != RNS_LINK_RESPONDER ||
                plaintext_length != RNS_IDENTITY_PUBLIC_SIZE + 64U)
                return true;
            rns_identity identified;
            uint8_t signed_data[16U + RNS_IDENTITY_PUBLIC_SIZE];
            if (!rns_identity_from_public(&identified, payload)) return true;
            memcpy(signed_data, link->protocol.link_id, 16U);
            memcpy(signed_data + 16U, payload, RNS_IDENTITY_PUBLIC_SIZE);
            if (!rns_identity_verify(&identified, signed_data,
                                     sizeof signed_data,
                                     payload + RNS_IDENTITY_PUBLIC_SIZE))
                return true;
            if (!link->remote_identified) {
                link->protocol.remote_identity = identified;
                link->remote_identified = true;
                if (link->options.identified_callback != NULL)
                    link->options.identified_callback(
                        link, &link->protocol.remote_identity,
                        link->options.callback_context);
            }
        } else if (packet.context == RNS_LINK_CONTEXT_REQUEST) {
            (void)handle_request(link, raw, raw_length, payload,
                                 plaintext_length);
        } else if (packet.context == RNS_LINK_CONTEXT_RESPONSE) {
            (void)handle_response(link, payload, plaintext_length);
        } else if (packet.context == RNS_LINK_CONTEXT_RESOURCE_ADV) {
            (void)resource_advertised(link, payload, plaintext_length);
        } else if (packet.context == RNS_LINK_CONTEXT_RESOURCE_REQ) {
            (void)outgoing_resource_request(link, payload, plaintext_length);
        } else if (packet.context == RNS_LINK_CONTEXT_RESOURCE_HMU) {
            (void)resource_hashmap_update(link, payload, plaintext_length);
        } else if (packet.context == RNS_LINK_CONTEXT_RESOURCE &&
                   link->resource != NULL) {
            (void)resource_part(link, payload, plaintext_length);
        } else if (packet.context == RNS_LINK_CONTEXT_RESOURCE_ICL) {
            if (link->resource != NULL &&
                plaintext_length == RNS_RESOURCE_HASH_SIZE &&
                memcmp(payload, link->resource_hash,
                       RNS_RESOURCE_HASH_SIZE) == 0)
                incoming_resource_abort(link, RNS_ERROR_INVALID_STATE);
        } else if (packet.context == RNS_LINK_CONTEXT_RESOURCE_RCL) {
            if (link->outgoing_resource != NULL &&
                plaintext_length == RNS_RESOURCE_HASH_SIZE &&
                memcmp(payload,
                       rns_resource_sender_hash(
                           link->outgoing_resource->sender),
                       RNS_RESOURCE_HASH_SIZE) == 0)
                outgoing_resource_finish(link->outgoing_resource,
                                         RNS_RUNTIME_RESOURCE_REJECTED,
                                         RNS_ERROR_INVALID_STATE);
        } else if (link_encrypted &&
                   link->options.packet_callback != NULL) {
            if (!rns_packet_hash(raw, raw_length, link->callback_packet_hash))
                return true;
            link->callback_packet_provable = true;
            if (packet.context == 0U && link->options.prove_data_packets)
                (void)rns_runtime_link_prove_current_packet(link);
            link->options.packet_callback(link, packet.context, payload,
                                          plaintext_length,
                                          link->options.callback_context);
            link->callback_packet_provable = false;
            memset(link->callback_packet_hash, 0,
                   sizeof link->callback_packet_hash);
        }
        return true;
    }
    return accept_inbound_link(runtime, interface_index, raw, raw_length,
                               &packet, received_hops);
}

static runtime_local_announce_t *local_announce(rns_runtime_t *runtime,
    const uint8_t hash[16], bool create) {
    bool registered = false;
    for (size_t i = 0; i < runtime->node.local_destination_count; ++i)
        if (memcmp(runtime->node.local_destinations + i * 16u, hash, 16u) == 0)
            registered = true;
    if (!registered) return NULL;
    runtime_local_announce_t *empty = NULL;
    for (size_t i = 0; i < runtime->plain_destination_capacity; ++i) {
        runtime_local_announce_t *entry = &runtime->local_announces[i];
        if (entry->used && memcmp(entry->hash, hash, 16u) == 0) return entry;
        if (!entry->used && empty == NULL) empty = entry;
    }
    return create ? empty : NULL;
}

static void forget_local_announce(rns_runtime_t *runtime, const uint8_t hash[16]) {
    for (size_t i = 0; i < runtime->plain_destination_capacity; ++i)
        if (runtime->local_announces[i].used &&
            memcmp(runtime->local_announces[i].hash, hash, 16u) == 0)
            rns_hal_secure_zero(&runtime->local_announces[i], sizeof runtime->local_announces[i]);
}

static void respond_local_path(rns_runtime_t *runtime, size_t interface_index,
    const rns_node_result *request) {
    runtime_local_announce_t *entry = local_announce(runtime,
        request->path_request.destination_hash, false);
    double now = runtime_clock(NULL);
    if (entry == NULL || now < entry->next_response_at) return;
    rns_announce previous;
    if (!rns_announce_parse(&previous, entry->body, entry->body_length, entry->context_flag)) return;
    uint8_t prefix[5], body[RNS_ANNOUNCE_MAX_BODY_SIZE], raw[RNS_MTU];
    uint64_t wall_ms;
    size_t body_length, raw_length;
    rns_packet response = {0};
    if (rns_hal_random_bytes(prefix, sizeof prefix) != RNS_OK ||
        rns_hal_wallclock_ms(&wall_ms) != RNS_OK ||
        !rns_announce_build(&entry->identity, entry->hash, previous.name_hash,
            prefix, wall_ms / 1000u, previous.ratchet, previous.app_data,
            previous.app_data_length, body, sizeof body, &body_length, &response.context_flag)) return;
    memcpy(response.destination_hash, entry->hash, 16u);
    response.packet_type = 1u;
    response.context = 0x0bu; /* Pinned Reticulum PATH_RESPONSE. */
    response.data = body; response.data_length = body_length;
    if (!rns_packet_encode(&response, raw, sizeof raw, &raw_length)) return;
    /* Bounded amplification: one response per second per advertised service.
     * Exact duplicate requests are already suppressed by node ingress. */
    entry->next_response_at = now + 1.0;
    (void)send_internal(runtime, interface_index, raw, raw_length);
}

static bool broadcast_enabled(const runtime_interface_t *interface) {
    rns_interface_stats_t stats;
    return interface->provider == NULL ||
        (rns_interface_get_stats(interface->provider, &stats) == RNS_OK &&
         stats.online && stats.outbound && stats.broadcast);
}

static rns_status_t ingress(receive_context_t *context, const uint8_t *packet, size_t length) {
    rns_runtime_t *runtime = context->runtime;
    runtime_interface_t *source = &runtime->interfaces[context->interface_index];
    uint8_t output[RNS_MTU];
    rns_node_result result;
    source->info.packets_received++;
    source->info.bytes_received += length;
    context->processed++;
    if (packet_receipt_ingress(runtime, packet, length)) return RNS_OK;
    if (!rns_node_ingress(&runtime->node, packet, length, source->info.id, 0,
                          output, sizeof(output), &result)) return RNS_ERROR_INVALID_STATE;
    if (result.action == RNS_NODE_DROP) source->info.packets_dropped++;
    if (result.action == RNS_NODE_PATH_RESPONSE && result.has_path_request)
        respond_local_path(runtime, context->interface_index, &result);
    bool handled_link = result.action == RNS_NODE_DELIVER &&
                        link_ingress(runtime, context->interface_index, packet, length,
                                     result.hops);
    if (!handled_link && runtime->packet_callback != NULL &&
        (result.action == RNS_NODE_DELIVER || result.action == RNS_NODE_PATH_RESPONSE))
        runtime->packet_callback(runtime, packet, length, &result, runtime->callback_context);
    if (result.has_verified_announce && runtime->announce_callback != NULL)
        runtime->announce_callback(runtime, &result, runtime->callback_context);
    if (result.action == RNS_NODE_FORWARD) {
        bool forwarded = false;
        if (runtime->config.enable_transport || runtime->shared_server)
            for (size_t i = 0U; i < runtime->interface_count; ++i)
                if (runtime->interfaces[i].info.id ==
                    result.forward_interface_id) {
                    forwarded = send_internal(runtime, i, output,
                                              result.output_length) == RNS_OK;
                    break;
                }
        if (!rns_node_complete_forward(&runtime->node, &result,
                                       forwarded ? 1 : 0))
            return RNS_ERROR_INVALID_STATE;
        if (!forwarded) source->info.packets_dropped++;
    } else if ((runtime->config.enable_transport || runtime->shared_server) &&
               result.action == RNS_NODE_REBROADCAST) {
        for (size_t i = 0U; i < runtime->interface_count; ++i)
            if ((i != context->interface_index || source->local_server ||
                 source->same_interface_rebroadcast) &&
                broadcast_enabled(&runtime->interfaces[i]))
                (void)send_internal(runtime, i, output,
                                    result.output_length);
    }
    return RNS_OK;
}

static rns_status_t provider_receive(void *opaque, const uint8_t *packet,
                                      size_t length) {
    receive_context_t *context = opaque;
    runtime_interface_t *source = &context->runtime->interfaces[context->interface_index];
    if (context->processed >= context->remaining) {
        source->info.packets_dropped++;
        source->info.ingress_error = RNS_ERROR_OVERFLOW;
        context->provider_budget_exceeded = true;
        return RNS_ERROR_OVERFLOW;
    }
    if (packet == NULL || length == 0U || length > RNS_MTU) {
        source->info.packets_dropped++;
        source->info.ingress_error = RNS_ERROR_OVERFLOW;
        if (context->processed < context->remaining) context->processed++;
        return RNS_OK;
    }
    rns_status_t status = ingress(context, packet, length);
    if (status != RNS_OK) {
        source->info.packets_dropped++;
        source->info.ingress_error = status;
    }
    return RNS_OK;
}

static rns_status_t udp_receive(const uint8_t *packet, size_t length,
                                const rns_udp_address_t *source, void *opaque) {
    (void)source;
    return ingress((receive_context_t *)opaque, packet, length);
}

static rns_status_t tcp_receive(const uint8_t *packet, size_t length, void *opaque) {
    return ingress((receive_context_t *)opaque, packet, length);
}

static rns_status_t auto_receive(const uint8_t *packet, size_t length,
                                 const rns_udp_address_t *source,
                                 uint32_t interface_index, void *opaque) {
    (void)source;
    (void)interface_index;
    return ingress((receive_context_t *)opaque, packet, length);
}

static double auto_clock(void *context) {
    return reconnect_clock((const rns_runtime_t *)context);
}

static rns_status_t start_interface(rns_runtime_t *runtime,
                                    runtime_interface_t *destination,
                                    const rns_config_interface_t *source,
                                    uint64_t id) {
    rns_status_t status = RNS_ERROR_UNSUPPORTED;
    memset(destination, 0, sizeof(*destination));
    destination->info.id = id;
    destination->info.type = source->type;
    destination->info.state = source->enabled ? RNS_RUNTIME_INTERFACE_STARTING : RNS_RUNTIME_INTERFACE_DISABLED;
    (void)memcpy(destination->info.name, source->name, sizeof(destination->info.name));
    if (!source->enabled) return RNS_OK;
    if (source->type == RNS_CONFIG_UDP) {
        const char *listen_ip = source->listen_ip[0] != '\0' ? source->listen_ip : "0.0.0.0";
        status = rns_udp_endpoint_create(&destination->udp, RNS_UDP_IPV4);
        if (status == RNS_OK) status = rns_udp_bind(destination->udp, listen_ip, source->listen_port);
        if (status == RNS_OK)
            status = rns_udp_resolve(source->forward_ip, source->forward_port,
                                     RNS_UDP_IPV4, &destination->udp_forward);
        if (status == RNS_OK) destination->has_udp_forward = true;
    } else if (source->type == RNS_CONFIG_TCP_CLIENT || source->type == RNS_CONFIG_TCP_SERVER) {
        status = rns_tcp_endpoint_create(&destination->tcp, RNS_UDP_IPV4, RUNTIME_TCP_QUEUE);
        if (status == RNS_OK && source->type == RNS_CONFIG_TCP_CLIENT) {
            destination->info.connection_attempts++;
            status = rns_tcp_connect(destination->tcp, source->target_host, source->target_port);
        } else if (status == RNS_OK) {
            const char *listen_ip = source->listen_ip[0] != '\0' ? source->listen_ip : "0.0.0.0";
            status = rns_tcp_listen(destination->tcp, listen_ip, source->listen_port, 8);
        }
    } else if (source->type == RNS_CONFIG_KISS) {
        rns_kiss_options_t options;
        rns_kiss_options_init(&options);
        options.device = source->device;
        if (source->speed != 0U) options.speed = source->speed;
        if (source->data_bits != 0U) options.data_bits = source->data_bits;
        if (source->parity != '\0') options.parity = source->parity;
        if (source->stop_bits != 0U) options.stop_bits = source->stop_bits;
        options.preamble_ms = source->preamble_ms;
        options.tx_tail_ms = source->tx_tail_ms;
        options.persistence = source->persistence;
        options.slot_time_ms = source->slot_time_ms;
        options.flow_control = source->flow_control;
        options.clock = runtime->reconnect_clock;
        options.clock_context = runtime->reconnect_clock_context;
        status = rns_kiss_endpoint_create(&destination->kiss, &options);
        if (status == RNS_OK) {
            rns_kiss_info_t info;
            if (rns_kiss_endpoint_info(destination->kiss, &info) == RNS_OK &&
                info.state == RNS_KISS_DOWN)
                status = info.last_error;
        }
    } else if (source->type == RNS_CONFIG_RNODE) {
        rns_rnode_options_t options;
        rns_rnode_options_init(&options);
        options.device = source->device;
        options.frequency = source->frequency;
        options.bandwidth = source->bandwidth;
        options.tx_power = (uint8_t)source->tx_power;
        options.spreading_factor = source->spreading_factor;
        options.coding_rate = source->coding_rate;
        options.flow_control = source->flow_control;
        options.short_airtime_limit_set = source->short_airtime_limit_set;
        options.long_airtime_limit_set = source->long_airtime_limit_set;
        options.short_airtime_limit_hundredths =
            source->short_airtime_limit_hundredths;
        options.long_airtime_limit_hundredths =
            source->long_airtime_limit_hundredths;
        options.clock = runtime->reconnect_clock;
        options.clock_context = runtime->reconnect_clock_context;
        status = rns_rnode_endpoint_create(&destination->rnode, &options);
        if (status == RNS_OK) {
            rns_rnode_info_t info;
            if (rns_rnode_endpoint_info(destination->rnode, &info) == RNS_OK &&
                info.state == RNS_RNODE_DOWN)
                status = info.last_error;
        }
    } else if (source->type == RNS_CONFIG_AUTO) {
        status = rns_auto_posix_create(&destination->auto_interface, source,
                                       auto_clock, runtime);
    }
    destination->info.last_error = status;
    if (status == RNS_ERROR_UNSUPPORTED) destination->info.state = RNS_RUNTIME_INTERFACE_UNSUPPORTED;
    else if (source->type == RNS_CONFIG_TCP_CLIENT) {
        if (status == RNS_OK && rns_tcp_state(destination->tcp) == RNS_TCP_CONNECTED)
            tcp_mark_connected(runtime, destination);
        else if (status == RNS_OK)
            destination->info.state = RNS_RUNTIME_INTERFACE_STARTING;
        else {
            destination->info.state = RNS_RUNTIME_INTERFACE_DOWN;
            tcp_schedule_reconnect(runtime, destination);
        }
    } else if (source->type == RNS_CONFIG_KISS && status == RNS_OK) {
        rns_kiss_info_t info;
        if (rns_kiss_endpoint_info(destination->kiss, &info) == RNS_OK) {
            destination->info.state = info.state == RNS_KISS_UP
                                          ? RNS_RUNTIME_INTERFACE_UP
                                          : (info.state == RNS_KISS_CONFIGURING
                                                 ? RNS_RUNTIME_INTERFACE_STARTING
                                                 : RNS_RUNTIME_INTERFACE_DOWN);
            destination->info.last_error = info.last_error;
        }
    } else if (source->type == RNS_CONFIG_RNODE && status == RNS_OK) {
        rns_rnode_info_t info;
        if (rns_rnode_endpoint_info(destination->rnode, &info) == RNS_OK) {
            destination->info.state = info.state == RNS_RNODE_UP
                                          ? RNS_RUNTIME_INTERFACE_UP
                                          : (info.state == RNS_RNODE_DOWN
                                                 ? RNS_RUNTIME_INTERFACE_DOWN
                                                 : RNS_RUNTIME_INTERFACE_STARTING);
            destination->info.last_error = info.last_error;
        }
    } else if (source->type == RNS_CONFIG_AUTO && status == RNS_OK) {
        destination->info.state = rns_auto_posix_online(destination->auto_interface)
                                      ? RNS_RUNTIME_INTERFACE_UP
                                      : RNS_RUNTIME_INTERFACE_STARTING;
    } else destination->info.state = status == RNS_OK ? RNS_RUNTIME_INTERFACE_UP : RNS_RUNTIME_INTERFACE_DOWN;
    return status;
}

static rns_status_t start_local_interface(rns_runtime_t *runtime,
                                          const rns_config_t *config,
                                          bool *shared_client) {
    rns_local_options_t options;
    rns_local_info_t info;
    size_t index = config->interface_count;
    rns_status_t status = rns_local_options_from_config(
        config, RNS_LOCAL_ROLE_AUTO, &options);
    options.clock = runtime->reconnect_clock;
    options.clock_context = runtime->reconnect_clock_context;
    if (status == RNS_OK)
        status = rns_local_instance_create(&runtime->interfaces[index].local,
                                           &options);
    if (status == RNS_OK)
        status = rns_local_instance_info(runtime->interfaces[index].local,
                                         &info);
    if (status != RNS_OK) return status;
    runtime_interface_t *interface = &runtime->interfaces[index];
    interface->info.id = index + 1U;
    interface->info.type = info.role == RNS_LOCAL_ROLE_SERVER
                               ? RNS_CONFIG_TCP_SERVER
                               : RNS_CONFIG_TCP_CLIENT;
    interface->info.state = info.state == RNS_LOCAL_UP
                                ? RNS_RUNTIME_INTERFACE_UP
                                : (info.state == RNS_LOCAL_STARTING
                                       ? RNS_RUNTIME_INTERFACE_STARTING
                                       : RNS_RUNTIME_INTERFACE_DOWN);
    interface->info.last_error = info.last_error;
    (void)memcpy(interface->info.name, "Local shared instance",
                 sizeof("Local shared instance"));
    interface->local_server = info.role == RNS_LOCAL_ROLE_SERVER;
    runtime->shared_server = interface->local_server;
    *shared_client = !interface->local_server;
    runtime->interface_count++;
    return RNS_OK;
}

rns_status_t rns_runtime_create(rns_runtime_t **output, const rns_config_t *config,
                                const rns_runtime_options_t *options) {
    rns_runtime_t *runtime;
    rns_node_config node_config;
    rns_status_t first_error = RNS_OK;
    if (output == NULL || config == NULL || config->interface_count > RNS_CONFIG_MAX_INTERFACES)
        return RNS_ERROR_INVALID_ARGUMENT;
    *output = NULL;
    runtime = calloc(1U, sizeof(*runtime));
    if (runtime == NULL) return RNS_ERROR_NO_MEMORY;
    runtime->config = *config;
    runtime->interface_count = config->interface_count;
    runtime->tcp_reconnect_initial = RUNTIME_TCP_RECONNECT_INITIAL;
    runtime->tcp_reconnect_max = RUNTIME_TCP_RECONNECT_MAX;
    if (options != NULL) {
        runtime->packet_callback = options->packet_callback;
        runtime->announce_callback = options->announce_callback;
        runtime->callback_context = options->callback_context;
        runtime->reconnect_clock = options->reconnect_clock;
        runtime->reconnect_clock_context = options->reconnect_clock_context;
        if (options->tcp_reconnect_initial_seconds > 0.0)
            runtime->tcp_reconnect_initial =
                options->tcp_reconnect_initial_seconds;
        if (options->tcp_reconnect_max_seconds > 0.0)
            runtime->tcp_reconnect_max = options->tcp_reconnect_max_seconds;
    }
    if (runtime->tcp_reconnect_max < runtime->tcp_reconnect_initial) {
        free(runtime);
        return RNS_ERROR_INVALID_ARGUMENT;
    }
    memset(&node_config, 0, sizeof(node_config));
    node_config.transport.path_capacity = options != NULL && options->path_capacity ? options->path_capacity : 128U;
    node_config.transport.dedupe_capacity = options != NULL && options->dedupe_capacity ? options->dedupe_capacity : 256U;
    node_config.transport.reverse_capacity = options != NULL && options->reverse_capacity ? options->reverse_capacity : 256U;
    node_config.transport.link_capacity =
        options != NULL && options->transport_link_capacity
            ? options->transport_link_capacity
            : RNS_TRANSPORT_DEFAULT_LINK_CAPACITY;
    node_config.transport.random_blob_history = 8U;
    node_config.transport.path_lifetime = 604800.0;
    node_config.transport.dedupe_lifetime = 60.0;
    node_config.transport.reverse_lifetime = RNS_TRANSPORT_REVERSE_TIMEOUT;
    node_config.transport.link_lifetime = RNS_TRANSPORT_LINK_TIMEOUT;
    node_config.transport.link_proof_timeout_per_hop =
        RNS_TRANSPORT_LINK_PROOF_TIMEOUT_PER_HOP;
    node_config.transport.clock = runtime_clock;
    {
        static const char *const path_aspects[] = {"path", "request"};
        if (!rns_destination_hash(NULL, "rnstransport", path_aspects, 2U,
                                  node_config.path_request_destination)) {
            free(runtime);
            return RNS_ERROR_NO_MEMORY;
        }
    }
    node_config.max_hops = 128U;
    node_config.local_destination_capacity = options != NULL && options->local_destination_capacity
                                               ? options->local_destination_capacity : 32U;
    if (rns_hal_random_bytes(node_config.transport_id, sizeof(node_config.transport_id)) != RNS_OK ||
        !rns_node_init(&runtime->node, &node_config)) {
        free(runtime);
        return RNS_ERROR_NO_MEMORY;
    }
    runtime->plain_destinations = calloc(node_config.local_destination_capacity,
                                         16U);
    if (runtime->plain_destinations == NULL) {
        rns_node_free(&runtime->node);
        free(runtime);
        return RNS_ERROR_NO_MEMORY;
    }
    runtime->plain_destination_capacity = node_config.local_destination_capacity;
    runtime->local_announces = calloc(runtime->plain_destination_capacity,
                                       sizeof *runtime->local_announces);
    if (runtime->local_announces == NULL) {
        free(runtime->plain_destinations);
        rns_node_free(&runtime->node);
        free(runtime);
        return RNS_ERROR_NO_MEMORY;
    }
    bool shared_client = false;
    if (config->share_instance_configured && config->share_instance) {
        rns_status_t status = start_local_interface(runtime, config,
                                                    &shared_client);
        if (status != RNS_OK) {
            rns_runtime_destroy(runtime);
            return status;
        }
    }
    for (size_t i = 0U; i < config->interface_count; ++i) {
        rns_status_t status;
        if (shared_client) {
            runtime_interface_t *interface = &runtime->interfaces[i];
            interface->info.id = i + 1U;
            interface->info.type = config->interfaces[i].type;
            interface->info.state = RNS_RUNTIME_INTERFACE_DISABLED;
            (void)memcpy(interface->info.name, config->interfaces[i].name,
                         sizeof(interface->info.name));
            continue;
        }
        status = start_interface(runtime, &runtime->interfaces[i],
                                 &config->interfaces[i], i + 1U);
        if (status != RNS_OK && first_error == RNS_OK) first_error = status;
        if (status != RNS_OK && config->panic_on_interface_error) {
            rns_runtime_destroy(runtime);
            return status;
        }
    }
    *output = runtime;
    (void)first_error; /* Per-interface diagnostics remain queryable in tolerant mode. */
    return RNS_OK;
}

void rns_runtime_destroy(rns_runtime_t *runtime) {
    if (runtime == NULL) return;
    for (size_t i = 0U; i < RNS_RUNTIME_MAX_PACKET_RECEIPTS; ++i) {
        free(runtime->packet_receipts[i]);
        runtime->packet_receipts[i] = NULL;
    }
    for (size_t i = 0U; i < RNS_RUNTIME_MAX_LINKS; i++) {
        if (runtime->links[i] != NULL)
            rns_runtime_link_destroy(runtime->links[i]);
    }
    for (size_t i = 0U; i < RNS_RUNTIME_MAX_DESTINATIONS; ++i) {
        if (runtime->destinations[i] != NULL)
            for (size_t j = 0U; j < RNS_RUNTIME_MAX_REQUEST_HANDLERS; ++j)
                free(runtime->destinations[i]->request_handlers[j]);
        free(runtime->destinations[i]);
        runtime->destinations[i] = NULL;
    }
    for (size_t i = 0U; i < runtime->interface_count; ++i) {
        rns_interface_destroy(runtime->interfaces[i].provider);
        rns_udp_endpoint_destroy(runtime->interfaces[i].udp);
        rns_tcp_endpoint_destroy(runtime->interfaces[i].accepted);
        rns_tcp_endpoint_destroy(runtime->interfaces[i].tcp);
        rns_local_instance_destroy(runtime->interfaces[i].local);
        rns_kiss_endpoint_destroy(runtime->interfaces[i].kiss);
        rns_rnode_endpoint_destroy(runtime->interfaces[i].rnode);
        rns_auto_posix_destroy(runtime->interfaces[i].auto_interface);
    }
    free(runtime->plain_destinations);
    runtime->plain_destinations = NULL;
    rns_hal_secure_zero(runtime->local_announces,
        runtime->plain_destination_capacity * sizeof *runtime->local_announces);
    free(runtime->local_announces);
    rns_node_free(&runtime->node);
    free(runtime);
}

rns_status_t rns_runtime_attach_interface(rns_runtime_t *runtime,
    rns_interface_t *provider, const char *name, const char *kind,
    bool same_interface_rebroadcast, size_t *interface_index) {
    if (runtime == NULL || provider == NULL || name == NULL || kind == NULL ||
        interface_index == NULL) return RNS_ERROR_INVALID_ARGUMENT;
    size_t name_length = strnlen(name, RNS_CONFIG_NAME_MAX);
    size_t kind_length = strnlen(kind, RNS_CONFIG_NAME_MAX);
    if (name_length == 0U || name_length == RNS_CONFIG_NAME_MAX ||
        kind_length == 0U || kind_length == RNS_CONFIG_NAME_MAX)
        return RNS_ERROR_INVALID_ARGUMENT;
    if (runtime->interface_count >= RNS_CONFIG_MAX_INTERFACES + 1U)
        return RNS_ERROR_OVERFLOW;
    rns_status_t status = rns_interface_claim(provider);
    if (status != RNS_OK) return status;
    size_t index = runtime->interface_count;
    runtime_interface_t *entry = &runtime->interfaces[index];
    memset(entry, 0, sizeof *entry);
    entry->provider = provider;
    entry->info.type = RNS_CONFIG_PROVIDER;
    entry->same_interface_rebroadcast = same_interface_rebroadcast;
    entry->info.id = index + 1U;
    memcpy(entry->info.name, name, name_length + 1U);
    memcpy(entry->info.provider_kind, kind, kind_length + 1U);
    rns_interface_stats_t stats;
    rns_status_t stats_status = rns_interface_get_stats(provider, &stats);
    entry->info.last_error = stats_status;
    entry->info.state = stats_status != RNS_OK ? RNS_RUNTIME_INTERFACE_DOWN :
        stats.online ? RNS_RUNTIME_INTERFACE_UP : RNS_RUNTIME_INTERFACE_STARTING;
    runtime->interface_count++;
    *interface_index = index;
    return RNS_OK;
}

rns_status_t rns_runtime_interface_provider_stats(const rns_runtime_t *runtime,
    size_t index, rns_interface_stats_t *stats) {
    if (runtime == NULL || stats == NULL || index >= runtime->interface_count)
        return RNS_ERROR_INVALID_ARGUMENT;
    if (runtime->interfaces[index].provider == NULL) return RNS_ERROR_UNSUPPORTED;
    return rns_interface_get_stats(runtime->interfaces[index].provider, stats);
}

static rns_status_t poll_tcp(rns_runtime_t *runtime, size_t index, receive_context_t *context) {
    runtime_interface_t *interface = &runtime->interfaces[index];
    const rns_config_interface_t *configuration =
        &runtime->config.interfaces[index];
    rns_status_t status;
    size_t bytes = 0U;
    if (configuration->type == RNS_CONFIG_TCP_SERVER &&
        interface->accepted == NULL) {
        status = rns_tcp_accept(interface->tcp, &interface->accepted, RUNTIME_TCP_QUEUE);
        if (status == RNS_OK) {
            interface->info.connections_established++;
            interface->info.last_error = RNS_OK;
        } else if (status != RNS_ERROR_TIMEOUT) {
            interface->info.last_error = status;
            return status;
        }
    }
    if (configuration->type == RNS_CONFIG_TCP_CLIENT &&
        rns_tcp_state(interface->tcp) == RNS_TCP_DISCONNECTED) {
        if (interface->reconnect_at > reconnect_clock(runtime)) return RNS_OK;
        interface->info.connection_attempts++;
        interface->info.state = RNS_RUNTIME_INTERFACE_STARTING;
        status = rns_tcp_connect(interface->tcp, configuration->target_host,
                                 configuration->target_port);
        if (status != RNS_OK) {
            tcp_mark_client_down(runtime, interface, status);
            return status;
        }
        if (rns_tcp_state(interface->tcp) == RNS_TCP_CONNECTED)
            tcp_mark_connected(runtime, interface);
    }
    if (configuration->type == RNS_CONFIG_TCP_CLIENT &&
        rns_tcp_state(interface->tcp) == RNS_TCP_CONNECTING) {
        status = rns_tcp_finish_connect(interface->tcp);
        if (status == RNS_OK)
            tcp_mark_connected(runtime, interface);
        else if (status != RNS_ERROR_TIMEOUT) {
            tcp_mark_client_down(runtime, interface, status);
            return status;
        }
    }
    rns_tcp_endpoint_t *connection = interface->accepted != NULL ? interface->accepted : interface->tcp;
    if (connection == NULL || rns_tcp_state(connection) != RNS_TCP_CONNECTED) return RNS_OK;
    status = rns_tcp_poll_receive(connection, tcp_receive, context, &bytes);
    interface->info.bytes_received += 0U; /* ingress accounts decoded bytes. */
    if (status == RNS_OK) status = rns_tcp_flush(connection, &bytes);
    if (status != RNS_OK && rns_tcp_state(connection) == RNS_TCP_DISCONNECTED) {
        interface->info.connections_lost++;
        if (configuration->type == RNS_CONFIG_TCP_SERVER) {
            rns_tcp_endpoint_destroy(interface->accepted);
            interface->accepted = NULL;
            /* Losing a child connection does not take down the listener. */
            interface->info.state = RNS_RUNTIME_INTERFACE_UP;
            interface->info.last_error = RNS_OK;
            return RNS_OK;
        }
        tcp_mark_client_down(runtime, interface, status);
    }
    return status;
}

static rns_status_t poll_local(runtime_interface_t *interface,
                               receive_context_t *context) {
    rns_status_t status = rns_local_instance_poll(interface->local,
                                                   tcp_receive, context);
    rns_local_info_t info;
    if (rns_local_instance_info(interface->local, &info) == RNS_OK) {
        interface->info.state = info.state == RNS_LOCAL_UP
                                    ? RNS_RUNTIME_INTERFACE_UP
                                    : (info.state == RNS_LOCAL_STARTING
                                           ? RNS_RUNTIME_INTERFACE_STARTING
                                           : RNS_RUNTIME_INTERFACE_DOWN);
        interface->info.last_error = info.last_error;
        interface->info.packets_received = info.packets_received;
        interface->info.packets_sent = info.packets_sent;
        interface->info.bytes_received = info.bytes_received;
        interface->info.bytes_sent = info.bytes_sent;
        interface->info.packets_dropped = info.packets_dropped;
        interface->info.connection_attempts = info.connection_attempts;
        interface->info.connections_established = info.connections_established;
        interface->info.connections_lost = info.connections_lost;
    }
    return status;
}

static rns_status_t poll_kiss(runtime_interface_t *interface,
                              receive_context_t *context) {
    size_t processed = 0U;
    rns_status_t status = rns_kiss_endpoint_poll(
        interface->kiss, context->remaining, tcp_receive, context, &processed);
    (void)processed; /* ingress updates the shared processed count. */
    rns_kiss_info_t info;
    if (rns_kiss_endpoint_info(interface->kiss, &info) == RNS_OK) {
        interface->info.state = info.state == RNS_KISS_UP
                                    ? RNS_RUNTIME_INTERFACE_UP
                                    : (info.state == RNS_KISS_CONFIGURING
                                           ? RNS_RUNTIME_INTERFACE_STARTING
                                           : RNS_RUNTIME_INTERFACE_DOWN);
        interface->info.last_error = info.last_error;
        interface->info.connection_attempts = info.connection_attempts;
        interface->info.connections_established = info.connections_established;
        interface->info.connections_lost = info.connections_lost;
        if (info.packets_dropped >= interface->kiss_reported_drops)
            interface->info.packets_dropped +=
                info.packets_dropped - interface->kiss_reported_drops;
        interface->kiss_reported_drops = info.packets_dropped;
    }
    return status;
}

static rns_status_t poll_rnode(runtime_interface_t *interface,
                               receive_context_t *context) {
    size_t processed = 0U;
    rns_status_t status = rns_rnode_endpoint_poll(
        interface->rnode, context->remaining, tcp_receive, context, &processed);
    (void)processed;
    rns_rnode_info_t info;
    if (rns_rnode_endpoint_info(interface->rnode, &info) == RNS_OK) {
        interface->info.state = info.state == RNS_RNODE_UP
                                    ? RNS_RUNTIME_INTERFACE_UP
                                    : (info.state == RNS_RNODE_DOWN
                                           ? RNS_RUNTIME_INTERFACE_DOWN
                                           : RNS_RUNTIME_INTERFACE_STARTING);
        interface->info.last_error = info.last_error;
        interface->info.connection_attempts = info.connection_attempts;
        interface->info.connections_established = info.connections_established;
        interface->info.connections_lost = info.connections_lost;
        if (info.packets_dropped >= interface->rnode_reported_drops)
            interface->info.packets_dropped +=
                info.packets_dropped - interface->rnode_reported_drops;
        interface->rnode_reported_drops = info.packets_dropped;
    }
    return status;
}

static rns_status_t poll_auto(runtime_interface_t *interface,
                              receive_context_t *context) {
    size_t received = 0u;
    rns_status_t status = rns_auto_posix_poll(
        interface->auto_interface, context->remaining, auto_receive, context,
        &received);
    (void)received;
    interface->info.state = rns_auto_posix_online(interface->auto_interface)
                                ? RNS_RUNTIME_INTERFACE_UP
                                : RNS_RUNTIME_INTERFACE_STARTING;
    interface->info.peers =
        (uint64_t)rns_auto_posix_peer_count(interface->auto_interface);
    if (status == RNS_OK) interface->info.last_error = RNS_OK;
    return status;
}

rns_status_t rns_runtime_poll(rns_runtime_t *runtime, size_t max_packets, size_t *processed) {
    rns_status_t first_error = RNS_OK;
    if (runtime == NULL || processed == NULL) return RNS_ERROR_INVALID_ARGUMENT;
    *processed = 0U;
    if (max_packets == 0U) max_packets = RUNTIME_DEFAULT_WORK;
    (void)rns_transport_expire(&runtime->node.transport);
    double now = runtime_clock(NULL);
    for (size_t i = 0U; i < RNS_RUNTIME_MAX_PACKET_RECEIPTS; ++i) {
        rns_packet_receipt_t *receipt = runtime->packet_receipts[i];
        if (receipt != NULL && receipt->state == RNS_PACKET_RECEIPT_PENDING &&
            now >= receipt->deadline) {
            receipt->state = RNS_PACKET_RECEIPT_FAILED;
            receipt->concluded_at = now;
            packet_receipt_notify(receipt, RNS_ERROR_TIMEOUT);
        }
    }
    for (size_t i = 0U; i < RNS_RUNTIME_MAX_LINKS; i++) {
        rns_runtime_link_t *link = runtime->links[i];
        if (link == NULL) continue;
        for (size_t j = 0U; j < RNS_RUNTIME_MAX_REQUESTS; j++) {
            rns_request_receipt_t *receipt = link->requests[j];
            if (receipt != NULL && receipt->state == RNS_REQUEST_PENDING &&
                now >= receipt->deadline) {
                receipt->state = RNS_REQUEST_FAILED;
                request_notify(receipt, RNS_ERROR_TIMEOUT, NULL, 0U);
            }
        }
        if (link->outgoing_resource != NULL &&
            now >= link->outgoing_resource->deadline)
            outgoing_resource_finish(link->outgoing_resource,
                                     RNS_RUNTIME_RESOURCE_FAILED,
                                     RNS_ERROR_TIMEOUT);
        if ((link->resource != NULL || link->resource_assembled != NULL) &&
            link->resource_deadline > 0.0 &&
            now >= link->resource_deadline)
            incoming_resource_abort(link, RNS_ERROR_TIMEOUT);
        else if (link->resource != NULL &&
                 link->resource_request_retry_at > 0.0 &&
                 now >= link->resource_request_retry_at) {
            rns_status_t retry_status = resource_request_parts(link, true);
            if (retry_status == RNS_ERROR_TIMEOUT)
                incoming_resource_abort(link, RNS_ERROR_TIMEOUT);
            else if (retry_status != RNS_OK)
                incoming_resource_abort(link, RNS_ERROR_IO);
        }
        if ((link->protocol.state == RNS_LINK_PENDING ||
             link->protocol.state == RNS_LINK_HANDSHAKE) &&
            rns_link_check_timeout(&link->protocol)) {
            fail_pending_requests(link, RNS_ERROR_TIMEOUT);
            fail_link_resources(link, RNS_ERROR_TIMEOUT);
            link_notify(link, RNS_LINK_CLOSED, RNS_ERROR_TIMEOUT);
        } else if (link->protocol.state == RNS_LINK_ACTIVE &&
                 now - link->last_inbound >= 2.0 * link->keepalive_seconds +
                                             link->protocol.rtt * 4.0 + 5.0) {
            link->protocol.state = RNS_LINK_CLOSED;
            fail_pending_requests(link, RNS_ERROR_TIMEOUT);
            fail_link_resources(link, RNS_ERROR_TIMEOUT);
            link_notify(link, RNS_LINK_CLOSED, RNS_ERROR_TIMEOUT);
        } else if (link->protocol.state == RNS_LINK_ACTIVE &&
                   now - link->last_outbound >= link->keepalive_seconds) {
            if (link->protocol.role == RNS_LINK_INITIATOR) {
                const uint8_t keepalive = 0xffU;
                (void)link_send_wire(link, RNS_LINK_CONTEXT_KEEPALIVE,
                                     &keepalive, 1U, NULL);
            }
        }
    }
    for (size_t offset = 0U; offset < runtime->interface_count; ++offset) {
        size_t i = (runtime->poll_cursor + offset) % runtime->interface_count;
        runtime_interface_t *interface = &runtime->interfaces[i];
        receive_context_t context = {runtime, i, max_packets - *processed, 0U, false};
        rns_status_t status = RNS_OK;
        if (interface->provider != NULL) {
            /* A zero RX budget still services provider TX and recovery. */
            status = rns_interface_poll(interface->provider, provider_receive,
                &context, context.remaining);
            if (context.provider_budget_exceeded && status == RNS_ERROR_OVERFLOW)
                status = RNS_OK;
            rns_interface_stats_t stats;
            rns_status_t stats_status = rns_interface_get_stats(interface->provider, &stats);
            if (status == RNS_OK) status = stats_status;
            interface->info.state = status != RNS_OK ? RNS_RUNTIME_INTERFACE_DOWN :
                stats.online ? RNS_RUNTIME_INTERFACE_UP : RNS_RUNTIME_INTERFACE_STARTING;
            interface->info.last_error = status;
        } else if (context.remaining == 0U) {
            continue;
        } else if (interface->local != NULL) {
            status = poll_local(interface, &context);
        } else if (interface->kiss != NULL) {
            status = poll_kiss(interface, &context);
        } else if (interface->rnode != NULL) {
            status = poll_rnode(interface, &context);
        } else if (interface->auto_interface != NULL) {
            status = poll_auto(interface, &context);
        } else if (interface->tcp != NULL &&
            interface->info.state != RNS_RUNTIME_INTERFACE_DISABLED &&
            interface->info.state != RNS_RUNTIME_INTERFACE_UNSUPPORTED) {
            status = poll_tcp(runtime, i, &context);
        } else if (interface->info.state != RNS_RUNTIME_INTERFACE_UP) {
            continue;
        } else if (interface->udp != NULL) {
            size_t count = 0U;
            status = rns_udp_poll(interface->udp, context.remaining, udp_receive, &context, &count);
        }
        *processed += context.processed;
        if (status != RNS_OK) {
            interface->info.last_error = status;
            if (first_error == RNS_OK) first_error = status;
        }
    }
    if (runtime->interface_count != 0U)
        runtime->poll_cursor = (runtime->poll_cursor + 1U) % runtime->interface_count;
    return first_error;
}

rns_status_t rns_runtime_send(rns_runtime_t *runtime, size_t index,
                              const uint8_t *packet, size_t length) {
    return send_internal(runtime, index, packet, length);
}

static size_t interface_for_path(const rns_runtime_t *runtime, uint64_t interface_id) {
    for (size_t i = 0U; i < runtime->interface_count; i++)
        if (runtime->interfaces[i].info.id == interface_id) return i;
    return runtime->interface_count;
}

static rns_status_t prepare_routed_packet(rns_runtime_t *runtime,
                                          const uint8_t *packet,
                                          size_t packet_length,
                                          uint8_t raw[RNS_MTU],
                                          size_t *raw_length,
                                          size_t *interface_index,
                                          uint8_t *path_hops) {
    rns_packet decoded;
    if (runtime == NULL || packet == NULL || raw == NULL || raw_length == NULL ||
        interface_index == NULL || packet_length > RNS_MTU)
        return RNS_ERROR_INVALID_ARGUMENT;
    if (!rns_packet_decode(&decoded, packet, packet_length))
        return RNS_ERROR_INVALID_ARGUMENT;
    const rns_path_entry *path = rns_transport_lookup(&runtime->node.transport,
                                                      decoded.destination_hash);
    if (path == NULL) return RNS_ERROR_NOT_FOUND;
    size_t index = interface_for_path(runtime, path->interface_id);
    if (index == runtime->interface_count) return RNS_ERROR_INVALID_STATE;
    if (path_hops != NULL) *path_hops = path->hops;
    *interface_index = index;
    if (path->hops <= 1U) {
        memcpy(raw, packet, packet_length);
        *raw_length = packet_length;
        return RNS_OK;
    }
    rns_packet routed = decoded;
    routed.header_type = RNS_PACKET_HEADER_2;
    routed.transport_type = 1U;
    memcpy(routed.transport_id, path->next_hop, sizeof routed.transport_id);
    if (!rns_packet_encode(&routed, raw, RNS_MTU, raw_length))
        return RNS_ERROR_OVERFLOW;
    return RNS_OK;
}

rns_status_t rns_runtime_send_routed(rns_runtime_t *runtime, const uint8_t *packet,
                                     size_t packet_length) {
    uint8_t raw[RNS_MTU];
    size_t raw_length = 0U;
    size_t index = 0U;
    rns_status_t status = prepare_routed_packet(runtime, packet, packet_length,
                                                raw, &raw_length, &index, NULL);
    if (status != RNS_OK) return status;
    return send_internal(runtime, index, raw, raw_length);
}

rns_status_t rns_runtime_send_routed_with_receipt(
    rns_runtime_t *runtime, const uint8_t *packet, size_t packet_length,
    const rns_identity *destination_identity,
    const rns_packet_receipt_options_t *options,
    rns_packet_receipt_t **output) {
    uint8_t raw[RNS_MTU];
    size_t raw_length = 0U;
    size_t interface_index = 0U;
    uint8_t hops = 0U;
    size_t slot = RNS_RUNTIME_MAX_PACKET_RECEIPTS;
    if (runtime == NULL || packet == NULL || destination_identity == NULL ||
        output == NULL)
        return RNS_ERROR_INVALID_ARGUMENT;
    *output = NULL;
    for (size_t i = 0U; i < RNS_RUNTIME_MAX_PACKET_RECEIPTS; ++i)
        if (runtime->packet_receipts[i] == NULL) {
            slot = i;
            break;
        }
    if (slot == RNS_RUNTIME_MAX_PACKET_RECEIPTS) return RNS_ERROR_OVERFLOW;
    rns_status_t status = prepare_routed_packet(runtime, packet, packet_length,
                                                raw, &raw_length,
                                                &interface_index, &hops);
    if (status != RNS_OK) return status;
    rns_packet_receipt_t *receipt = calloc(1U, sizeof *receipt);
    if (receipt == NULL) return RNS_ERROR_NO_MEMORY;
    receipt->runtime = runtime;
    receipt->destination_identity = *destination_identity;
    if (options != NULL) receipt->options = *options;
    if (!rns_packet_hash(raw, raw_length, receipt->packet_hash)) {
        free(receipt);
        return RNS_ERROR_CRYPTO;
    }
    double now = runtime_clock(NULL);
    double timeout = receipt->options.timeout_seconds;
    if (timeout <= 0.0) timeout = 10.0 + 6.0 * (double)(hops != 0U ? hops : 1U);
    receipt->state = RNS_PACKET_RECEIPT_PENDING;
    receipt->sent_at = now;
    receipt->deadline = now + timeout;
    runtime->packet_receipts[slot] = receipt;
    status = send_internal(runtime, interface_index, raw, raw_length);
    if (status != RNS_OK) {
        runtime->packet_receipts[slot] = NULL;
        free(receipt);
        return status;
    }
    *output = receipt;
    return RNS_OK;
}

rns_status_t rns_runtime_prove_packet(rns_runtime_t *runtime,
                                      const rns_node_result *received,
                                      const rns_identity *identity,
                                      bool explicit_proof) {
    uint8_t proof[RNS_PROOF_EXPLICIT_SIZE];
    size_t proof_length = explicit_proof ? RNS_PROOF_EXPLICIT_SIZE
                                         : RNS_PROOF_IMPLICIT_SIZE;
    uint8_t raw[RNS_MTU];
    size_t raw_length = 0U;
    if (runtime == NULL || received == NULL || identity == NULL ||
        !identity->has_private)
        return RNS_ERROR_INVALID_ARGUMENT;
    int generated = explicit_proof
                        ? rns_proof_generate_explicit(identity,
                                                      received->packet_hash,
                                                      proof)
                        : rns_proof_generate_implicit(identity,
                                                      received->packet_hash,
                                                      proof);
    if (!generated) return RNS_ERROR_CRYPTO;
    rns_packet packet = {0};
    packet.destination_type = 0U;
    packet.packet_type = 3U;
    memcpy(packet.destination_hash, received->packet_hash, 16U);
    packet.data = proof;
    packet.data_length = proof_length;
    if (!rns_packet_encode(&packet, raw, sizeof raw, &raw_length))
        return RNS_ERROR_OVERFLOW;
    size_t interface_index = interface_for_path(runtime,
                                                received->received_interface_id);
    if (interface_index == runtime->interface_count)
        return RNS_ERROR_INVALID_ARGUMENT;
    return send_internal(runtime, interface_index, raw, raw_length);
}

rns_packet_receipt_state_t rns_packet_receipt_state(
    const rns_packet_receipt_t *receipt) {
    return receipt != NULL ? receipt->state : RNS_PACKET_RECEIPT_FAILED;
}

const uint8_t *rns_packet_receipt_hash(const rns_packet_receipt_t *receipt) {
    return receipt != NULL ? receipt->packet_hash : NULL;
}

double rns_packet_receipt_rtt(const rns_packet_receipt_t *receipt) {
    if (receipt == NULL || receipt->state != RNS_PACKET_RECEIPT_DELIVERED)
        return 0.0;
    return receipt->concluded_at - receipt->sent_at;
}

void rns_packet_receipt_cancel(rns_packet_receipt_t *receipt) {
    if (receipt == NULL || receipt->state != RNS_PACKET_RECEIPT_PENDING) return;
    receipt->state = RNS_PACKET_RECEIPT_CANCELLED;
    receipt->concluded_at = runtime_clock(NULL);
    packet_receipt_notify(receipt, RNS_OK);
}

void rns_packet_receipt_destroy(rns_packet_receipt_t *receipt) {
    if (receipt == NULL) return;
    rns_runtime_t *runtime = receipt->runtime;
    if (runtime != NULL)
        for (size_t i = 0U; i < RNS_RUNTIME_MAX_PACKET_RECEIPTS; ++i)
            if (runtime->packet_receipts[i] == receipt) {
                runtime->packet_receipts[i] = NULL;
                break;
            }
    free(receipt);
}

rns_status_t rns_runtime_announce(rns_runtime_t *runtime, const rns_identity *identity,
                                  const char *app_name, const char *const *aspects,
                                  size_t aspect_count, const uint8_t *app_data,
                                  size_t app_data_length) {
    return rns_runtime_announce_with_ratchet(
        runtime, identity, app_name, aspects, aspect_count, NULL, app_data,
        app_data_length);
}

rns_status_t rns_runtime_announce_with_ratchet(
    rns_runtime_t *runtime, const rns_identity *identity,
    const char *app_name, const char *const *aspects, size_t aspect_count,
    const uint8_t ratchet_public[RNS_RATCHET_PUBLIC_SIZE],
    const uint8_t *app_data, size_t app_data_length) {
    uint8_t destination[16], name_hash[10], prefix[5];
    uint8_t body[RNS_MTU], raw[RNS_MTU];
    size_t body_length = 0U, raw_length = 0U;
    uint8_t context_flag = 0U;
    uint64_t wallclock_ms = 0U;
    if (runtime == NULL || identity == NULL || app_name == NULL)
        return RNS_ERROR_INVALID_ARGUMENT;
    if (!rns_destination_hash(identity, app_name, aspects, aspect_count, destination) ||
        !rns_destination_name_hash(app_name, aspects, aspect_count, name_hash))
        return RNS_ERROR_INVALID_ARGUMENT;
    if (rns_hal_random_bytes(prefix, sizeof prefix) != RNS_OK ||
        rns_hal_wallclock_ms(&wallclock_ms) != RNS_OK) return RNS_ERROR_INVALID_STATE;
    if (!rns_announce_build(identity, destination, name_hash, prefix,
                            wallclock_ms / 1000U, ratchet_public, app_data,
                            app_data_length,
                            body, sizeof body, &body_length, &context_flag))
        return RNS_ERROR_INVALID_STATE;
    rns_packet packet = {0};
    packet.context_flag = context_flag;
    packet.destination_type = 0U;
    packet.packet_type = 1U;
    memcpy(packet.destination_hash, destination, sizeof packet.destination_hash);
    packet.data = body;
    packet.data_length = body_length;
    if (!rns_packet_encode(&packet, raw, sizeof raw, &raw_length))
        return RNS_ERROR_OVERFLOW;
    runtime_local_announce_t *cached = local_announce(runtime, destination, true);
    if (cached != NULL) {
        cached->used = true;
        memcpy(cached->hash, destination, sizeof cached->hash);
        cached->identity = *identity;
        memcpy(cached->body, body, body_length);
        cached->body_length = body_length;
        cached->context_flag = context_flag;
    }
    rns_status_t result = RNS_ERROR_INVALID_STATE;
    for (size_t i = 0U; i < runtime->interface_count; i++) {
        if (runtime->interfaces[i].info.state != RNS_RUNTIME_INTERFACE_UP) continue;
        if (!broadcast_enabled(&runtime->interfaces[i])) continue;
        rns_status_t status = send_internal(runtime, i, raw, raw_length);
        if (status == RNS_OK) result = RNS_OK;
        else if (result != RNS_OK) result = status;
    }
    return result;
}

size_t rns_runtime_interface_count(const rns_runtime_t *runtime) {
    return runtime != NULL ? runtime->interface_count : 0U;
}

rns_status_t rns_runtime_interface_info(const rns_runtime_t *runtime, size_t index,
                                        rns_runtime_interface_info_t *info) {
    if (runtime == NULL || info == NULL || index >= runtime->interface_count)
        return RNS_ERROR_INVALID_ARGUMENT;
    *info = runtime->interfaces[index].info;
    return RNS_OK;
}

rns_status_t rns_runtime_register_destination(rns_runtime_t *runtime, const uint8_t hash[16]) {
    if (runtime == NULL || hash == NULL) return RNS_ERROR_INVALID_ARGUMENT;
    for (size_t i = 0U; i < runtime->plain_destination_count; ++i)
        if (memcmp(runtime->plain_destinations + i * 16U, hash, 16U) == 0)
            return RNS_OK;
    if (runtime->plain_destination_count == runtime->plain_destination_capacity)
        return RNS_ERROR_OVERFLOW;
    if (!rns_node_register_destination(&runtime->node, hash))
        return RNS_ERROR_OVERFLOW;
    memcpy(runtime->plain_destinations + runtime->plain_destination_count++ * 16U,
           hash, 16U);
    return RNS_OK;
}

rns_status_t rns_runtime_unregister_destination(rns_runtime_t *runtime, const uint8_t hash[16]) {
    if (runtime == NULL || hash == NULL) return RNS_ERROR_INVALID_ARGUMENT;
    size_t index = 0U;
    while (index < runtime->plain_destination_count &&
           memcmp(runtime->plain_destinations + index * 16U, hash, 16U) != 0)
        ++index;
    if (index == runtime->plain_destination_count) return RNS_ERROR_NOT_FOUND;
    if (index + 1U < runtime->plain_destination_count)
        memmove(runtime->plain_destinations + index * 16U,
                runtime->plain_destinations + (index + 1U) * 16U,
                (runtime->plain_destination_count - index - 1U) * 16U);
    --runtime->plain_destination_count;
    if (find_link_destination(runtime, hash) == NULL) {
        (void)rns_node_unregister_destination(&runtime->node, hash);
        forget_local_announce(runtime, hash);
    }
    return RNS_OK;
}

rns_status_t rns_runtime_register_link_destination(
    rns_runtime_t *runtime, const uint8_t hash[16],
    const rns_identity *identity,
    const rns_runtime_link_options_t *link_options,
    rns_runtime_inbound_link_callback_t accepted_callback,
    void *callback_context, rns_runtime_destination_t **output) {
    if (runtime == NULL || hash == NULL || identity == NULL ||
        !identity->has_private || accepted_callback == NULL || output == NULL)
        return RNS_ERROR_INVALID_ARGUMENT;
    *output = NULL;
    if (find_link_destination(runtime, hash) != NULL)
        return RNS_ERROR_INVALID_STATE;
    size_t slot = RNS_RUNTIME_MAX_DESTINATIONS;
    for (size_t i = 0U; i < RNS_RUNTIME_MAX_DESTINATIONS; ++i)
        if (runtime->destinations[i] == NULL) { slot = i; break; }
    if (slot == RNS_RUNTIME_MAX_DESTINATIONS) return RNS_ERROR_OVERFLOW;
    rns_runtime_destination_t *destination = calloc(1U, sizeof *destination);
    if (destination == NULL) return RNS_ERROR_NO_MEMORY;
    destination->runtime = runtime;
    destination->identity = *identity;
    memcpy(destination->hash, hash, sizeof destination->hash);
    if (link_options != NULL) destination->link_options = *link_options;
    destination->accepted_callback = accepted_callback;
    destination->callback_context = callback_context;
    if (!rns_node_register_destination(&runtime->node, hash)) {
        free(destination);
        return RNS_ERROR_OVERFLOW;
    }
    runtime->destinations[slot] = destination;
    *output = destination;
    return RNS_OK;
}

const uint8_t *rns_runtime_destination_hash(
    const rns_runtime_destination_t *destination) {
    return destination != NULL ? destination->hash : NULL;
}

void rns_runtime_destination_destroy(rns_runtime_destination_t *destination) {
    if (destination == NULL) return;
    rns_runtime_t *runtime = destination->runtime;
    if (runtime != NULL) {
        for (size_t i = 0U; i < RNS_RUNTIME_MAX_LINKS; ++i)
            if (runtime->links[i] != NULL &&
                runtime->links[i]->inbound_destination == destination)
                runtime->links[i]->inbound_destination = NULL;
        for (size_t i = 0U; i < RNS_RUNTIME_MAX_DESTINATIONS; ++i)
            if (runtime->destinations[i] == destination) {
                runtime->destinations[i] = NULL;
                break;
            }
        bool plain = false;
        for (size_t i = 0U; i < runtime->plain_destination_count; ++i)
            if (memcmp(runtime->plain_destinations + i * 16U,
                       destination->hash, 16U) == 0) {
                plain = true;
                break;
            }
        if (!plain) {
            (void)rns_node_unregister_destination(&runtime->node,
                                                  destination->hash);
            forget_local_announce(runtime, destination->hash);
        }
    }
    for (size_t i = 0U; i < RNS_RUNTIME_MAX_REQUEST_HANDLERS; ++i)
        free(destination->request_handlers[i]);
    free(destination);
}

rns_status_t rns_runtime_destination_register_request_handler(
    rns_runtime_destination_t *destination, const char *path,
    const rns_runtime_request_handler_options_t *options,
    rns_runtime_request_handler_t **output) {
    if (destination == NULL || path == NULL || options == NULL ||
        options->callback == NULL || output == NULL ||
        options->access > RNS_REQUEST_ALLOW_LIST ||
        options->allow_identity_count > RNS_RUNTIME_MAX_REQUEST_ALLOWLIST ||
        (options->allow_identity_count != 0U &&
         options->allow_identity_hashes == NULL))
        return RNS_ERROR_INVALID_ARGUMENT;
    *output = NULL;
    size_t path_length = strnlen(path, RNS_REQUEST_PATH_MAX + 1U);
    if (path_length == 0U || path_length > RNS_REQUEST_PATH_MAX)
        return RNS_ERROR_INVALID_ARGUMENT;
    size_t response_size = options->max_response_size != 0U
                               ? options->max_response_size
                               : RNS_REQUEST_HANDLER_DEFAULT_MAX_RESPONSE;
    if (response_size > RNS_REQUEST_DEFAULT_MAX_RESPONSE ||
        response_size > RNS_RESOURCE_MAX_SIZE - 19U)
        return RNS_ERROR_OVERFLOW;
    uint8_t digest[32];
    if (!rns_sha256((const uint8_t *)path, path_length, digest))
        return RNS_ERROR_CRYPTO;
    size_t slot = RNS_RUNTIME_MAX_REQUEST_HANDLERS;
    for (size_t i = 0U; i < RNS_RUNTIME_MAX_REQUEST_HANDLERS; ++i) {
        rns_runtime_request_handler_t *existing =
            destination->request_handlers[i];
        if (existing == NULL && slot == RNS_RUNTIME_MAX_REQUEST_HANDLERS)
            slot = i;
        else if (existing != NULL &&
                 memcmp(existing->path_hash, digest,
                        RNS_REQUEST_ID_LENGTH) == 0)
            return RNS_ERROR_INVALID_STATE;
    }
    if (slot == RNS_RUNTIME_MAX_REQUEST_HANDLERS) return RNS_ERROR_OVERFLOW;
    rns_runtime_request_handler_t *handler = calloc(1U, sizeof *handler);
    if (handler == NULL) return RNS_ERROR_NO_MEMORY;
    handler->destination = destination;
    memcpy(handler->path, path, path_length + 1U);
    memcpy(handler->path_hash, digest, RNS_REQUEST_ID_LENGTH);
    handler->options = *options;
    handler->options.max_response_size = response_size;
    handler->options.allow_identity_hashes = NULL;
    if (options->allow_identity_count != 0U)
        memcpy(handler->allow_identity_hashes,
               options->allow_identity_hashes,
               options->allow_identity_count * 16U);
    destination->request_handlers[slot] = handler;
    *output = handler;
    return RNS_OK;
}

const char *rns_runtime_request_handler_path(
    const rns_runtime_request_handler_t *handler) {
    return handler != NULL ? handler->path : NULL;
}

void rns_runtime_request_handler_destroy(
    rns_runtime_request_handler_t *handler) {
    if (handler == NULL) return;
    rns_runtime_destination_t *destination = handler->destination;
    if (destination != NULL)
        for (size_t i = 0U; i < RNS_RUNTIME_MAX_REQUEST_HANDLERS; ++i)
            if (destination->request_handlers[i] == handler) {
                destination->request_handlers[i] = NULL;
                break;
            }
    free(handler);
}

rns_status_t rns_runtime_request_path(rns_runtime_t *runtime,
                                      const uint8_t destination_hash[16]) {
    if (runtime == NULL || destination_hash == NULL) return RNS_ERROR_INVALID_ARGUMENT;
    uint8_t tag[RNS_PATH_REQUEST_MAX_TAG_SIZE], body[48], raw[RNS_MTU];
    size_t body_length = 0U, raw_length = 0U;
    if (rns_hal_random_bytes(tag, sizeof tag) != RNS_OK ||
        !rns_path_request_build(destination_hash, NULL, tag, sizeof tag, body,
                                sizeof body, &body_length))
        return RNS_ERROR_INVALID_STATE;
    rns_packet request = {0};
    memcpy(request.destination_hash, runtime->node.path_request_destination,
           sizeof request.destination_hash);
    request.transport_type = 0U;
    request.destination_type = 2U;
    request.packet_type = 0U;
    request.context = RNS_NODE_PATH_REQUEST_CONTEXT;
    request.data = body;
    request.data_length = body_length;
    if (!rns_packet_encode(&request, raw, sizeof raw, &raw_length))
        return RNS_ERROR_INVALID_STATE;
    rns_status_t last_error = RNS_ERROR_INVALID_STATE;
    bool sent_on_live_interface = false;
    bool succeeded = false;
    for (size_t i = 0U; i < runtime->interface_count; i++) {
        if (runtime->interfaces[i].info.state != RNS_RUNTIME_INTERFACE_UP) continue;
        if (!broadcast_enabled(&runtime->interfaces[i])) continue;
        sent_on_live_interface = true;
        rns_status_t status = send_internal(runtime, i, raw, raw_length);
        if (status == RNS_OK) succeeded = true;
        last_error = status;
    }
    return succeeded ? RNS_OK : sent_on_live_interface ? last_error : RNS_ERROR_INVALID_STATE;
}

rns_status_t rns_runtime_path_lookup(const rns_runtime_t *runtime, const uint8_t hash[16],
                                     rns_path_entry *path) {
    if (runtime == NULL || hash == NULL || path == NULL) return RNS_ERROR_INVALID_ARGUMENT;
    const rns_path_entry *found = rns_transport_lookup((rns_transport *)&runtime->node.transport, hash);
    if (found == NULL) return RNS_ERROR_NOT_FOUND;
    *path = *found;
    return RNS_OK;
}

rns_status_t rns_runtime_recall_identity(const rns_runtime_t *runtime,
                                        const uint8_t destination_hash[16],
                                        const uint8_t name_hash[10],
                                        rns_identity *identity) {
    if (runtime == NULL || destination_hash == NULL || name_hash == NULL || identity == NULL)
        return RNS_ERROR_INVALID_ARGUMENT;
    const rns_transport *transport = &runtime->node.transport;
    double now = transport->config.clock(transport->config.clock_context);
    uint8_t material[26], digest[32];
    memcpy(material, name_hash, 10u);
    for (size_t i = 0u; i < transport->config.path_capacity; ++i) {
        const rns_path_entry *path = &transport->paths[i];
        if (!path->occupied || !path->has_identity || !(path->expires_at > now)) continue;
        rns_identity candidate;
        if (!rns_identity_from_public(&candidate, path->identity_public_key)) continue;
        memcpy(material + 10u, candidate.hash, 16u);
        if (!rns_sha256(material, sizeof material, digest)) return RNS_ERROR_CRYPTO;
        if (memcmp(digest, destination_hash, 16u) == 0) {
            *identity = candidate;
            return RNS_OK;
        }
    }
    return RNS_ERROR_NOT_FOUND;
}

size_t rns_runtime_path_snapshot(const rns_runtime_t *runtime, rns_path_entry *paths,
                                 size_t capacity) {
    if (runtime == NULL || (paths == NULL && capacity != 0U)) return 0U;
    size_t count = 0U;
    for (size_t i = 0U; i < runtime->node.transport.config.path_capacity && count < capacity; ++i) {
        if (!runtime->node.transport.paths[i].occupied) continue;
        paths[count++] = runtime->node.transport.paths[i];
    }
    return count;
}

rns_status_t rns_runtime_paths_export(const rns_runtime_t *runtime,
                                      uint64_t wall_time_ms,
                                      uint8_t *output, size_t output_capacity,
                                      size_t *output_length,
                                      size_t *encoded_count) {
    if (runtime == NULL) return RNS_ERROR_INVALID_ARGUMENT;
    return rns_path_store_encode(&runtime->node.transport, wall_time_ms, output,
                                 output_capacity, output_length, encoded_count);
}

rns_status_t rns_runtime_paths_import(rns_runtime_t *runtime,
                                      uint64_t wall_time_ms,
                                      const uint8_t *input,
                                      size_t input_length,
                                      size_t *decoded_count) {
    if (runtime == NULL) return RNS_ERROR_INVALID_ARGUMENT;
    return rns_path_store_decode(&runtime->node.transport, wall_time_ms, input,
                                 input_length, decoded_count);
}

rns_status_t rns_runtime_link_open(
    rns_runtime_t *runtime, const uint8_t destination_hash[16],
    const rns_identity *destination_identity,
    const rns_runtime_link_options_t *options, rns_runtime_link_t **output) {
    if (runtime == NULL || destination_hash == NULL || destination_identity == NULL ||
        output == NULL)
        return RNS_ERROR_INVALID_ARGUMENT;
    *output = NULL;
    const rns_path_entry *path = rns_transport_lookup(&runtime->node.transport,
                                                       destination_hash);
    if (path == NULL) return RNS_ERROR_NOT_FOUND;
    size_t slot = RNS_RUNTIME_MAX_LINKS;
    for (size_t i = 0U; i < RNS_RUNTIME_MAX_LINKS; i++)
        if (runtime->links[i] == NULL) { slot = i; break; }
    if (slot == RNS_RUNTIME_MAX_LINKS) return RNS_ERROR_OVERFLOW;
    size_t interface_index = runtime->interface_count;
    for (size_t i = 0U; i < runtime->interface_count; i++)
        if (runtime->interfaces[i].info.id == path->interface_id) {
            interface_index = i;
            break;
        }
    if (interface_index == runtime->interface_count ||
        runtime->interfaces[interface_index].info.state != RNS_RUNTIME_INTERFACE_UP)
        return RNS_ERROR_INVALID_STATE;

    rns_runtime_link_t *link = calloc(1U, sizeof *link);
    if (link == NULL) return RNS_ERROR_NO_MEMORY;
    link->runtime = runtime;
    link->interface_index = interface_index;
    if (options != NULL) link->options = *options;
    link->remote_identified = true;
    double timeout = link->options.timeout_seconds > 0.0
                         ? link->options.timeout_seconds
                         : 10.0 + 6.0 * (double)(path->hops ? path->hops : 1U);
    uint32_t mtu = link->options.mtu ? link->options.mtu : RNS_MTU;
    if (!rns_link_initiator_init(&link->protocol, destination_identity, mtu,
                                 timeout, runtime_clock, NULL)) {
        free(link);
        return RNS_ERROR_CRYPTO;
    }
    uint8_t payload[RNS_LINK_REQUEST_BYTES], raw[RNS_MTU];
    size_t raw_length = 0U;
    if (!rns_link_build_request_payload(&link->protocol, payload)) {
        free(link);
        return RNS_ERROR_CRYPTO;
    }
    rns_packet packet = {0};
    packet.destination_type = 0U;
    packet.packet_type = 2U;
    packet.data = payload;
    packet.data_length = sizeof payload;
    memcpy(packet.destination_hash, destination_hash,
           sizeof packet.destination_hash);
    if (path->hops > 1U) {
        packet.header_type = RNS_PACKET_HEADER_2;
        packet.transport_type = 1U;
        memcpy(packet.transport_id, path->next_hop, sizeof packet.transport_id);
    }
    if (!rns_packet_encode(&packet, raw, sizeof raw, &raw_length) ||
        !rns_link_initiator_set_request_packet(&link->protocol, raw, raw_length) ||
        !rns_node_register_destination(&runtime->node, link->protocol.link_id)) {
        free(link);
        return RNS_ERROR_PROTOCOL;
    }
    link->registered = true;
    runtime->links[slot] = link;
    rns_status_t status = send_internal(runtime, interface_index, raw, raw_length);
    if (status != RNS_OK) {
        runtime->links[slot] = NULL;
        (void)rns_node_unregister_destination(&runtime->node,
                                              link->protocol.link_id);
        free(link);
        return status;
    }
    link->last_inbound = link->protocol.request_time;
    link->last_outbound = link->protocol.request_time;
    link_notify(link, RNS_LINK_PENDING, RNS_OK);
    *output = link;
    return RNS_OK;
}

static rns_status_t link_send_plain(rns_runtime_link_t *link, uint8_t context,
                                    const uint8_t *plaintext,
                                    size_t plaintext_length,
                                    uint8_t packet_id[16]) {
    if (link == NULL || (plaintext == NULL && plaintext_length != 0U))
        return RNS_ERROR_INVALID_ARGUMENT;
    if (link->protocol.state != RNS_LINK_ACTIVE) return RNS_ERROR_INVALID_STATE;
    uint8_t encrypted[RNS_MTU];
    size_t encrypted_length = 0U;
    if (!rns_link_encrypt(&link->protocol, plaintext, plaintext_length, encrypted,
                          sizeof encrypted, &encrypted_length))
        return RNS_ERROR_OVERFLOW;
    return link_send_wire(link, context, encrypted, encrypted_length, packet_id);
}

rns_status_t rns_runtime_link_send(rns_runtime_link_t *link, uint8_t context,
                                   const uint8_t *plaintext,
                                   size_t plaintext_length) {
    if (context == RNS_LINK_CONTEXT_KEEPALIVE) {
        if (link == NULL || plaintext == NULL || plaintext_length != 1U ||
            link->protocol.state != RNS_LINK_ACTIVE)
            return RNS_ERROR_INVALID_ARGUMENT;
        uint8_t expected = link->protocol.role == RNS_LINK_INITIATOR
                               ? 0xffU : 0xfeU;
        if (plaintext[0] != expected) return RNS_ERROR_INVALID_ARGUMENT;
        return link_send_wire(link, context, plaintext, plaintext_length, NULL);
    }
    return link_send_plain(link, context, plaintext, plaintext_length, NULL);
}

static bool link_receipt_context(uint8_t context) {
    return context != RNS_LINK_CONTEXT_KEEPALIVE &&
           context != RNS_LINK_CONTEXT_CLOSE &&
           context != RNS_LINK_CONTEXT_RTT &&
           context != RNS_LINK_CONTEXT_IDENTIFY &&
           context != RNS_LINK_CONTEXT_PROOF &&
           context != RNS_LINK_CONTEXT_RESOURCE &&
           context != RNS_LINK_CONTEXT_RESOURCE_ADV &&
           context != RNS_LINK_CONTEXT_RESOURCE_REQ &&
           context != RNS_LINK_CONTEXT_RESOURCE_HMU &&
           context != RNS_LINK_CONTEXT_RESOURCE_PRF &&
           context != RNS_LINK_CONTEXT_RESOURCE_ICL;
}

rns_status_t rns_runtime_link_send_with_receipt(
    rns_runtime_link_t *link, uint8_t context, const uint8_t *plaintext,
    size_t plaintext_length, const rns_packet_receipt_options_t *options,
    rns_packet_receipt_t **output) {
    size_t slot = RNS_RUNTIME_MAX_PACKET_RECEIPTS;
    uint8_t encrypted[RNS_MTU];
    size_t encrypted_length = 0U;
    if (link == NULL || output == NULL ||
        (plaintext == NULL && plaintext_length != 0U) ||
        link->protocol.state != RNS_LINK_ACTIVE || !link_receipt_context(context))
        return RNS_ERROR_INVALID_ARGUMENT;
    *output = NULL;
    for (size_t i = 0U; i < RNS_RUNTIME_MAX_PACKET_RECEIPTS; ++i)
        if (link->runtime->packet_receipts[i] == NULL) {
            slot = i;
            break;
        }
    if (slot == RNS_RUNTIME_MAX_PACKET_RECEIPTS) return RNS_ERROR_OVERFLOW;
    if (!rns_link_encrypt(&link->protocol, plaintext, plaintext_length, encrypted,
                          sizeof encrypted, &encrypted_length))
        return RNS_ERROR_OVERFLOW;
    rns_packet_receipt_t *receipt = calloc(1U, sizeof *receipt);
    if (receipt == NULL) return RNS_ERROR_NO_MEMORY;
    receipt->runtime = link->runtime;
    receipt->link = link;
    if (options != NULL) receipt->options = *options;
    receipt->link_proof = true;
    memcpy(receipt->link_id, link->protocol.link_id, sizeof receipt->link_id);
    memcpy(receipt->link_signing_public, link->protocol.peer_signing_public,
           sizeof receipt->link_signing_public);
    double now = runtime_clock(NULL);
    double timeout = receipt->options.timeout_seconds;
    if (timeout <= 0.0) timeout = link->protocol.rtt * 6.0 + 5.0;
    if (timeout < 10.0) timeout = 10.0;
    receipt->state = RNS_PACKET_RECEIPT_PENDING;
    receipt->sent_at = now;
    receipt->deadline = now + timeout;
    link->runtime->packet_receipts[slot] = receipt;
    rns_status_t status = link_send_wire_hash(link, context, encrypted,
                                              encrypted_length,
                                              receipt->packet_hash);
    if (status != RNS_OK) {
        link->runtime->packet_receipts[slot] = NULL;
        free(receipt);
        return status;
    }
    *output = receipt;
    return RNS_OK;
}

rns_status_t rns_runtime_link_prove_current_packet(rns_runtime_link_t *link) {
    uint8_t proof[RNS_PROOF_EXPLICIT_SIZE];
    if (link == NULL || !link->callback_packet_provable ||
        link->protocol.state != RNS_LINK_ACTIVE)
        return RNS_ERROR_INVALID_STATE;
    memcpy(proof, link->callback_packet_hash, RNS_PROOF_HASH_SIZE);
    if (!rns_ed25519_sign(link->protocol.signing_private,
                          link->callback_packet_hash, RNS_PROOF_HASH_SIZE,
                          proof + RNS_PROOF_HASH_SIZE))
        return RNS_ERROR_CRYPTO;
    return link_send_proof(link, 0U, proof, sizeof proof);
}

rns_status_t rns_runtime_link_identify(rns_runtime_link_t *link,
                                       const rns_identity *identity) {
    uint8_t public_key[RNS_IDENTITY_PUBLIC_SIZE];
    uint8_t signed_data[16U + RNS_IDENTITY_PUBLIC_SIZE];
    uint8_t proof[RNS_IDENTITY_PUBLIC_SIZE + 64U];
    if (link == NULL || identity == NULL || !identity->has_private ||
        link->protocol.role != RNS_LINK_INITIATOR ||
        link->protocol.state != RNS_LINK_ACTIVE)
        return RNS_ERROR_INVALID_ARGUMENT;
    rns_identity_export_public(identity, public_key);
    memcpy(signed_data, link->protocol.link_id, 16U);
    memcpy(signed_data + 16U, public_key, sizeof public_key);
    memcpy(proof, public_key, sizeof public_key);
    if (!rns_identity_sign(identity, signed_data, sizeof signed_data,
                           proof + sizeof public_key))
        return RNS_ERROR_CRYPTO;
    return link_send_plain2(link, RNS_LINK_CONTEXT_IDENTIFY, proof,
                            sizeof proof);
}

const rns_identity *rns_runtime_link_remote_identity(
    const rns_runtime_link_t *link) {
    return link != NULL && link->remote_identified
               ? &link->protocol.remote_identity
               : NULL;
}

rns_status_t rns_runtime_link_request(
    rns_runtime_link_t *link, const char *path, const uint8_t *data_msgpack,
    size_t data_msgpack_length, const rns_request_options_t *options,
    rns_request_receipt_t **output) {
    uint64_t wallclock_ms = 0U;
    uint8_t request[RNS_MTU];
    size_t request_length = 0U;
    size_t slot = RNS_RUNTIME_MAX_REQUESTS;
    if (link == NULL || path == NULL || output == NULL ||
        (data_msgpack == NULL && data_msgpack_length != 0U))
        return RNS_ERROR_INVALID_ARGUMENT;
    *output = NULL;
    if (link->protocol.state != RNS_LINK_ACTIVE) return RNS_ERROR_INVALID_STATE;
    for (size_t i = 0U; i < RNS_RUNTIME_MAX_REQUESTS; i++)
        if (link->requests[i] == NULL) { slot = i; break; }
    if (slot == RNS_RUNTIME_MAX_REQUESTS) return RNS_ERROR_OVERFLOW;
    if (rns_hal_wallclock_ms(&wallclock_ms) != RNS_OK)
        return RNS_ERROR_INVALID_STATE;
    rns_status_t status = rns_request_encode(
        path, (double)wallclock_ms / 1000.0, data_msgpack,
        data_msgpack_length, request, sizeof request, &request_length);
    if (status != RNS_OK) return status;

    rns_request_receipt_t *receipt = calloc(1U, sizeof *receipt);
    if (receipt == NULL) return RNS_ERROR_NO_MEMORY;
    receipt->link = link;
    if (options != NULL) receipt->options = *options;
    if (receipt->options.max_response_size == 0U)
        receipt->options.max_response_size = RNS_REQUEST_DEFAULT_MAX_RESPONSE;
    double timeout = receipt->options.timeout_seconds;
    if (timeout <= 0.0) {
        timeout = link->protocol.rtt * 6.0 + 5.0;
        if (timeout < 10.0) timeout = 10.0;
    }
    receipt->state = RNS_REQUEST_PENDING;
    receipt->deadline = runtime_clock(NULL) + timeout;
    status = link_send_plain(link, RNS_LINK_CONTEXT_REQUEST, request,
                             request_length, receipt->request_id);
    if (status != RNS_OK) {
        free(receipt);
        return status;
    }
    link->requests[slot] = receipt;
    *output = receipt;
    return RNS_OK;
}

rns_request_state_t rns_request_receipt_state(
    const rns_request_receipt_t *receipt) {
    return receipt != NULL ? receipt->state : RNS_REQUEST_FAILED;
}

const uint8_t *rns_request_receipt_id(const rns_request_receipt_t *receipt) {
    return receipt != NULL ? receipt->request_id : NULL;
}

void rns_request_receipt_cancel(rns_request_receipt_t *receipt) {
    if (receipt == NULL || receipt->state != RNS_REQUEST_PENDING) return;
    receipt->state = RNS_REQUEST_CANCELLED;
    request_notify(receipt, RNS_OK, NULL, 0U);
}

void rns_request_receipt_destroy(rns_request_receipt_t *receipt) {
    if (receipt == NULL) return;
    rns_runtime_link_t *link = receipt->link;
    if (link != NULL)
        for (size_t i = 0U; i < RNS_RUNTIME_MAX_REQUESTS; i++)
            if (link->requests[i] == receipt) link->requests[i] = NULL;
    free(receipt);
}

rns_link_state rns_runtime_link_state(const rns_runtime_link_t *link) {
    return link != NULL ? link->protocol.state : RNS_LINK_CLOSED;
}

const uint8_t *rns_runtime_link_id(const rns_runtime_link_t *link) {
    return link != NULL ? link->protocol.link_id : NULL;
}

rns_status_t rns_runtime_link_send_resource(
    rns_runtime_link_t *link, const uint8_t *data, size_t data_length,
    const rns_runtime_resource_options_t *options,
    rns_runtime_resource_transfer_t **output) {
    uint8_t advertisement[RNS_MTU];
    size_t advertisement_length = 0U;
    rns_resource_sender_options_t sender_options = {0};
    if (link == NULL || output == NULL ||
        (data == NULL && data_length != 0U))
        return RNS_ERROR_INVALID_ARGUMENT;
    *output = NULL;
    if (link->protocol.state != RNS_LINK_ACTIVE)
        return RNS_ERROR_INVALID_STATE;
    if (link->outgoing_resource != NULL) return RNS_ERROR_OVERFLOW;
    rns_runtime_resource_transfer_t *transfer =
        calloc(1U, sizeof *transfer);
    if (transfer == NULL) return RNS_ERROR_NO_MEMORY;
    if (options != NULL) transfer->options = *options;
    sender_options.auto_compress = transfer->options.auto_compress;
    sender_options.is_response = transfer->options.is_response;
    sender_options.request_id = transfer->options.request_id;
    rns_status_t status = rns_resource_sender_create(
        &transfer->sender, &link->protocol, data, data_length,
        &sender_options);
    if (status != RNS_OK) {
        free(transfer);
        return status;
    }
    transfer->options.request_id = NULL;
    status = rns_resource_sender_advertisement(
        transfer->sender, advertisement, sizeof advertisement,
        &advertisement_length);
    if (status != RNS_OK) {
        rns_resource_sender_destroy(transfer->sender);
        free(transfer);
        return status;
    }
    transfer->link = link;
    transfer->state = RNS_RUNTIME_RESOURCE_ADVERTISED;
    double timeout = transfer->options.timeout_seconds;
    if (timeout <= 0.0) timeout = 30.0;
    transfer->deadline = runtime_clock(NULL) + timeout;
    link->outgoing_resource = transfer;
    status = link_send_plain2(link, RNS_LINK_CONTEXT_RESOURCE_ADV,
                              advertisement, advertisement_length);
    if (status != RNS_OK) {
        link->outgoing_resource = NULL;
        rns_resource_sender_destroy(transfer->sender);
        free(transfer);
        return status;
    }
    *output = transfer;
    return RNS_OK;
}

rns_runtime_resource_state_t rns_runtime_resource_transfer_state(
    const rns_runtime_resource_transfer_t *transfer) {
    return transfer != NULL ? transfer->state : RNS_RUNTIME_RESOURCE_FAILED;
}

size_t rns_runtime_resource_transfer_sent_parts(
    const rns_runtime_resource_transfer_t *transfer) {
    return transfer != NULL ? transfer->sent_parts : 0U;
}

size_t rns_runtime_resource_transfer_total_parts(
    const rns_runtime_resource_transfer_t *transfer) {
    return transfer != NULL
               ? rns_resource_sender_total_data_parts(transfer->sender)
               : 0U;
}

rns_status_t rns_runtime_resource_transfer_progress(
    const rns_runtime_resource_transfer_t *transfer,
    rns_runtime_resource_progress_t *progress) {
    if (transfer == NULL || progress == NULL)
        return RNS_ERROR_INVALID_ARGUMENT;
    size_t total = rns_resource_sender_data_size(transfer->sender);
    size_t segments = rns_resource_sender_total_segments(transfer->sender);
    size_t segment = rns_resource_sender_segment_index(transfer->sender);
    size_t wire = rns_resource_sender_transfer_size(transfer->sender);
    if (total == 0U || segments == 0U || segment == 0U || segment > segments ||
        wire == 0U)
        return RNS_ERROR_INVALID_STATE;
    size_t completed = (segment - 1U) * RNS_RESOURCE_SINGLE_SEGMENT_MAX_SIZE;
    if (completed > total) completed = total;
    size_t segment_source = total - completed;
    if (segment_source > RNS_RESOURCE_SINGLE_SEGMENT_MAX_SIZE)
        segment_source = RNS_RESOURCE_SINGLE_SEGMENT_MAX_SIZE;
    size_t sent = transfer->sent_segment_bytes;
    if (sent > wire) sent = wire;
    uint64_t partial = ((uint64_t)segment_source * sent) / wire;
    uint64_t transferred = (uint64_t)completed + partial;
    if (transfer->state == RNS_RUNTIME_RESOURCE_COMPLETE)
        transferred = total;
    if (transferred > total) transferred = total;
    progress->transferred = transferred;
    progress->total = total;
    progress->current_segment = segment;
    progress->total_segments = segments;
    return RNS_OK;
}

const uint8_t *rns_runtime_resource_transfer_hash(
    const rns_runtime_resource_transfer_t *transfer) {
    return transfer != NULL ? rns_resource_sender_hash(transfer->sender) : NULL;
}

void rns_runtime_resource_transfer_cancel(
    rns_runtime_resource_transfer_t *transfer) {
    if (transfer == NULL || transfer->link == NULL) return;
    rns_runtime_link_t *link = transfer->link;
    (void)link_send_plain2(link, RNS_LINK_CONTEXT_RESOURCE_ICL,
                           rns_resource_sender_hash(transfer->sender),
                           RNS_RESOURCE_HASH_SIZE);
    outgoing_resource_finish(transfer, RNS_RUNTIME_RESOURCE_CANCELLED,
                             RNS_OK);
}

void rns_runtime_resource_transfer_destroy(
    rns_runtime_resource_transfer_t *transfer) {
    if (transfer == NULL) return;
    if (transfer->link != NULL) {
        rns_runtime_link_t *link = transfer->link;
        if (link->protocol.state == RNS_LINK_ACTIVE)
            (void)link_send_plain2(
                link, RNS_LINK_CONTEXT_RESOURCE_ICL,
                rns_resource_sender_hash(transfer->sender),
                RNS_RESOURCE_HASH_SIZE);
        if (link->outgoing_resource == transfer)
            link->outgoing_resource = NULL;
        transfer->link = NULL;
    }
    rns_resource_sender_destroy(transfer->sender);
    free(transfer);
}

void rns_runtime_link_destroy(rns_runtime_link_t *link) {
    if (link == NULL) return;
    rns_runtime_t *runtime = link->runtime;
    if (link->protocol.state == RNS_LINK_ACTIVE)
        (void)rns_runtime_link_send(link, RNS_LINK_CONTEXT_CLOSE,
                                    link->protocol.link_id,
                                    sizeof link->protocol.link_id);
    link->protocol.state = RNS_LINK_CLOSED;
    fail_link_resources(link, RNS_ERROR_INVALID_STATE);
    fail_pending_requests(link, RNS_ERROR_INVALID_STATE);
    if (runtime != NULL) {
        for (size_t i = 0U; i < RNS_RUNTIME_MAX_PACKET_RECEIPTS; ++i) {
            rns_packet_receipt_t *receipt = runtime->packet_receipts[i];
            if (receipt == NULL || receipt->link != link ||
                receipt->state != RNS_PACKET_RECEIPT_PENDING)
                continue;
            receipt->state = RNS_PACKET_RECEIPT_FAILED;
            receipt->concluded_at = runtime_clock(NULL);
            receipt->link = NULL;
            packet_receipt_notify(receipt, RNS_ERROR_INVALID_STATE);
        }
        if (link->registered)
            (void)rns_node_unregister_destination(&runtime->node,
                                                  link->protocol.link_id);
        for (size_t i = 0U; i < RNS_RUNTIME_MAX_LINKS; i++)
            if (runtime->links[i] == link) runtime->links[i] = NULL;
    }
    link_notify(link, RNS_LINK_CLOSED, RNS_OK);
    for (size_t i = 0U; i < RNS_RUNTIME_MAX_REQUESTS; i++) {
        free(link->requests[i]);
        link->requests[i] = NULL;
    }
    free(link);
}
