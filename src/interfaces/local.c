#include "reticulum/local.h"

#include "reticulum/hal.h"
#include "reticulum/packet.h"
#include "reticulum/tcp.h"

#include <stdlib.h>
#include <string.h>

#define LOCAL_DEFAULT_CLIENTS 8U
#define LOCAL_DEFAULT_QUEUE (RNS_MTU * 8U)
#define LOCAL_RECONNECT_INITIAL 8.0
#define LOCAL_RECONNECT_MAX 32.0

struct rns_local_instance {
    rns_local_options_t options;
    rns_local_info_t info;
    rns_tcp_endpoint_t *listener;
    rns_tcp_endpoint_t *client;
    rns_tcp_endpoint_t *peers[RNS_LOCAL_MAX_CLIENTS];
    double reconnect_at;
    double reconnect_delay;
};

typedef struct local_receive_context {
    rns_local_instance_t *instance;
    rns_frame_callback_t callback;
    void *callback_context;
} local_receive_context_t;

static double local_now(const rns_local_instance_t *instance) {
    uint64_t milliseconds = 0U;
    if (instance->options.clock != NULL)
        return instance->options.clock(instance->options.clock_context);
    if (rns_hal_monotonic_ms(&milliseconds) != RNS_OK) return 0.0;
    return (double)milliseconds / 1000.0;
}

static rns_status_t receive_frame(const uint8_t *frame, size_t frame_length,
                                  void *context) {
    local_receive_context_t *receive = context;
    rns_status_t status = receive->callback(
        frame, frame_length, receive->callback_context);
    if (status == RNS_OK) {
        receive->instance->info.packets_received++;
        receive->instance->info.bytes_received += frame_length;
    } else {
        receive->instance->info.packets_dropped++;
    }
    return status;
}

static void schedule_reconnect(rns_local_instance_t *instance,
                               rns_status_t reason) {
    if (instance->reconnect_delay <= 0.0)
        instance->reconnect_delay = instance->options.reconnect_initial_seconds;
    instance->reconnect_at = local_now(instance) + instance->reconnect_delay;
    if (instance->reconnect_delay < instance->options.reconnect_max_seconds) {
        instance->reconnect_delay *= 2.0;
        if (instance->reconnect_delay > instance->options.reconnect_max_seconds)
            instance->reconnect_delay = instance->options.reconnect_max_seconds;
    }
    instance->info.state = RNS_LOCAL_DOWN;
    instance->info.last_error = reason;
}

static void mark_connected(rns_local_instance_t *instance) {
    instance->info.state = RNS_LOCAL_UP;
    instance->info.last_error = RNS_OK;
    instance->info.connected_clients = 1U;
    instance->info.connections_established++;
    instance->reconnect_at = 0.0;
    instance->reconnect_delay = 0.0;
}

static rns_status_t create_endpoint(rns_tcp_endpoint_t **endpoint,
                                    size_t capacity) {
    return rns_tcp_endpoint_create(endpoint, RNS_UDP_IPV4, capacity);
}

static rns_status_t start_client(rns_local_instance_t *instance) {
    rns_status_t status;
    instance->info.connection_attempts++;
    instance->info.state = RNS_LOCAL_STARTING;
    status = rns_tcp_connect(instance->client, "127.0.0.1",
                             instance->options.port);
    if (status != RNS_OK) {
        schedule_reconnect(instance, status);
        return status;
    }
    if (rns_tcp_state(instance->client) == RNS_TCP_CONNECTED)
        mark_connected(instance);
    return RNS_OK;
}

rns_status_t rns_local_options_from_config(const rns_config_t *config,
                                           rns_local_role_t role,
                                           rns_local_options_t *options) {
    if (config == NULL || options == NULL || role > RNS_LOCAL_ROLE_CLIENT)
        return RNS_ERROR_INVALID_ARGUMENT;
    memset(options, 0, sizeof(*options));
    if (!config->share_instance) return RNS_ERROR_INVALID_STATE;
    if (config->shared_instance_type != RNS_CONFIG_SHARED_INSTANCE_TCP)
        return RNS_ERROR_UNSUPPORTED;
    options->role = role;
    options->port = config->shared_instance_port;
    return options->port != 0U ? RNS_OK : RNS_ERROR_INVALID_ARGUMENT;
}

rns_status_t rns_local_instance_create(rns_local_instance_t **output,
                                       const rns_local_options_t *options) {
    rns_local_instance_t *instance;
    rns_status_t status;
    if (output == NULL || options == NULL ||
        options->role > RNS_LOCAL_ROLE_CLIENT)
        return RNS_ERROR_INVALID_ARGUMENT;
    *output = NULL;
    instance = calloc(1U, sizeof(*instance));
    if (instance == NULL) return RNS_ERROR_NO_MEMORY;
    instance->options = *options;
    if (instance->options.port == 0U)
        instance->options.port = RNS_LOCAL_DEFAULT_PORT;
    if (instance->options.max_clients == 0U)
        instance->options.max_clients = LOCAL_DEFAULT_CLIENTS;
    if (instance->options.max_clients > RNS_LOCAL_MAX_CLIENTS ||
        instance->options.send_queue_capacity > (size_t)RNS_MTU * 64U) {
        free(instance);
        return RNS_ERROR_INVALID_ARGUMENT;
    }
    if (instance->options.send_queue_capacity == 0U)
        instance->options.send_queue_capacity = LOCAL_DEFAULT_QUEUE;
    if (instance->options.reconnect_initial_seconds <= 0.0)
        instance->options.reconnect_initial_seconds = LOCAL_RECONNECT_INITIAL;
    if (instance->options.reconnect_max_seconds <= 0.0)
        instance->options.reconnect_max_seconds = LOCAL_RECONNECT_MAX;
    if (instance->options.reconnect_max_seconds <
        instance->options.reconnect_initial_seconds) {
        free(instance);
        return RNS_ERROR_INVALID_ARGUMENT;
    }

    if (options->role != RNS_LOCAL_ROLE_CLIENT) {
        status = create_endpoint(&instance->listener,
                                 instance->options.send_queue_capacity);
        if (status == RNS_OK)
            status = rns_tcp_listen(instance->listener, "127.0.0.1",
                                    instance->options.port,
                                    (int)instance->options.max_clients);
        if (status == RNS_OK) {
            instance->info.role = RNS_LOCAL_ROLE_SERVER;
            instance->info.state = RNS_LOCAL_UP;
            instance->info.last_error = RNS_OK;
            *output = instance;
            return RNS_OK;
        }
        rns_tcp_endpoint_destroy(instance->listener);
        instance->listener = NULL;
        if (options->role == RNS_LOCAL_ROLE_SERVER) {
            free(instance);
            return status;
        }
    }

    status = create_endpoint(&instance->client,
                             instance->options.send_queue_capacity);
    if (status != RNS_OK) {
        free(instance);
        return status;
    }
    instance->info.role = RNS_LOCAL_ROLE_CLIENT;
    (void)start_client(instance);
    *output = instance;
    return RNS_OK;
}

void rns_local_instance_destroy(rns_local_instance_t *instance) {
    if (instance == NULL) return;
    for (size_t i = 0U; i < instance->options.max_clients; ++i)
        rns_tcp_endpoint_destroy(instance->peers[i]);
    rns_tcp_endpoint_destroy(instance->client);
    rns_tcp_endpoint_destroy(instance->listener);
    free(instance);
}

static rns_status_t poll_connection(rns_tcp_endpoint_t *connection,
                                    local_receive_context_t *receive) {
    size_t transferred = 0U;
    rns_status_t status = rns_tcp_poll_receive(
        connection, receive_frame, receive, &transferred);
    if (status == RNS_OK)
        status = rns_tcp_flush(connection, &transferred);
    return status;
}

static rns_status_t poll_server(rns_local_instance_t *instance,
                                local_receive_context_t *receive) {
    rns_status_t first_error = RNS_OK;
    for (size_t i = 0U; i < instance->options.max_clients; ++i) {
        if (instance->peers[i] == NULL) {
            rns_status_t accepted = rns_tcp_accept(
                instance->listener, &instance->peers[i],
                instance->options.send_queue_capacity);
            if (accepted == RNS_OK) {
                instance->info.connected_clients++;
                instance->info.connections_established++;
            } else if (accepted != RNS_ERROR_TIMEOUT) {
                instance->info.last_error = accepted;
                return accepted;
            }
            break;
        }
    }
    for (size_t i = 0U; i < instance->options.max_clients; ++i) {
        if (instance->peers[i] == NULL) continue;
        rns_status_t status = poll_connection(instance->peers[i], receive);
        if (status != RNS_OK &&
            rns_tcp_state(instance->peers[i]) == RNS_TCP_DISCONNECTED) {
            rns_tcp_endpoint_destroy(instance->peers[i]);
            instance->peers[i] = NULL;
            instance->info.connected_clients--;
            instance->info.connections_lost++;
            continue;
        }
        if (status != RNS_OK && first_error == RNS_OK) first_error = status;
    }
    instance->info.last_error = first_error;
    return first_error;
}

static rns_status_t poll_client(rns_local_instance_t *instance,
                                local_receive_context_t *receive) {
    rns_status_t status;
    if (rns_tcp_state(instance->client) == RNS_TCP_DISCONNECTED) {
        if (local_now(instance) < instance->reconnect_at) return RNS_OK;
        status = start_client(instance);
        if (status != RNS_OK) return status;
    }
    if (rns_tcp_state(instance->client) == RNS_TCP_CONNECTING) {
        status = rns_tcp_finish_connect(instance->client);
        if (status == RNS_ERROR_TIMEOUT) return RNS_OK;
        if (status != RNS_OK) {
            schedule_reconnect(instance, status);
            return status;
        }
        mark_connected(instance);
    }
    if (rns_tcp_state(instance->client) != RNS_TCP_CONNECTED) return RNS_OK;
    status = poll_connection(instance->client, receive);
    if (status != RNS_OK &&
        rns_tcp_state(instance->client) == RNS_TCP_DISCONNECTED) {
        instance->info.connected_clients = 0U;
        instance->info.connections_lost++;
        schedule_reconnect(instance, status);
    }
    return status;
}

rns_status_t rns_local_instance_poll(rns_local_instance_t *instance,
                                     rns_frame_callback_t callback,
                                     void *callback_context) {
    if (instance == NULL || callback == NULL)
        return RNS_ERROR_INVALID_ARGUMENT;
    local_receive_context_t receive = {instance, callback, callback_context};
    return instance->info.role == RNS_LOCAL_ROLE_SERVER
               ? poll_server(instance, &receive)
               : poll_client(instance, &receive);
}

rns_status_t rns_local_instance_send(rns_local_instance_t *instance,
                                     const uint8_t *packet,
                                     size_t packet_length) {
    if (instance == NULL || packet == NULL || packet_length == 0U)
        return RNS_ERROR_INVALID_ARGUMENT;
    if (packet_length > RNS_MTU) return RNS_ERROR_OVERFLOW;
    size_t queued = 0U;
    if (instance->info.role == RNS_LOCAL_ROLE_CLIENT) {
        if (instance->info.state != RNS_LOCAL_UP ||
            rns_tcp_state(instance->client) != RNS_TCP_CONNECTED)
            return instance->info.last_error != RNS_OK
                       ? instance->info.last_error
                       : RNS_ERROR_INVALID_STATE;
        rns_status_t status = rns_tcp_queue_frame(instance->client, packet,
                                                   packet_length);
        if (status != RNS_OK) {
            instance->info.packets_dropped++;
            instance->info.last_error = status;
            return status;
        }
        queued = 1U;
    } else {
        for (size_t i = 0U; i < instance->options.max_clients; ++i) {
            if (instance->peers[i] == NULL) continue;
            rns_status_t status = rns_tcp_queue_frame(
                instance->peers[i], packet, packet_length);
            if (status == RNS_OK)
                queued++;
            else
                instance->info.packets_dropped++;
        }
        if (queued == 0U) {
            instance->info.packets_dropped++;
            return RNS_ERROR_NOT_FOUND;
        }
    }
    instance->info.packets_sent++;
    instance->info.bytes_sent += packet_length;
    return RNS_OK;
}

rns_status_t rns_local_instance_info(const rns_local_instance_t *instance,
                                     rns_local_info_t *info) {
    if (instance == NULL || info == NULL) return RNS_ERROR_INVALID_ARGUMENT;
    *info = instance->info;
    return RNS_OK;
}
