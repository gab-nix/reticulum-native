#include "reticulum/runtime.h"

#include "reticulum/destination.h"
#include "reticulum/hal.h"
#include "reticulum/packet.h"
#include "reticulum/request.h"
#include "reticulum/resource.h"
#include "reticulum/tcp.h"
#include "reticulum/udp.h"

#include <stdlib.h>
#include <string.h>

#define RUNTIME_DEFAULT_WORK 32U
#define RUNTIME_TCP_QUEUE (RNS_MTU * 8U)

typedef struct runtime_interface {
    rns_runtime_interface_info_t info;
    rns_udp_endpoint_t *udp;
    rns_udp_address_t udp_forward;
    bool has_udp_forward;
    rns_tcp_endpoint_t *tcp;
    rns_tcp_endpoint_t *accepted;
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
};

struct rns_request_receipt {
    rns_runtime_link_t *link;
    rns_request_options_t options;
    rns_request_state_t state;
    uint8_t request_id[RNS_REQUEST_ID_LENGTH];
    double deadline;
};

struct rns_runtime {
    rns_config_t config;
    rns_node node;
    runtime_interface_t interfaces[RNS_CONFIG_MAX_INTERFACES];
    size_t interface_count;
    rns_runtime_packet_callback_t packet_callback;
    rns_runtime_announce_callback_t announce_callback;
    void *callback_context;
    rns_runtime_link_t *links[RNS_RUNTIME_MAX_LINKS];
};

typedef struct receive_context {
    rns_runtime_t *runtime;
    size_t interface_index;
    size_t remaining;
    size_t processed;
} receive_context_t;

static double runtime_clock(void *context) {
    uint64_t milliseconds = 0U;
    (void)context;
    (void)rns_hal_monotonic_ms(&milliseconds);
    return (double)milliseconds / 1000.0;
}

static rns_status_t send_internal(rns_runtime_t *runtime, size_t index,
                                  const uint8_t *packet, size_t length) {
    runtime_interface_t *interface;
    rns_status_t status = RNS_ERROR_INVALID_STATE;
    if (runtime == NULL || index >= runtime->interface_count || packet == NULL ||
        length == 0U || length > RNS_MTU) return RNS_ERROR_INVALID_ARGUMENT;
    interface = &runtime->interfaces[index];
    if (interface->info.state != RNS_RUNTIME_INTERFACE_UP)
        return interface->info.last_error != RNS_OK ? interface->info.last_error
                                                    : RNS_ERROR_INVALID_STATE;
    if (interface->udp != NULL && interface->has_udp_forward)
        status = rns_udp_send_to(interface->udp, &interface->udp_forward, packet, length);
    else if (interface->accepted != NULL)
        status = rns_tcp_queue_frame(interface->accepted, packet, length);
    else if (interface->tcp != NULL && rns_tcp_state(interface->tcp) == RNS_TCP_CONNECTED)
        status = rns_tcp_queue_frame(interface->tcp, packet, length);
    if (status == RNS_OK) {
        interface->info.packets_sent++;
        interface->info.bytes_sent += length;
    } else interface->info.last_error = status;
    return status;
}

static void link_notify(rns_runtime_link_t *link, rns_link_state state,
                        rns_status_t reason) {
    if (link->options.state_callback != NULL)
        link->options.state_callback(link, state, reason,
                                     link->options.callback_context);
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
    return send_internal(link->runtime, link->interface_index, raw, raw_length);
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
    link->resource_receipt = NULL;
}

static rns_status_t resource_request_parts(rns_runtime_link_t *link) {
    uint8_t body[RNS_MTU];
    size_t body_length = 0U;
    rns_status_t status = rns_resource_build_request(link->resource, body,
                                                     sizeof body, &body_length);
    if (status != RNS_OK) return status;
    return link_send_plain2(link, RNS_LINK_CONTEXT_RESOURCE_REQ, body, body_length);
}

/* Returns true when the advertisement was taken over by the resource layer. */
static bool resource_advertised(rns_runtime_link_t *link, const uint8_t *plaintext,
                                size_t plaintext_length) {
    rns_resource_advertisement_t advertisement;
    if (rns_resource_advertisement_parse(plaintext, plaintext_length,
                                         &advertisement) != RNS_OK) return false;
    rns_request_receipt_t *receipt =
        advertisement.has_request_id ? find_receipt(link, advertisement.request_id)
                                     : NULL;
    if (receipt == NULL) return false;
    if (link->resource != NULL) resource_release(link);
    if (rns_resource_accept(&link->resource, &advertisement,
                            receipt->options.max_response_size) != RNS_OK) {
        link->resource = NULL;
        return false;
    }
    link->resource_receipt = receipt;
    if (resource_request_parts(link) != RNS_OK) {
        resource_release(link);
        return false;
    }
    return true;
}

static void resource_complete(rns_runtime_link_t *link) {
    rns_request_receipt_t *receipt = link->resource_receipt;
    uint8_t proof[RNS_RESOURCE_PROOF_SIZE];
    size_t capacity = receipt->options.max_response_size;
    uint8_t *assembled = malloc(capacity != 0U ? capacity : 1U);
    size_t assembled_length = 0U;
    if (assembled == NULL) {
        receipt->state = RNS_REQUEST_FAILED;
        request_notify(receipt, RNS_ERROR_NO_MEMORY, NULL, 0U);
        resource_release(link);
        return;
    }
    rns_status_t status = rns_resource_assemble(link->resource, &link->protocol,
                                                assembled, capacity,
                                                &assembled_length);
    if (status != RNS_OK) {
        receipt->state = RNS_REQUEST_FAILED;
        request_notify(receipt, status, NULL, 0U);
        free(assembled);
        resource_release(link);
        return;
    }
    if (rns_resource_build_proof(link->resource, proof) == RNS_OK)
        (void)link_send_proof(link, RNS_LINK_CONTEXT_RESOURCE_PRF, proof,
                              sizeof proof);
    /* A response resource carries the same [request id, response] pair. */
    rns_response_view_t response;
    if (rns_response_decode(assembled, assembled_length, &response) == RNS_OK) {
        receipt->state = RNS_REQUEST_COMPLETE;
        request_notify(receipt, RNS_OK, response.response, response.response_length);
    } else {
        receipt->state = RNS_REQUEST_FAILED;
        request_notify(receipt, RNS_ERROR_PROTOCOL, NULL, 0U);
    }
    free(assembled);
    resource_release(link);
}

static void resource_part(rns_runtime_link_t *link, const uint8_t *plaintext,
                          size_t plaintext_length) {
    if (link->resource == NULL || link->resource_receipt == NULL) return;
    if (link->resource_receipt->state != RNS_REQUEST_PENDING) {
        resource_release(link);
        return;
    }
    if (rns_resource_receive_part(link->resource, plaintext, plaintext_length) != RNS_OK)
        return;
    if (rns_resource_parts_complete(link->resource)) resource_complete(link);
    else if (resource_request_parts(link) != RNS_OK) {
        /* Nothing outstanding in this window; the sender keeps streaming. */
    }
}

static bool link_ingress(rns_runtime_t *runtime, size_t interface_index,
                         const uint8_t *raw, size_t raw_length) {
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
        link->last_inbound = runtime_clock(NULL);
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
            double scaled = link->protocol.rtt * (360.0 / 1.75);
            if (scaled < 5.0) scaled = 5.0;
            if (scaled > 360.0) scaled = 360.0;
            link->keepalive_seconds = scaled;
            link_notify(link, RNS_LINK_ACTIVE, RNS_OK);
            return true;
        }
        if (link->protocol.state != RNS_LINK_ACTIVE || packet.packet_type != 0U)
            return true;
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
        } else plaintext_length = packet.data_length;
        if (packet.context == RNS_LINK_CONTEXT_CLOSE) {
            link->protocol.state = RNS_LINK_CLOSED;
            fail_pending_requests(link, RNS_ERROR_INVALID_STATE);
            link_notify(link, RNS_LINK_CLOSED, RNS_OK);
        } else if (packet.context == RNS_LINK_CONTEXT_RESPONSE) {
            (void)handle_response(link, payload, plaintext_length);
        } else if (packet.context == RNS_LINK_CONTEXT_RESOURCE_ADV &&
                   resource_advertised(link, payload, plaintext_length)) {
            /* Taken over by the resource layer. */
        } else if (packet.context == RNS_LINK_CONTEXT_RESOURCE &&
                   link->resource != NULL) {
            resource_part(link, payload, plaintext_length);
        } else if (packet.context == RNS_LINK_CONTEXT_RESOURCE_ICL &&
                   link->resource != NULL) {
            rns_request_receipt_t *receipt = link->resource_receipt;
            resource_release(link);
            if (receipt != NULL && receipt->state == RNS_REQUEST_PENDING) {
                receipt->state = RNS_REQUEST_FAILED;
                request_notify(receipt, RNS_ERROR_IO, NULL, 0U);
            }
        } else if (packet.context != RNS_LINK_CONTEXT_KEEPALIVE &&
                   link->options.packet_callback != NULL) {
            link->options.packet_callback(link, packet.context, payload,
                                          plaintext_length,
                                          link->options.callback_context);
        }
        return true;
    }
    return false;
}

static rns_status_t ingress(receive_context_t *context, const uint8_t *packet, size_t length) {
    rns_runtime_t *runtime = context->runtime;
    runtime_interface_t *source = &runtime->interfaces[context->interface_index];
    uint8_t output[RNS_MTU];
    rns_node_result result;
    source->info.packets_received++;
    source->info.bytes_received += length;
    context->processed++;
    if (!rns_node_ingress(&runtime->node, packet, length, source->info.id, 0,
                          output, sizeof(output), &result)) return RNS_ERROR_INVALID_STATE;
    if (result.action == RNS_NODE_DROP) source->info.packets_dropped++;
    bool handled_link = result.action == RNS_NODE_DELIVER &&
                        link_ingress(runtime, context->interface_index, packet, length);
    if (!handled_link && runtime->packet_callback != NULL &&
        (result.action == RNS_NODE_DELIVER || result.action == RNS_NODE_PATH_RESPONSE))
        runtime->packet_callback(runtime, packet, length, &result, runtime->callback_context);
    if (result.has_verified_announce && runtime->announce_callback != NULL)
        runtime->announce_callback(runtime, &result, runtime->callback_context);
    if (runtime->config.enable_transport &&
        (result.action == RNS_NODE_FORWARD || result.action == RNS_NODE_REBROADCAST)) {
        for (size_t i = 0U; i < runtime->interface_count; ++i)
            if (i != context->interface_index) (void)send_internal(runtime, i, output, result.output_length);
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

static rns_status_t start_interface(runtime_interface_t *destination,
                                    const rns_config_interface_t *source, uint64_t id) {
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
        if (status == RNS_OK && source->type == RNS_CONFIG_TCP_CLIENT)
            status = rns_tcp_connect(destination->tcp, source->target_host, source->target_port);
        else if (status == RNS_OK) {
            const char *listen_ip = source->listen_ip[0] != '\0' ? source->listen_ip : "0.0.0.0";
            status = rns_tcp_listen(destination->tcp, listen_ip, source->listen_port, 8);
        }
    }
    destination->info.last_error = status;
    if (status == RNS_ERROR_UNSUPPORTED) destination->info.state = RNS_RUNTIME_INTERFACE_UNSUPPORTED;
    else destination->info.state = status == RNS_OK ? RNS_RUNTIME_INTERFACE_UP : RNS_RUNTIME_INTERFACE_DOWN;
    return status;
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
    if (options != NULL) {
        runtime->packet_callback = options->packet_callback;
        runtime->announce_callback = options->announce_callback;
        runtime->callback_context = options->callback_context;
    }
    memset(&node_config, 0, sizeof(node_config));
    node_config.transport.path_capacity = options != NULL && options->path_capacity ? options->path_capacity : 128U;
    node_config.transport.dedupe_capacity = options != NULL && options->dedupe_capacity ? options->dedupe_capacity : 256U;
    node_config.transport.random_blob_history = 8U;
    node_config.transport.path_lifetime = 604800.0;
    node_config.transport.dedupe_lifetime = 60.0;
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
    for (size_t i = 0U; i < runtime->interface_count; ++i) {
        rns_status_t status = start_interface(&runtime->interfaces[i], &config->interfaces[i], i + 1U);
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
    for (size_t i = 0U; i < RNS_RUNTIME_MAX_LINKS; i++) {
        if (runtime->links[i] != NULL)
            for (size_t j = 0U; j < RNS_RUNTIME_MAX_REQUESTS; j++)
                free(runtime->links[i]->requests[j]);
        free(runtime->links[i]);
        runtime->links[i] = NULL;
    }
    for (size_t i = 0U; i < runtime->interface_count; ++i) {
        rns_udp_endpoint_destroy(runtime->interfaces[i].udp);
        rns_tcp_endpoint_destroy(runtime->interfaces[i].accepted);
        rns_tcp_endpoint_destroy(runtime->interfaces[i].tcp);
    }
    rns_node_free(&runtime->node);
    free(runtime);
}

static rns_status_t poll_tcp(rns_runtime_t *runtime, size_t index, receive_context_t *context) {
    runtime_interface_t *interface = &runtime->interfaces[index];
    rns_status_t status;
    size_t bytes = 0U;
    if (runtime->config.interfaces[index].type == RNS_CONFIG_TCP_SERVER && interface->accepted == NULL) {
        status = rns_tcp_accept(interface->tcp, &interface->accepted, RUNTIME_TCP_QUEUE);
        if (status != RNS_OK && status != RNS_ERROR_TIMEOUT) return status;
    }
    if (runtime->config.interfaces[index].type == RNS_CONFIG_TCP_CLIENT &&
        rns_tcp_state(interface->tcp) == RNS_TCP_CONNECTING) {
        status = rns_tcp_finish_connect(interface->tcp);
        if (status != RNS_OK && status != RNS_ERROR_TIMEOUT) return status;
    }
    rns_tcp_endpoint_t *connection = interface->accepted != NULL ? interface->accepted : interface->tcp;
    if (connection == NULL || rns_tcp_state(connection) != RNS_TCP_CONNECTED) return RNS_OK;
    status = rns_tcp_poll_receive(connection, tcp_receive, context, &bytes);
    interface->info.bytes_received += 0U; /* ingress accounts decoded bytes. */
    if (status == RNS_OK) status = rns_tcp_flush(connection, &bytes);
    return status;
}

rns_status_t rns_runtime_poll(rns_runtime_t *runtime, size_t max_packets, size_t *processed) {
    rns_status_t first_error = RNS_OK;
    if (runtime == NULL || processed == NULL) return RNS_ERROR_INVALID_ARGUMENT;
    *processed = 0U;
    if (max_packets == 0U) max_packets = RUNTIME_DEFAULT_WORK;
    (void)rns_transport_expire(&runtime->node.transport);
    double now = runtime_clock(NULL);
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
        if (link->protocol.state == RNS_LINK_PENDING &&
            rns_link_check_timeout(&link->protocol)) {
            fail_pending_requests(link, RNS_ERROR_TIMEOUT);
            link_notify(link, RNS_LINK_CLOSED, RNS_ERROR_TIMEOUT);
        } else if (link->protocol.state == RNS_LINK_ACTIVE &&
                 now - link->last_inbound >= 2.0 * link->keepalive_seconds +
                                             link->protocol.rtt * 4.0 + 5.0) {
            link->protocol.state = RNS_LINK_CLOSED;
            fail_pending_requests(link, RNS_ERROR_TIMEOUT);
            link_notify(link, RNS_LINK_CLOSED, RNS_ERROR_TIMEOUT);
        } else if (link->protocol.state == RNS_LINK_ACTIVE &&
                   now - link->last_outbound >= link->keepalive_seconds) {
            const uint8_t keepalive = 0xffU;
            (void)rns_runtime_link_send(link, RNS_LINK_CONTEXT_KEEPALIVE,
                                        &keepalive, 1U);
        }
    }
    for (size_t i = 0U; i < runtime->interface_count && *processed < max_packets; ++i) {
        runtime_interface_t *interface = &runtime->interfaces[i];
        receive_context_t context = {runtime, i, max_packets - *processed, 0U};
        rns_status_t status = RNS_OK;
        if (interface->info.state != RNS_RUNTIME_INTERFACE_UP) continue;
        if (interface->udp != NULL) {
            size_t count = 0U;
            status = rns_udp_poll(interface->udp, context.remaining, udp_receive, &context, &count);
        } else if (interface->tcp != NULL) status = poll_tcp(runtime, i, &context);
        *processed += context.processed;
        if (status != RNS_OK) {
            interface->info.last_error = status;
            if (first_error == RNS_OK) first_error = status;
        }
    }
    return first_error;
}

rns_status_t rns_runtime_send(rns_runtime_t *runtime, size_t index,
                              const uint8_t *packet, size_t length) {
    return send_internal(runtime, index, packet, length);
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
    return rns_node_register_destination(&runtime->node, hash) ? RNS_OK : RNS_ERROR_OVERFLOW;
}

rns_status_t rns_runtime_unregister_destination(rns_runtime_t *runtime, const uint8_t hash[16]) {
    if (runtime == NULL || hash == NULL) return RNS_ERROR_INVALID_ARGUMENT;
    return rns_node_unregister_destination(&runtime->node, hash) ? RNS_OK : RNS_ERROR_NOT_FOUND;
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
    request.transport_type = 1U;
    request.destination_type = 2U;
    request.packet_type = 0U;
    request.context = RNS_NODE_PATH_REQUEST_CONTEXT;
    request.data = body;
    request.data_length = body_length;
    if (!rns_packet_encode(&request, raw, sizeof raw, &raw_length))
        return RNS_ERROR_INVALID_STATE;
    rns_status_t last_error = RNS_ERROR_INVALID_STATE;
    bool sent_on_live_interface = false;
    for (size_t i = 0U; i < runtime->interface_count; i++) {
        if (runtime->interfaces[i].info.state != RNS_RUNTIME_INTERFACE_UP) continue;
        sent_on_live_interface = true;
        rns_status_t status = send_internal(runtime, i, raw, raw_length);
        if (status == RNS_OK) return RNS_OK;
        last_error = status;
    }
    return sent_on_live_interface ? last_error : RNS_ERROR_INVALID_STATE;
}

rns_status_t rns_runtime_path_lookup(const rns_runtime_t *runtime, const uint8_t hash[16],
                                     rns_path_entry *path) {
    if (runtime == NULL || hash == NULL || path == NULL) return RNS_ERROR_INVALID_ARGUMENT;
    const rns_path_entry *found = rns_transport_lookup((rns_transport *)&runtime->node.transport, hash);
    if (found == NULL) return RNS_ERROR_NOT_FOUND;
    *path = *found;
    return RNS_OK;
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
    return link_send_plain(link, context, plaintext, plaintext_length, NULL);
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

void rns_runtime_link_destroy(rns_runtime_link_t *link) {
    if (link == NULL) return;
    rns_runtime_t *runtime = link->runtime;
    if (link->protocol.state == RNS_LINK_ACTIVE)
        (void)rns_runtime_link_send(link, RNS_LINK_CONTEXT_CLOSE,
                                    link->protocol.link_id,
                                    sizeof link->protocol.link_id);
    link->protocol.state = RNS_LINK_CLOSED;
    rns_resource_destroy(link->resource);
    link->resource = NULL;
    link->resource_receipt = NULL;
    fail_pending_requests(link, RNS_ERROR_INVALID_STATE);
    if (runtime != NULL) {
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
