#define _DARWIN_C_SOURCE
#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "reticulum/rnode.h"

#include "reticulum/packet.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#define RNODE_DEVICE_MAX 255U
#define RNODE_QUEUE_DEPTH 8U
#define RNODE_HW_MTU 508U
#define RNODE_OUTPUT_CAPACITY (2U * RNS_MTU + 64U)
#define RNODE_INPUT_CAPACITY 1024U
#define RNODE_FRAME_TIMEOUT 0.1

#define RNODE_CMD_DATA 0x00U
#define RNODE_CMD_FREQUENCY 0x01U
#define RNODE_CMD_BANDWIDTH 0x02U
#define RNODE_CMD_TXPOWER 0x03U
#define RNODE_CMD_SF 0x04U
#define RNODE_CMD_CR 0x05U
#define RNODE_CMD_RADIO_STATE 0x06U
#define RNODE_CMD_ST_ALOCK 0x0bU
#define RNODE_CMD_LT_ALOCK 0x0cU
#define RNODE_CMD_DETECT 0x08U
#define RNODE_CMD_LEAVE 0x0aU
#define RNODE_CMD_READY 0x0fU
#define RNODE_CMD_PLATFORM 0x48U
#define RNODE_CMD_MCU 0x49U
#define RNODE_CMD_FW_VERSION 0x50U
#define RNODE_CMD_ERROR 0x90U
#define RNODE_DETECT_REQUEST 0x73U
#define RNODE_DETECT_RESPONSE 0x46U
#define RNODE_RADIO_OFF 0x00U
#define RNODE_RADIO_ON 0x01U
#define RNODE_ERROR_MEMORY_LOW 0x05U
#define RNODE_ERROR_MODEM_TIMEOUT 0x06U

typedef struct rnode_packet {
    uint8_t bytes[RNS_MTU];
    size_t length;
} rnode_packet_t;

typedef struct rnode_decoder {
    uint8_t command;
    uint8_t bytes[RNODE_HW_MTU];
    size_t length;
    size_t malformed;
    size_t oversized;
    bool synchronized;
    bool have_command;
    bool escaped;
    bool discarding;
} rnode_decoder_t;

struct rns_rnode_endpoint {
    rns_rnode_options_t options;
    char device[RNODE_DEVICE_MAX + 1U];
    int descriptor;
    rns_rnode_info_t info;
    rnode_decoder_t decoder;
    rnode_packet_t transmit[RNODE_QUEUE_DEPTH];
    size_t transmit_head;
    size_t transmit_count;
    rnode_packet_t receive[RNODE_QUEUE_DEPTH];
    size_t receive_head;
    size_t receive_count;
    uint8_t output[RNODE_OUTPUT_CAPACITY];
    size_t output_offset;
    size_t output_length;
    bool output_packet;
    bool leave_sent;
    bool detached;
    bool flow_ready;
    bool detected;
    bool firmware_seen;
    bool frequency_seen;
    bool bandwidth_seen;
    bool tx_power_seen;
    bool sf_seen;
    bool cr_seen;
    bool radio_seen;
    bool short_lock_seen;
    bool long_lock_seen;
    uint16_t reported_short_lock;
    uint16_t reported_long_lock;
    double state_at;
    double deadline;
    double reconnect_at;
    double last_input_at;
};

static double default_clock(void *context) {
    struct timespec value;
    (void)context;
    if (clock_gettime(CLOCK_MONOTONIC, &value) != 0) return 0.0;
    return (double)value.tv_sec + (double)value.tv_nsec / 1000000000.0;
}

static double endpoint_clock(const rns_rnode_endpoint_t *endpoint) {
    return endpoint->options.clock != NULL
               ? endpoint->options.clock(endpoint->options.clock_context)
               : default_clock(NULL);
}

void rns_rnode_options_init(rns_rnode_options_t *options) {
    if (options == NULL) return;
    memset(options, 0, sizeof(*options));
    options->startup_delay_seconds = 2.0;
    options->detect_timeout_seconds = 0.2;
    options->validation_timeout_seconds = 0.25;
    options->reconnect_seconds = 5.0;
}

static bool valid_options(const rns_rnode_options_t *options,
                          size_t *device_length) {
    if (options == NULL || options->device == NULL) return false;
    *device_length = strnlen(options->device, RNODE_DEVICE_MAX + 1U);
    return *device_length > 0U && *device_length <= RNODE_DEVICE_MAX &&
           options->frequency >= RNS_RNODE_FREQUENCY_MIN &&
           options->frequency <= RNS_RNODE_FREQUENCY_MAX &&
           options->bandwidth >= 7800U && options->bandwidth <= 1625000U &&
           options->tx_power <= 37U && options->spreading_factor >= 5U &&
           options->spreading_factor <= 12U && options->coding_rate >= 5U &&
           options->coding_rate <= 8U &&
           (!options->short_airtime_limit_set ||
            options->short_airtime_limit_hundredths <= 10000U) &&
           (!options->long_airtime_limit_set ||
            options->long_airtime_limit_hundredths <= 10000U) &&
           options->startup_delay_seconds >= 0.0 &&
           options->detect_timeout_seconds > 0.0 &&
           options->validation_timeout_seconds > 0.0 &&
           options->reconnect_seconds > 0.0;
}

static rns_status_t configure_serial(int descriptor) {
    struct termios attributes;
    if (tcgetattr(descriptor, &attributes) != 0) return RNS_ERROR_IO;
    cfmakeraw(&attributes);
    attributes.c_cflag &= (tcflag_t)~(CSIZE | PARENB | PARODD | CSTOPB);
    attributes.c_cflag |= CS8 | CLOCAL | CREAD;
    attributes.c_cc[VMIN] = 0;
    attributes.c_cc[VTIME] = 0;
    if (cfsetispeed(&attributes, B115200) != 0 ||
        cfsetospeed(&attributes, B115200) != 0 ||
        tcsetattr(descriptor, TCSANOW, &attributes) != 0)
        return RNS_ERROR_IO;
    return RNS_OK;
}

static void reset_decoder(rnode_decoder_t *decoder) {
    decoder->length = 0U;
    decoder->synchronized = false;
    decoder->have_command = false;
    decoder->escaped = false;
    decoder->discarding = false;
}

static void reset_reports(rns_rnode_endpoint_t *endpoint) {
    endpoint->detected = false;
    endpoint->firmware_seen = false;
    endpoint->frequency_seen = false;
    endpoint->bandwidth_seen = false;
    endpoint->tx_power_seen = false;
    endpoint->sf_seen = false;
    endpoint->cr_seen = false;
    endpoint->radio_seen = false;
    endpoint->short_lock_seen = false;
    endpoint->long_lock_seen = false;
    endpoint->info.radio_on = false;
}

static void mark_down(rns_rnode_endpoint_t *endpoint, rns_status_t reason) {
    if (endpoint->descriptor >= 0) {
        (void)close(endpoint->descriptor);
        endpoint->descriptor = -1;
        endpoint->info.connections_lost++;
    }
    endpoint->info.state = RNS_RNODE_DOWN;
    endpoint->info.last_error = reason;
    endpoint->reconnect_at = endpoint_clock(endpoint) +
                             endpoint->options.reconnect_seconds;
    endpoint->output_offset = 0U;
    endpoint->output_length = 0U;
    endpoint->output_packet = false;
    if (!endpoint->detached) endpoint->leave_sent = false;
    endpoint->flow_ready = false;
    endpoint->deadline = 0.0;
    reset_decoder(&endpoint->decoder);
    reset_reports(endpoint);
}

static rns_status_t open_serial(rns_rnode_endpoint_t *endpoint) {
    endpoint->info.connection_attempts++;
    int descriptor = open(endpoint->device, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (descriptor < 0) return RNS_ERROR_IO;
    rns_status_t status = configure_serial(descriptor);
    if (status != RNS_OK) {
        (void)close(descriptor);
        return status;
    }
    endpoint->descriptor = descriptor;
    endpoint->info.connections_established++;
    endpoint->info.state = RNS_RNODE_STARTING;
    endpoint->info.last_error = RNS_OK;
    endpoint->state_at = endpoint_clock(endpoint) +
                         endpoint->options.startup_delay_seconds;
    endpoint->last_input_at = endpoint_clock(endpoint);
    reset_reports(endpoint);
    return RNS_OK;
}

rns_status_t rns_rnode_endpoint_create(rns_rnode_endpoint_t **output,
                                       const rns_rnode_options_t *options) {
    size_t device_length = 0U;
    if (output == NULL || !valid_options(options, &device_length))
        return RNS_ERROR_INVALID_ARGUMENT;
    *output = NULL;
    rns_rnode_endpoint_t *endpoint = calloc(1U, sizeof(*endpoint));
    if (endpoint == NULL) return RNS_ERROR_NO_MEMORY;
    endpoint->options = *options;
    memcpy(endpoint->device, options->device, device_length);
    endpoint->device[device_length] = '\0';
    endpoint->options.device = endpoint->device;
    endpoint->descriptor = -1;
    endpoint->info.state = RNS_RNODE_DOWN;
    rns_status_t status = open_serial(endpoint);
    if (status != RNS_OK) {
        endpoint->info.last_error = status;
        endpoint->reconnect_at = endpoint_clock(endpoint) +
                                 endpoint->options.reconnect_seconds;
    }
    *output = endpoint;
    return RNS_OK;
}

static rns_status_t append_frame(rns_rnode_endpoint_t *endpoint,
                                 uint8_t command, const uint8_t *payload,
                                 size_t payload_length) {
    if ((payload == NULL && payload_length != 0U) ||
        endpoint->output_offset != 0U)
        return RNS_ERROR_INVALID_STATE;
    size_t needed = 3U;
    for (size_t i = 0U; i < payload_length; ++i)
        needed += payload[i] == RNS_KISS_FEND || payload[i] == RNS_KISS_FESC
                      ? 2U : 1U;
    if (needed > sizeof(endpoint->output) - endpoint->output_length)
        return RNS_ERROR_OVERFLOW;
    endpoint->output[endpoint->output_length++] = RNS_KISS_FEND;
    endpoint->output[endpoint->output_length++] = command;
    for (size_t i = 0U; i < payload_length; ++i) {
        uint8_t byte = payload[i];
        if (byte == RNS_KISS_FEND || byte == RNS_KISS_FESC) {
            endpoint->output[endpoint->output_length++] = RNS_KISS_FESC;
            endpoint->output[endpoint->output_length++] =
                byte == RNS_KISS_FEND ? RNS_KISS_TFEND : RNS_KISS_TFESC;
        } else endpoint->output[endpoint->output_length++] = byte;
    }
    endpoint->output[endpoint->output_length++] = RNS_KISS_FEND;
    return RNS_OK;
}

static rns_status_t flush_output(rns_rnode_endpoint_t *endpoint) {
    while (endpoint->output_offset < endpoint->output_length) {
        ssize_t written = write(endpoint->descriptor,
                                endpoint->output + endpoint->output_offset,
                                endpoint->output_length - endpoint->output_offset);
        if (written > 0) endpoint->output_offset += (size_t)written;
        else if (written < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
            return RNS_OK;
        else return RNS_ERROR_IO;
    }
    endpoint->output_offset = 0U;
    endpoint->output_length = 0U;
    return RNS_OK;
}

static rns_status_t queue_detect(rns_rnode_endpoint_t *endpoint) {
    const uint8_t request = RNODE_DETECT_REQUEST;
    const uint8_t ask = 0U;
    rns_status_t status = append_frame(endpoint, RNODE_CMD_DETECT, &request, 1U);
    if (status == RNS_OK)
        status = append_frame(endpoint, RNODE_CMD_FW_VERSION, &ask, 1U);
    if (status == RNS_OK)
        status = append_frame(endpoint, RNODE_CMD_PLATFORM, &ask, 1U);
    if (status == RNS_OK)
        status = append_frame(endpoint, RNODE_CMD_MCU, &ask, 1U);
    if (status == RNS_OK) endpoint->info.state = RNS_RNODE_DETECTING;
    return status;
}

static void store_u32(uint32_t value, uint8_t output[4]) {
    output[0] = (uint8_t)(value >> 24U);
    output[1] = (uint8_t)(value >> 16U);
    output[2] = (uint8_t)(value >> 8U);
    output[3] = (uint8_t)value;
}

static uint32_t load_u32(const uint8_t input[4]) {
    return ((uint32_t)input[0] << 24U) | ((uint32_t)input[1] << 16U) |
           ((uint32_t)input[2] << 8U) | (uint32_t)input[3];
}

static rns_status_t queue_configuration(rns_rnode_endpoint_t *endpoint) {
    uint8_t value[4];
    store_u32(endpoint->options.frequency, value);
    rns_status_t status = append_frame(endpoint, RNODE_CMD_FREQUENCY, value, 4U);
    store_u32(endpoint->options.bandwidth, value);
    if (status == RNS_OK)
        status = append_frame(endpoint, RNODE_CMD_BANDWIDTH, value, 4U);
    value[0] = endpoint->options.tx_power;
    if (status == RNS_OK)
        status = append_frame(endpoint, RNODE_CMD_TXPOWER, value, 1U);
    value[0] = endpoint->options.spreading_factor;
    if (status == RNS_OK) status = append_frame(endpoint, RNODE_CMD_SF, value, 1U);
    value[0] = endpoint->options.coding_rate;
    if (status == RNS_OK) status = append_frame(endpoint, RNODE_CMD_CR, value, 1U);
    if (endpoint->options.short_airtime_limit_set) {
        value[0] = (uint8_t)(endpoint->options.short_airtime_limit_hundredths >> 8U);
        value[1] = (uint8_t)endpoint->options.short_airtime_limit_hundredths;
        if (status == RNS_OK)
            status = append_frame(endpoint, RNODE_CMD_ST_ALOCK, value, 2U);
    }
    if (endpoint->options.long_airtime_limit_set) {
        value[0] = (uint8_t)(endpoint->options.long_airtime_limit_hundredths >> 8U);
        value[1] = (uint8_t)endpoint->options.long_airtime_limit_hundredths;
        if (status == RNS_OK)
            status = append_frame(endpoint, RNODE_CMD_LT_ALOCK, value, 2U);
    }
    value[0] = RNODE_RADIO_ON;
    if (status == RNS_OK)
        status = append_frame(endpoint, RNODE_CMD_RADIO_STATE, value, 1U);
    if (status == RNS_OK) {
        endpoint->info.state = RNS_RNODE_CONFIGURING;
        endpoint->deadline = 0.0;
    }
    return status;
}

static bool firmware_supported(const rns_rnode_endpoint_t *endpoint) {
    return endpoint->info.firmware_major > RNS_RNODE_FIRMWARE_MAJOR ||
           (endpoint->info.firmware_major == RNS_RNODE_FIRMWARE_MAJOR &&
            endpoint->info.firmware_minor >= RNS_RNODE_FIRMWARE_MINOR);
}

static bool configuration_matches(const rns_rnode_endpoint_t *endpoint) {
    uint32_t frequency_difference = endpoint->info.reported_frequency >
                                            endpoint->options.frequency
                                        ? endpoint->info.reported_frequency -
                                              endpoint->options.frequency
                                        : endpoint->options.frequency -
                                              endpoint->info.reported_frequency;
    return endpoint->bandwidth_seen && endpoint->tx_power_seen &&
           endpoint->sf_seen && endpoint->radio_seen &&
           (!endpoint->frequency_seen || frequency_difference <= 100U) &&
           endpoint->info.reported_bandwidth == endpoint->options.bandwidth &&
           endpoint->info.reported_tx_power == endpoint->options.tx_power &&
           endpoint->info.reported_spreading_factor ==
               endpoint->options.spreading_factor && endpoint->info.radio_on;
}

static rns_status_t queue_received(rns_rnode_endpoint_t *endpoint,
                                   const uint8_t *data, size_t length) {
    if (length == 0U) return RNS_OK;
    if (length > RNS_MTU || endpoint->receive_count == RNODE_QUEUE_DEPTH) {
        endpoint->info.packets_dropped++;
        return RNS_OK;
    }
    size_t tail = (endpoint->receive_head + endpoint->receive_count) %
                  RNODE_QUEUE_DEPTH;
    memcpy(endpoint->receive[tail].bytes, data, length);
    endpoint->receive[tail].length = length;
    endpoint->receive_count++;
    return RNS_OK;
}

static rns_status_t handle_command(rns_rnode_endpoint_t *endpoint,
                                   uint8_t command, const uint8_t *data,
                                   size_t length) {
    switch (command) {
        case RNODE_CMD_DATA: return queue_received(endpoint, data, length);
        case RNODE_CMD_DETECT:
            endpoint->detected = length == 1U && data[0] == RNODE_DETECT_RESPONSE;
            return RNS_OK;
        case RNODE_CMD_FW_VERSION:
            if (length == 2U) {
                endpoint->info.firmware_major = data[0];
                endpoint->info.firmware_minor = data[1];
                endpoint->firmware_seen = true;
            }
            return RNS_OK;
        case RNODE_CMD_PLATFORM:
            if (length == 1U) endpoint->info.platform = data[0];
            return RNS_OK;
        case RNODE_CMD_MCU:
            if (length == 1U) endpoint->info.mcu = data[0];
            return RNS_OK;
        case RNODE_CMD_FREQUENCY:
            if (length == 4U) {
                endpoint->info.reported_frequency = load_u32(data);
                endpoint->frequency_seen = true;
            }
            return RNS_OK;
        case RNODE_CMD_BANDWIDTH:
            if (length == 4U) {
                endpoint->info.reported_bandwidth = load_u32(data);
                endpoint->bandwidth_seen = true;
            }
            return RNS_OK;
        case RNODE_CMD_TXPOWER:
            if (length == 1U) {
                endpoint->info.reported_tx_power = data[0];
                endpoint->tx_power_seen = true;
            }
            return RNS_OK;
        case RNODE_CMD_SF:
            if (length == 1U) {
                endpoint->info.reported_spreading_factor = data[0];
                endpoint->sf_seen = true;
            }
            return RNS_OK;
        case RNODE_CMD_CR:
            if (length == 1U) {
                endpoint->info.reported_coding_rate = data[0];
                endpoint->cr_seen = true;
            }
            return RNS_OK;
        case RNODE_CMD_RADIO_STATE:
            if (length == 1U) {
                endpoint->info.radio_on = data[0] == RNODE_RADIO_ON;
                endpoint->radio_seen = true;
            }
            return RNS_OK;
        case RNODE_CMD_ST_ALOCK:
            if (length == 2U) {
                endpoint->reported_short_lock =
                    (uint16_t)((uint16_t)data[0] << 8U) | data[1];
                endpoint->short_lock_seen = true;
            }
            return RNS_OK;
        case RNODE_CMD_LT_ALOCK:
            if (length == 2U) {
                endpoint->reported_long_lock =
                    (uint16_t)((uint16_t)data[0] << 8U) | data[1];
                endpoint->long_lock_seen = true;
            }
            return RNS_OK;
        case RNODE_CMD_READY:
            endpoint->flow_ready = true;
            return RNS_OK;
        case RNODE_CMD_ERROR:
            if (length != 1U) return RNS_ERROR_PROTOCOL;
            endpoint->info.last_hardware_error = data[0];
            return data[0] == RNODE_ERROR_MEMORY_LOW ||
                           data[0] == RNODE_ERROR_MODEM_TIMEOUT
                       ? RNS_OK : RNS_ERROR_IO;
        default: return RNS_OK;
    }
}

static rns_status_t finish_frame(rns_rnode_endpoint_t *endpoint) {
    rnode_decoder_t *decoder = &endpoint->decoder;
    rns_status_t status = RNS_OK;
    if (decoder->escaped) {
        decoder->malformed++;
        decoder->discarding = true;
    }
    if (decoder->synchronized && decoder->have_command && !decoder->discarding)
        status = handle_command(endpoint, decoder->command, decoder->bytes,
                                decoder->length);
    decoder->length = 0U;
    decoder->synchronized = true;
    decoder->have_command = false;
    decoder->escaped = false;
    decoder->discarding = false;
    return status;
}

static rns_status_t feed_decoder(rns_rnode_endpoint_t *endpoint,
                                 const uint8_t *input, size_t input_length) {
    rnode_decoder_t *decoder = &endpoint->decoder;
    for (size_t i = 0U; i < input_length; ++i) {
        uint8_t byte = input[i];
        if (byte == RNS_KISS_FEND) {
            rns_status_t status = finish_frame(endpoint);
            if (status != RNS_OK) return status;
            continue;
        }
        if (!decoder->synchronized || decoder->discarding) continue;
        if (!decoder->have_command) {
            decoder->command = byte;
            decoder->have_command = true;
            continue;
        }
        if (decoder->escaped) {
            if (byte == RNS_KISS_TFEND) byte = RNS_KISS_FEND;
            else if (byte == RNS_KISS_TFESC) byte = RNS_KISS_FESC;
            else {
                decoder->malformed++;
                decoder->discarding = true;
                decoder->length = 0U;
                decoder->escaped = false;
                continue;
            }
            decoder->escaped = false;
        } else if (byte == RNS_KISS_FESC) {
            decoder->escaped = true;
            continue;
        }
        if (decoder->length == sizeof(decoder->bytes)) {
            decoder->oversized++;
            decoder->discarding = true;
            decoder->length = 0U;
            decoder->escaped = false;
            continue;
        }
        decoder->bytes[decoder->length++] = byte;
    }
    return RNS_OK;
}

static rns_status_t poll_read(rns_rnode_endpoint_t *endpoint) {
    uint8_t input[RNODE_INPUT_CAPACITY];
    ssize_t length = read(endpoint->descriptor, input, sizeof(input));
    if (length > 0) {
        endpoint->last_input_at = endpoint_clock(endpoint);
        return feed_decoder(endpoint, input, (size_t)length);
    }
    if (length == 0) {
        struct pollfd descriptor = {endpoint->descriptor, POLLIN, 0};
        if (poll(&descriptor, 1, 0) > 0 &&
            (descriptor.revents & (POLLHUP | POLLERR | POLLNVAL)) != 0)
            return RNS_ERROR_IO;
        return RNS_OK;
    }
    return errno == EAGAIN || errno == EWOULDBLOCK ? RNS_OK : RNS_ERROR_IO;
}

static rns_status_t dispatch_received(rns_rnode_endpoint_t *endpoint,
                                      size_t limit,
                                      rns_frame_callback_t callback,
                                      void *context, size_t *processed) {
    while (endpoint->receive_count != 0U && *processed < limit) {
        rnode_packet_t *packet = &endpoint->receive[endpoint->receive_head];
        rns_status_t status = callback(packet->bytes, packet->length, context);
        if (status != RNS_OK) return status;
        endpoint->info.packets_received++;
        endpoint->info.bytes_received += packet->length;
        endpoint->receive_head = (endpoint->receive_head + 1U) %
                                 RNODE_QUEUE_DEPTH;
        endpoint->receive_count--;
        (*processed)++;
    }
    return RNS_OK;
}

static rns_status_t poll_transmit(rns_rnode_endpoint_t *endpoint) {
    rns_status_t status = flush_output(endpoint);
    if (status != RNS_OK || endpoint->output_length != 0U) return status;
    if (endpoint->output_packet) {
        rnode_packet_t *packet = &endpoint->transmit[endpoint->transmit_head];
        endpoint->info.packets_sent++;
        endpoint->info.bytes_sent += packet->length;
        endpoint->transmit_head = (endpoint->transmit_head + 1U) %
                                  RNODE_QUEUE_DEPTH;
        endpoint->transmit_count--;
        endpoint->info.pending_packets = endpoint->transmit_count;
        endpoint->output_packet = false;
        if (endpoint->options.flow_control) endpoint->flow_ready = false;
        return RNS_OK;
    }
    if (endpoint->info.state != RNS_RNODE_UP || !endpoint->flow_ready ||
        endpoint->transmit_count == 0U)
        return RNS_OK;
    rnode_packet_t *packet = &endpoint->transmit[endpoint->transmit_head];
    status = append_frame(endpoint, RNODE_CMD_DATA, packet->bytes, packet->length);
    if (status != RNS_OK) return status;
    endpoint->output_packet = true;
    status = flush_output(endpoint);
    if (status != RNS_OK || endpoint->output_length != 0U) return status;
    endpoint->info.packets_sent++;
    endpoint->info.bytes_sent += packet->length;
    endpoint->transmit_head = (endpoint->transmit_head + 1U) % RNODE_QUEUE_DEPTH;
    endpoint->transmit_count--;
    endpoint->info.pending_packets = endpoint->transmit_count;
    endpoint->output_packet = false;
    if (endpoint->options.flow_control) endpoint->flow_ready = false;
    return RNS_OK;
}

rns_status_t rns_rnode_endpoint_send(rns_rnode_endpoint_t *endpoint,
                                     const uint8_t *packet,
                                     size_t packet_length) {
    if (endpoint == NULL || packet == NULL || packet_length == 0U ||
        packet_length > RNS_MTU)
        return RNS_ERROR_INVALID_ARGUMENT;
    if (endpoint->info.state == RNS_RNODE_DOWN || endpoint->leave_sent)
        return RNS_ERROR_INVALID_STATE;
    if (endpoint->transmit_count == RNODE_QUEUE_DEPTH) {
        endpoint->info.packets_dropped++;
        return RNS_ERROR_OVERFLOW;
    }
    size_t tail = (endpoint->transmit_head + endpoint->transmit_count) %
                  RNODE_QUEUE_DEPTH;
    memcpy(endpoint->transmit[tail].bytes, packet, packet_length);
    endpoint->transmit[tail].length = packet_length;
    endpoint->transmit_count++;
    endpoint->info.pending_packets = endpoint->transmit_count;
    return RNS_OK;
}

static rns_status_t advance_state(rns_rnode_endpoint_t *endpoint, double now) {
    if (endpoint->info.state == RNS_RNODE_STARTING && now >= endpoint->state_at)
        return queue_detect(endpoint);
    if (endpoint->info.state == RNS_RNODE_DETECTING) {
        if (endpoint->output_length == 0U && endpoint->deadline == 0.0)
            endpoint->deadline = now + endpoint->options.detect_timeout_seconds;
        if (endpoint->detected && endpoint->firmware_seen) {
            if (!firmware_supported(endpoint)) return RNS_ERROR_UNSUPPORTED;
            return queue_configuration(endpoint);
        }
        if (endpoint->deadline > 0.0 && now >= endpoint->deadline)
            return RNS_ERROR_TIMEOUT;
    } else if (endpoint->info.state == RNS_RNODE_CONFIGURING) {
        if (endpoint->output_length == 0U && endpoint->deadline == 0.0)
            endpoint->deadline = now + endpoint->options.validation_timeout_seconds;
        if (configuration_matches(endpoint)) {
            endpoint->info.state = RNS_RNODE_UP;
            endpoint->info.last_error = RNS_OK;
            endpoint->flow_ready = true;
            endpoint->deadline = 0.0;
        } else if (endpoint->deadline > 0.0 && now >= endpoint->deadline)
            return RNS_ERROR_PROTOCOL;
    }
    return RNS_OK;
}

rns_status_t rns_rnode_endpoint_poll(rns_rnode_endpoint_t *endpoint,
                                     size_t max_packets,
                                     rns_frame_callback_t callback,
                                     void *context, size_t *processed) {
    if (endpoint == NULL || callback == NULL || processed == NULL)
        return RNS_ERROR_INVALID_ARGUMENT;
    *processed = 0U;
    if (endpoint->detached) return RNS_OK;
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
    if (endpoint->decoder.have_command &&
        now - endpoint->last_input_at >= RNODE_FRAME_TIMEOUT) {
        endpoint->decoder.malformed++;
        reset_decoder(&endpoint->decoder);
    }
    rns_status_t status = poll_read(endpoint);
    if (status == RNS_OK) status = advance_state(endpoint, now);
    if (status == RNS_OK) status = poll_transmit(endpoint);
    if (status == RNS_OK) status = advance_state(endpoint, now);
    size_t limit = max_packets == 0U ? RNODE_QUEUE_DEPTH : max_packets;
    if (status == RNS_OK)
        status = dispatch_received(endpoint, limit, callback, context, processed);
    endpoint->info.malformed_frames = endpoint->decoder.malformed;
    endpoint->info.oversized_frames = endpoint->decoder.oversized;
    if (status != RNS_OK) mark_down(endpoint, status);
    return status;
}

rns_status_t rns_rnode_endpoint_leave(rns_rnode_endpoint_t *endpoint) {
    if (endpoint == NULL) return RNS_ERROR_INVALID_ARGUMENT;
    if (endpoint->detached) return RNS_OK;
    if (endpoint->leave_sent) {
        rns_status_t status = flush_output(endpoint);
        if (status != RNS_OK) return status;
        if (endpoint->output_length != 0U) return RNS_ERROR_TIMEOUT;
        (void)close(endpoint->descriptor);
        endpoint->descriptor = -1;
        endpoint->detached = true;
        endpoint->info.state = RNS_RNODE_DOWN;
        endpoint->info.last_error = RNS_OK;
        return RNS_OK;
    }
    if (endpoint->descriptor < 0 || endpoint->output_length != 0U)
        return RNS_ERROR_INVALID_STATE;
    uint8_t value = RNODE_RADIO_OFF;
    rns_status_t status = append_frame(endpoint, RNODE_CMD_RADIO_STATE,
                                       &value, 1U);
    value = 0xffU;
    if (status == RNS_OK)
        status = append_frame(endpoint, RNODE_CMD_LEAVE, &value, 1U);
    if (status == RNS_OK) {
        endpoint->leave_sent = true;
        endpoint->flow_ready = false;
        endpoint->info.radio_on = false;
        status = flush_output(endpoint);
    }
    if (status != RNS_OK || endpoint->output_length != 0U)
        return status != RNS_OK ? status : RNS_ERROR_TIMEOUT;
    (void)close(endpoint->descriptor);
    endpoint->descriptor = -1;
    endpoint->detached = true;
    endpoint->info.state = RNS_RNODE_DOWN;
    endpoint->info.last_error = RNS_OK;
    return RNS_OK;
}

void rns_rnode_endpoint_destroy(rns_rnode_endpoint_t *endpoint) {
    if (endpoint == NULL) return;
    if (endpoint->descriptor >= 0) {
        if (endpoint->output_length == 0U && !endpoint->leave_sent)
            (void)rns_rnode_endpoint_leave(endpoint);
        (void)close(endpoint->descriptor);
    }
    free(endpoint);
}

rns_status_t rns_rnode_endpoint_info(const rns_rnode_endpoint_t *endpoint,
                                     rns_rnode_info_t *info) {
    if (endpoint == NULL || info == NULL) return RNS_ERROR_INVALID_ARGUMENT;
    *info = endpoint->info;
    return RNS_OK;
}
