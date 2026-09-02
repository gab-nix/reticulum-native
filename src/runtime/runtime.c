#include "reticulum/runtime.h"

#include "reticulum/destination.h"
#include "reticulum/hal.h"
#include "reticulum/packet.h"
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

struct rns_runtime {
    rns_config_t config;
    rns_node node;
    runtime_interface_t interfaces[RNS_CONFIG_MAX_INTERFACES];
    size_t interface_count;
    rns_runtime_packet_callback_t packet_callback;
    rns_runtime_announce_callback_t announce_callback;
    void *callback_context;
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
    if (runtime->packet_callback != NULL &&
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
