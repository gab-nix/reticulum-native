#define _DARWIN_C_SOURCE
#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "reticulum/kiss.h"

#include "reticulum/packet.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#define KISS_DEVICE_MAX 255U
#define KISS_QUEUE_DEPTH 8U
#define KISS_ENCODED_MAX (2U * RNS_MTU + 3U)
#define KISS_READ_SIZE 1024U
#define KISS_FLOW_TIMEOUT 5.0

typedef struct kiss_packet {
    uint8_t data[RNS_MTU];
    size_t length;
} kiss_packet_t;

struct rns_kiss_endpoint {
    rns_kiss_options_t options;
    char device[KISS_DEVICE_MAX + 1U];
    int descriptor;
    rns_kiss_info_t info;
    rns_kiss_decoder_t decoder;
    uint8_t decoder_storage[RNS_MTU];
    kiss_packet_t queue[KISS_QUEUE_DEPTH];
    size_t queue_head;
    size_t queue_count;
    kiss_packet_t receive_queue[KISS_QUEUE_DEPTH];
    size_t receive_head;
    size_t receive_count;
    uint8_t output[KISS_ENCODED_MAX];
    size_t output_offset;
    size_t output_length;
    bool output_packet;
    double configure_at;
    double reconnect_at;
    double flow_locked_at;
    size_t configure_index;
    bool configured;
    bool flow_ready;
};

static double default_clock(void *context) {
    struct timespec value;
    (void)context;
    if (clock_gettime(CLOCK_MONOTONIC, &value) != 0) return 0.0;
    return (double)value.tv_sec + (double)value.tv_nsec / 1000000000.0;
}

static double endpoint_clock(const rns_kiss_endpoint_t *endpoint) {
    return endpoint->options.clock != NULL
               ? endpoint->options.clock(endpoint->options.clock_context)
               : default_clock(NULL);
}

void rns_kiss_options_init(rns_kiss_options_t *options) {
    if (options == NULL) return;
    memset(options, 0, sizeof(*options));
    options->speed = 9600U;
    options->data_bits = 8U;
    options->parity = 'N';
    options->stop_bits = 1U;
    options->preamble_ms = 350U;
    options->tx_tail_ms = 20U;
    options->persistence = 64U;
    options->slot_time_ms = 20U;
    options->configure_delay_seconds = 2.0;
    options->reconnect_seconds = 5.0;
}

static bool valid_options(const rns_kiss_options_t *options, size_t *device_length) {
    if (options == NULL || options->device == NULL) return false;
    *device_length = strnlen(options->device, KISS_DEVICE_MAX + 1U);
    if (*device_length == 0U || *device_length > KISS_DEVICE_MAX ||
        options->speed == 0U || options->data_bits < 5U ||
        options->data_bits > 8U ||
        (options->parity != 'N' && options->parity != 'E' &&
         options->parity != 'O') ||
        (options->stop_bits != 1U && options->stop_bits != 2U) ||
        options->configure_delay_seconds < 0.0 ||
        options->reconnect_seconds <= 0.0)
        return false;
    return true;
}

static bool baud_value(uint32_t speed, speed_t *value) {
    switch (speed) {
        case 1200U: *value = B1200; return true;
        case 2400U: *value = B2400; return true;
        case 4800U: *value = B4800; return true;
        case 9600U: *value = B9600; return true;
        case 19200U: *value = B19200; return true;
        case 38400U: *value = B38400; return true;
#ifdef B57600
        case 57600U: *value = B57600; return true;
#endif
#ifdef B115200
        case 115200U: *value = B115200; return true;
#endif
#ifdef B230400
        case 230400U: *value = B230400; return true;
#endif
        default: return false;
    }
}

static rns_status_t configure_serial(int descriptor,
                                     const rns_kiss_options_t *options) {
    struct termios attributes;
    speed_t baud;
    if (!baud_value(options->speed, &baud)) return RNS_ERROR_UNSUPPORTED;
    if (tcgetattr(descriptor, &attributes) != 0) return RNS_ERROR_IO;
    cfmakeraw(&attributes);
    attributes.c_cflag &= (tcflag_t)~(CSIZE | PARENB | PARODD | CSTOPB);
    switch (options->data_bits) {
        case 5U: attributes.c_cflag |= CS5; break;
        case 6U: attributes.c_cflag |= CS6; break;
        case 7U: attributes.c_cflag |= CS7; break;
        default: attributes.c_cflag |= CS8; break;
    }
    if (options->parity != 'N') attributes.c_cflag |= PARENB;
    if (options->parity == 'O') attributes.c_cflag |= PARODD;
    if (options->stop_bits == 2U) attributes.c_cflag |= CSTOPB;
    attributes.c_cflag |= CLOCAL | CREAD;
    attributes.c_cc[VMIN] = 0;
    attributes.c_cc[VTIME] = 0;
    if (cfsetispeed(&attributes, baud) != 0 ||
        cfsetospeed(&attributes, baud) != 0 ||
        tcsetattr(descriptor, TCSANOW, &attributes) != 0)
        return RNS_ERROR_IO;
    return RNS_OK;
}

static void mark_down(rns_kiss_endpoint_t *endpoint, rns_status_t reason) {
    if (endpoint->descriptor >= 0) {
        (void)close(endpoint->descriptor);
        endpoint->descriptor = -1;
        endpoint->info.connections_lost++;
    }
    endpoint->info.state = RNS_KISS_DOWN;
    endpoint->info.last_error = reason;
    endpoint->reconnect_at = endpoint_clock(endpoint) +
                             endpoint->options.reconnect_seconds;
    endpoint->configured = false;
    endpoint->configure_index = 0U;
    endpoint->output_offset = 0U;
    endpoint->output_length = 0U;
    endpoint->output_packet = false;
    endpoint->flow_ready = !endpoint->options.flow_control;
    rns_kiss_decoder_reset(&endpoint->decoder);
}

static rns_status_t open_serial(rns_kiss_endpoint_t *endpoint) {
    endpoint->info.connection_attempts++;
    int descriptor = open(endpoint->device, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (descriptor < 0) return RNS_ERROR_IO;
    rns_status_t status = configure_serial(descriptor, &endpoint->options);
    if (status != RNS_OK) {
        (void)close(descriptor);
        return status;
    }
    endpoint->descriptor = descriptor;
    endpoint->info.connections_established++;
    endpoint->info.state = RNS_KISS_CONFIGURING;
    endpoint->info.last_error = RNS_OK;
    endpoint->configure_at = endpoint_clock(endpoint) +
                             endpoint->options.configure_delay_seconds;
    endpoint->flow_locked_at = endpoint_clock(endpoint);
    return RNS_OK;
}

rns_status_t rns_kiss_endpoint_create(rns_kiss_endpoint_t **output,
                                      const rns_kiss_options_t *options) {
    size_t device_length = 0U;
    if (output == NULL || !valid_options(options, &device_length))
        return RNS_ERROR_INVALID_ARGUMENT;
    *output = NULL;
    rns_kiss_endpoint_t *endpoint = calloc(1U, sizeof(*endpoint));
    if (endpoint == NULL) return RNS_ERROR_NO_MEMORY;
    endpoint->options = *options;
    memcpy(endpoint->device, options->device, device_length);
    endpoint->device[device_length] = '\0';
    endpoint->options.device = endpoint->device;
    endpoint->descriptor = -1;
    endpoint->info.state = RNS_KISS_DOWN;
    endpoint->flow_ready = !options->flow_control;
    rns_kiss_decoder_init(&endpoint->decoder, endpoint->decoder_storage,
                          sizeof(endpoint->decoder_storage));
    rns_status_t status = open_serial(endpoint);
    if (status != RNS_OK) {
        endpoint->info.last_error = status;
        endpoint->reconnect_at = endpoint_clock(endpoint) +
                                 endpoint->options.reconnect_seconds;
    }
    *output = endpoint;
    return RNS_OK;
}

void rns_kiss_endpoint_destroy(rns_kiss_endpoint_t *endpoint) {
    if (endpoint == NULL) return;
    if (endpoint->descriptor >= 0) (void)close(endpoint->descriptor);
    free(endpoint);
}

static rns_status_t queue_command(rns_kiss_endpoint_t *endpoint,
                                  uint8_t command, uint8_t value) {
    if (endpoint->output_length != 0U) return RNS_ERROR_INVALID_STATE;
    return rns_kiss_encode_command(0U, command, &value, 1U, endpoint->output,
                                   sizeof(endpoint->output),
                                   &endpoint->output_length);
}

static rns_status_t flush_output(rns_kiss_endpoint_t *endpoint) {
    while (endpoint->output_offset < endpoint->output_length) {
        ssize_t written = write(endpoint->descriptor,
                                endpoint->output + endpoint->output_offset,
                                endpoint->output_length - endpoint->output_offset);
        if (written > 0) {
            endpoint->output_offset += (size_t)written;
            continue;
        }
        if (written < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
            return RNS_OK;
        return RNS_ERROR_IO;
    }
    endpoint->output_offset = 0U;
    endpoint->output_length = 0U;
    return RNS_OK;
}

static rns_status_t configure_device(rns_kiss_endpoint_t *endpoint) {
    const uint8_t commands[] = {
        RNS_KISS_TXDELAY_COMMAND, RNS_KISS_TXTAIL_COMMAND,
        RNS_KISS_PERSISTENCE_COMMAND, RNS_KISS_SLOTTIME_COMMAND,
        RNS_KISS_READY_COMMAND
    };
    uint8_t values[] = {
        (uint8_t)(endpoint->options.preamble_ms / 10U > 255U
                      ? 255U : endpoint->options.preamble_ms / 10U),
        (uint8_t)(endpoint->options.tx_tail_ms / 10U > 255U
                      ? 255U : endpoint->options.tx_tail_ms / 10U),
        endpoint->options.persistence,
        (uint8_t)(endpoint->options.slot_time_ms / 10U > 255U
                      ? 255U : endpoint->options.slot_time_ms / 10U),
        1U
    };
    rns_status_t status = flush_output(endpoint);
    if (status != RNS_OK || endpoint->output_length != 0U) return status;
    while (endpoint->configure_index < sizeof(commands)) {
        size_t i = endpoint->configure_index;
        status = queue_command(endpoint, commands[i], values[i]);
        if (status != RNS_OK) return status;
        endpoint->configure_index++;
        status = flush_output(endpoint);
        if (status != RNS_OK || endpoint->output_length != 0U) return status;
    }
    endpoint->configured = true;
    /* Configuration makes the first transmit immediately eligible. Each
     * subsequent flow-controlled frame waits for CMD_READY or the timeout. */
    endpoint->flow_ready = true;
    endpoint->info.state = RNS_KISS_UP;
    return RNS_OK;
}

rns_status_t rns_kiss_endpoint_send(rns_kiss_endpoint_t *endpoint,
                                    const uint8_t *packet,
                                    size_t packet_length) {
    if (endpoint == NULL || packet == NULL || packet_length == 0U ||
        packet_length > RNS_MTU)
        return RNS_ERROR_INVALID_ARGUMENT;
    if (endpoint->info.state == RNS_KISS_DOWN) return RNS_ERROR_INVALID_STATE;
    if (endpoint->queue_count == KISS_QUEUE_DEPTH) {
        endpoint->info.packets_dropped++;
        return RNS_ERROR_OVERFLOW;
    }
    size_t tail = (endpoint->queue_head + endpoint->queue_count) %
                  KISS_QUEUE_DEPTH;
    memcpy(endpoint->queue[tail].data, packet, packet_length);
    endpoint->queue[tail].length = packet_length;
    endpoint->queue_count++;
    endpoint->info.pending_packets = endpoint->queue_count;
    return RNS_OK;
}

typedef struct receive_adapter {
    rns_kiss_endpoint_t *endpoint;
} receive_adapter_t;

static rns_status_t receive_frame(uint8_t port, uint8_t command,
                                  const uint8_t *frame, size_t frame_length,
                                  void *opaque) {
    receive_adapter_t *adapter = opaque;
    /* Pinned KISSInterface masks the channel nibble and accepts all ports. */
    (void)port;
    if (command == RNS_KISS_READY_COMMAND) {
        adapter->endpoint->flow_ready = true;
        return RNS_OK;
    }
    if (command != RNS_KISS_DATA_COMMAND || frame_length == 0U) return RNS_OK;
    if (adapter->endpoint->receive_count == KISS_QUEUE_DEPTH) {
        adapter->endpoint->info.packets_dropped++;
        return RNS_OK;
    }
    size_t tail = (adapter->endpoint->receive_head +
                   adapter->endpoint->receive_count) % KISS_QUEUE_DEPTH;
    memcpy(adapter->endpoint->receive_queue[tail].data, frame, frame_length);
    adapter->endpoint->receive_queue[tail].length = frame_length;
    adapter->endpoint->receive_count++;
    return RNS_OK;
}

static rns_status_t poll_read(rns_kiss_endpoint_t *endpoint,
                              receive_adapter_t *adapter) {
    uint8_t input[KISS_READ_SIZE];
    ssize_t length = read(endpoint->descriptor, input, sizeof(input));
    if (length > 0)
        return rns_kiss_decoder_feed_commands(&endpoint->decoder, input,
                                              (size_t)length, receive_frame,
                                              adapter);
    {
        if (length == 0) {
            struct pollfd descriptor = {endpoint->descriptor, POLLIN, 0};
            if (poll(&descriptor, 1, 0) > 0 &&
                (descriptor.revents & (POLLHUP | POLLERR | POLLNVAL)) != 0)
                return RNS_ERROR_IO;
            return RNS_OK;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK)
            return RNS_OK;
        return RNS_ERROR_IO;
    }
}

static rns_status_t dispatch_received(rns_kiss_endpoint_t *endpoint,
                                      size_t limit,
                                      rns_frame_callback_t callback,
                                      void *context, size_t *processed) {
    while (endpoint->receive_count != 0U && *processed < limit) {
        kiss_packet_t *packet = &endpoint->receive_queue[endpoint->receive_head];
        rns_status_t status = callback(packet->data, packet->length, context);
        if (status != RNS_OK) return status;
        endpoint->info.packets_received++;
        endpoint->info.bytes_received += packet->length;
        endpoint->receive_head = (endpoint->receive_head + 1U) % KISS_QUEUE_DEPTH;
        endpoint->receive_count--;
        (*processed)++;
    }
    return RNS_OK;
}

static rns_status_t poll_write(rns_kiss_endpoint_t *endpoint) {
    rns_status_t status = flush_output(endpoint);
    if (status != RNS_OK || endpoint->output_length != 0U) return status;
    if (endpoint->output_packet) {
        kiss_packet_t *completed = &endpoint->queue[endpoint->queue_head];
        endpoint->info.packets_sent++;
        endpoint->info.bytes_sent += completed->length;
        endpoint->queue_head = (endpoint->queue_head + 1U) % KISS_QUEUE_DEPTH;
        endpoint->queue_count--;
        endpoint->info.pending_packets = endpoint->queue_count;
        endpoint->output_packet = false;
        if (endpoint->options.flow_control) {
            endpoint->flow_ready = false;
            endpoint->flow_locked_at = endpoint_clock(endpoint);
        }
        return RNS_OK;
    }
    if (endpoint->queue_count == 0U || !endpoint->flow_ready) return RNS_OK;
    kiss_packet_t *packet = &endpoint->queue[endpoint->queue_head];
    status = rns_kiss_encode(0U, packet->data, packet->length,
                             endpoint->output, sizeof(endpoint->output),
                             &endpoint->output_length);
    if (status != RNS_OK) return status;
    endpoint->output_packet = true;
    status = flush_output(endpoint);
    if (status != RNS_OK) return status;
    if (endpoint->output_length == 0U) {
        endpoint->info.packets_sent++;
        endpoint->info.bytes_sent += packet->length;
        endpoint->queue_head = (endpoint->queue_head + 1U) % KISS_QUEUE_DEPTH;
        endpoint->queue_count--;
        endpoint->info.pending_packets = endpoint->queue_count;
        endpoint->output_packet = false;
        if (endpoint->options.flow_control) {
            endpoint->flow_ready = false;
            endpoint->flow_locked_at = endpoint_clock(endpoint);
        }
    }
    return RNS_OK;
}

rns_status_t rns_kiss_endpoint_poll(rns_kiss_endpoint_t *endpoint,
                                    size_t max_packets,
                                    rns_frame_callback_t callback,
                                    void *context, size_t *processed) {
    if (endpoint == NULL || callback == NULL || processed == NULL)
        return RNS_ERROR_INVALID_ARGUMENT;
    *processed = 0U;
    double now = endpoint_clock(endpoint);
    if (endpoint->descriptor < 0) {
        if (now < endpoint->reconnect_at) return RNS_OK;
        rns_status_t status = open_serial(endpoint);
        if (status != RNS_OK) {
            endpoint->info.last_error = status;
            endpoint->reconnect_at = now + endpoint->options.reconnect_seconds;
            return status;
        }
    }
    if (!endpoint->configured && now >= endpoint->configure_at) {
        rns_status_t status = configure_device(endpoint);
        if (status != RNS_OK) {
            mark_down(endpoint, status);
            return status;
        }
    }
    if (endpoint->options.flow_control && !endpoint->flow_ready &&
        now - endpoint->flow_locked_at >= KISS_FLOW_TIMEOUT)
        endpoint->flow_ready = true;
    size_t limit = max_packets == 0U ? KISS_QUEUE_DEPTH : max_packets;
    rns_status_t status = dispatch_received(endpoint, limit, callback, context,
                                            processed);
    receive_adapter_t adapter = {endpoint};
    if (status == RNS_OK && *processed < limit)
        status = poll_read(endpoint, &adapter);
    if (status == RNS_OK)
        status = dispatch_received(endpoint, limit, callback, context,
                                   processed);
    if (status == RNS_OK) status = poll_write(endpoint);
    endpoint->info.malformed_frames = endpoint->decoder.malformed_frames;
    endpoint->info.oversized_frames = endpoint->decoder.oversized_frames;
    if (status != RNS_OK) mark_down(endpoint, status);
    return status;
}

rns_status_t rns_kiss_endpoint_info(const rns_kiss_endpoint_t *endpoint,
                                    rns_kiss_info_t *info) {
    if (endpoint == NULL || info == NULL) return RNS_ERROR_INVALID_ARGUMENT;
    *info = endpoint->info;
    return RNS_OK;
}
