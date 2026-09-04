#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "reticulum/packet.h"
#include "reticulum/rnode.h"
#include "reticulum/runtime.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <util.h>
#else
#include <pty.h>
#endif

#define CMD_DATA 0x00U
#define CMD_FREQUENCY 0x01U
#define CMD_BANDWIDTH 0x02U
#define CMD_TXPOWER 0x03U
#define CMD_SF 0x04U
#define CMD_CR 0x05U
#define CMD_RADIO_STATE 0x06U
#define CMD_DETECT 0x08U
#define CMD_ST_ALOCK 0x0bU
#define CMD_LT_ALOCK 0x0cU
#define CMD_READY 0x0fU
#define CMD_PLATFORM 0x48U
#define CMD_MCU 0x49U
#define CMD_FW_VERSION 0x50U

typedef struct peer {
    bool detected;
    bool frequency;
    bool bandwidth;
    bool tx_power;
    bool sf;
    bool cr;
    bool radio;
    bool short_lock;
    bool long_lock;
    bool suppress_frequency_echo;
    bool mismatch_bandwidth_echo;
    bool mismatch_cr_echo;
    bool suppress_short_lock_echo;
    bool mismatch_long_lock_echo;
    uint8_t data[RNS_MTU];
    size_t data_length;
    size_t data_count;
    bool in_frame;
    bool escaped;
    bool have_command;
    uint8_t command;
    uint8_t payload[508U];
    size_t payload_length;
} peer_t;

typedef struct capture {
    uint8_t data[RNS_MTU];
    size_t length;
    size_t count;
} capture_t;

static double fake_now;

static double fake_clock(void *context) {
    (void)context;
    return fake_now;
}

static int new_pty(char path[128]) {
    int master = -1;
    int slave = -1;
    char name[128];
    assert(openpty(&master, &slave, name, NULL, NULL) == 0);
    size_t length = strnlen(name, sizeof(name));
    assert(length > 0U && length < 128U);
    memcpy(path, name, length + 1U);
    int flags = fcntl(master, F_GETFL, 0);
    assert(flags >= 0 && fcntl(master, F_SETFL, flags | O_NONBLOCK) == 0);
    assert(close(slave) == 0);
    return master;
}

static size_t encode_frame(uint8_t command, const uint8_t *payload,
                           size_t payload_length, uint8_t *output,
                           size_t capacity) {
    size_t position = 0U;
    assert(capacity >= 3U);
    output[position++] = RNS_KISS_FEND;
    output[position++] = command;
    for (size_t i = 0U; i < payload_length; ++i) {
        if (payload[i] == RNS_KISS_FEND || payload[i] == RNS_KISS_FESC) {
            assert(capacity - position >= 2U);
            output[position++] = RNS_KISS_FESC;
            output[position++] = payload[i] == RNS_KISS_FEND
                                     ? RNS_KISS_TFEND : RNS_KISS_TFESC;
        } else {
            assert(position < capacity);
            output[position++] = payload[i];
        }
    }
    assert(position < capacity);
    output[position++] = RNS_KISS_FEND;
    return position;
}

static void write_frame(int master, uint8_t command, const uint8_t *payload,
                        size_t payload_length) {
    uint8_t encoded[2U * RNS_MTU + 3U];
    size_t length = encode_frame(command, payload, payload_length, encoded,
                                 sizeof(encoded));
    assert(write(master, encoded, length) == (ssize_t)length);
}

static void peer_frame(int master, peer_t *peer, uint8_t command,
                       const uint8_t *payload, size_t payload_length) {
    static const uint8_t detect_response = 0x46U;
    static const uint8_t firmware[] = {1U, 52U};
    static const uint8_t platform = 0x80U;
    static const uint8_t mcu = 0x01U;
    switch (command) {
        case CMD_DETECT:
            assert(payload_length == 1U && payload[0] == 0x73U);
            peer->detected = true;
            write_frame(master, command, &detect_response, 1U);
            break;
        case CMD_FW_VERSION: write_frame(master, command, firmware, 2U); break;
        case CMD_PLATFORM: write_frame(master, command, &platform, 1U); break;
        case CMD_MCU: write_frame(master, command, &mcu, 1U); break;
        case CMD_FREQUENCY:
            peer->frequency = true;
            if (!peer->suppress_frequency_echo)
                write_frame(master, command, payload, payload_length);
            break;
        case CMD_BANDWIDTH:
            peer->bandwidth = true;
            if (peer->mismatch_bandwidth_echo) {
                uint8_t response[4];
                assert(payload_length == sizeof(response));
                memcpy(response, payload, sizeof(response));
                response[sizeof(response) - 1U] ^= 1U;
                write_frame(master, command, response, sizeof(response));
            } else {
                write_frame(master, command, payload, payload_length);
            }
            break;
        case CMD_TXPOWER:
            peer->tx_power = true;
            write_frame(master, command, payload, payload_length);
            break;
        case CMD_SF:
            peer->sf = true;
            write_frame(master, command, payload, payload_length);
            break;
        case CMD_CR:
            peer->cr = true;
            if (peer->mismatch_cr_echo) {
                uint8_t response;
                assert(payload_length == 1U);
                response = (uint8_t)(payload[0] + 1U);
                write_frame(master, command, &response, 1U);
            } else {
                write_frame(master, command, payload, payload_length);
            }
            break;
        case CMD_RADIO_STATE:
            peer->radio = true;
            write_frame(master, command, payload, payload_length);
            break;
        case CMD_ST_ALOCK:
            peer->short_lock = true;
            if (!peer->suppress_short_lock_echo)
                write_frame(master, command, payload, payload_length);
            break;
        case CMD_LT_ALOCK:
            peer->long_lock = true;
            if (peer->mismatch_long_lock_echo) {
                uint8_t response[2];
                assert(payload_length == sizeof(response));
                memcpy(response, payload, sizeof(response));
                response[sizeof(response) - 1U] ^= 1U;
                write_frame(master, command, response, sizeof(response));
            } else {
                write_frame(master, command, payload, payload_length);
            }
            break;
        case CMD_DATA:
            assert(payload_length <= sizeof(peer->data));
            memcpy(peer->data, payload, payload_length);
            peer->data_length = payload_length;
            peer->data_count++;
            break;
        default: break;
    }
}

static size_t peer_process(int master, peer_t *peer) {
    uint8_t input[4096];
    ssize_t count = read(master, input, sizeof(input));
    if (count < 0) {
        assert(errno == EAGAIN || errno == EWOULDBLOCK);
        return 0U;
    }
    for (size_t i = 0U; i < (size_t)count; ++i) {
        uint8_t byte = input[i];
        if (byte == RNS_KISS_FEND) {
            if (peer->in_frame && peer->have_command)
                peer_frame(master, peer, peer->command, peer->payload,
                           peer->payload_length);
            peer->in_frame = true;
            peer->escaped = false;
            peer->have_command = false;
            peer->payload_length = 0U;
        } else if (peer->in_frame && !peer->have_command) {
            peer->command = byte;
            peer->have_command = true;
        } else if (peer->in_frame) {
            if (peer->escaped) {
                if (byte == RNS_KISS_TFEND) byte = RNS_KISS_FEND;
                else if (byte == RNS_KISS_TFESC) byte = RNS_KISS_FESC;
                else assert(false);
                peer->escaped = false;
            } else if (byte == RNS_KISS_FESC) {
                peer->escaped = true;
                continue;
            }
            assert(peer->payload_length < sizeof(peer->payload));
            peer->payload[peer->payload_length++] = byte;
        }
    }
    return (size_t)count;
}

static size_t drain_bytes(int descriptor, uint8_t *buffer, size_t capacity) {
    size_t total = 0U;
    while (total < capacity) {
        ssize_t count = read(descriptor, buffer + total, capacity - total);
        if (count > 0) total += (size_t)count;
        else if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
        else break;
    }
    return total;
}

static rns_status_t capture_frame(const uint8_t *frame, size_t length,
                                  void *context) {
    capture_t *capture = context;
    assert(length <= sizeof(capture->data));
    memcpy(capture->data, frame, length);
    capture->length = length;
    capture->count++;
    return RNS_OK;
}

static void endpoint_poll_ok(rns_rnode_endpoint_t *endpoint,
                             capture_t *capture) {
    size_t processed = 0U;
    assert(rns_rnode_endpoint_poll(endpoint, 8U, capture_frame, capture,
                                   &processed) == RNS_OK);
}

static void complete_endpoint_handshake(rns_rnode_endpoint_t *endpoint,
                                        int master, peer_t *peer,
                                        capture_t *capture) {
    rns_rnode_info_t info;
    memset(&info, 0, sizeof(info));
    for (size_t attempt = 0U;
         attempt < 16U && info.state != RNS_RNODE_UP;
         ++attempt) {
        endpoint_poll_ok(endpoint, capture);
        (void)peer_process(master, peer);
        assert(rns_rnode_endpoint_info(endpoint, &info) == RNS_OK);
    }
    assert(rns_rnode_endpoint_info(endpoint, &info) == RNS_OK);
    assert(info.state == RNS_RNODE_UP && info.firmware_major == 1U &&
           info.firmware_minor == 52U && info.platform == 0x80U);
    assert(peer->detected && peer->frequency && peer->bandwidth &&
           peer->tx_power && peer->sf && peer->cr && peer->radio &&
           peer->short_lock && peer->long_lock);
}

static void test_endpoint(void) {
    char first_path[128];
    char second_path[128];
    char directory[] = "/tmp/rns-rnode-XXXXXX";
    char link_path[256];
    int first_master = new_pty(first_path);
    assert(mkdtemp(directory) != NULL);
    int path_length = snprintf(link_path, sizeof(link_path), "%s/device", directory);
    assert(path_length > 0 && (size_t)path_length < sizeof(link_path));
    assert(symlink(first_path, link_path) == 0);

    rns_rnode_options_t options;
    rns_rnode_options_init(&options);
    options.device = link_path;
    options.frequency = 915000000U;
    options.bandwidth = 125000U;
    options.tx_power = 17U;
    options.spreading_factor = 8U;
    options.coding_rate = 5U;
    options.flow_control = true;
    options.short_airtime_limit_set = true;
    options.short_airtime_limit_hundredths = 2500U;
    options.long_airtime_limit_set = true;
    options.long_airtime_limit_hundredths = 5000U;
    options.startup_delay_seconds = 0.0;
    options.clock = fake_clock;
    rns_rnode_endpoint_t *endpoint = NULL;
    assert(rns_rnode_endpoint_create(&endpoint, &options) == RNS_OK);
    peer_t peer = {0};
    capture_t capture = {0};
    complete_endpoint_handshake(endpoint, first_master, &peer, &capture);

    static const uint8_t first[] = {1U, RNS_KISS_FEND, RNS_KISS_FESC};
    static const uint8_t second[] = {2U};
    assert(rns_rnode_endpoint_send(endpoint, first, sizeof(first)) == RNS_OK);
    assert(rns_rnode_endpoint_send(endpoint, second, sizeof(second)) == RNS_OK);
    endpoint_poll_ok(endpoint, &capture);
    peer_process(first_master, &peer);
    assert(peer.data_count == 1U && peer.data_length == sizeof(first) &&
           memcmp(peer.data, first, sizeof(first)) == 0);
    endpoint_poll_ok(endpoint, &capture);
    peer_process(first_master, &peer);
    assert(peer.data_count == 1U);
    static const uint8_t ready = 1U;
    write_frame(first_master, CMD_READY, &ready, 1U);
    endpoint_poll_ok(endpoint, &capture);
    peer_process(first_master, &peer);
    assert(peer.data_count == 2U && peer.data[0] == 2U);

    /* Force serial write backpressure and ensure the queued packet is not
     * duplicated or discarded across partial non-blocking progress. */
    write_frame(first_master, CMD_READY, &ready, 1U);
    endpoint_poll_ok(endpoint, &capture);
    int filler = open(first_path, O_WRONLY | O_NONBLOCK | O_NOCTTY);
    assert(filler >= 0);
    uint8_t fill[4096];
    memset(fill, RNS_KISS_FEND, sizeof(fill));
    while (write(filler, fill, sizeof(fill)) > 0) {}
    assert(errno == EAGAIN || errno == EWOULDBLOCK);
    assert(close(filler) == 0);
    uint8_t large[RNS_MTU];
    memset(large, 0x55, sizeof(large));
    assert(rns_rnode_endpoint_send(endpoint, large, sizeof(large)) == RNS_OK);
    endpoint_poll_ok(endpoint, &capture);
    rns_rnode_info_t info;
    assert(rns_rnode_endpoint_info(endpoint, &info) == RNS_OK);
    assert(info.pending_packets == 1U);
    while (peer_process(first_master, &peer) != 0U) {}
    for (size_t attempt = 0U; attempt < 8U && info.pending_packets != 0U;
         ++attempt) {
        endpoint_poll_ok(endpoint, &capture);
        assert(rns_rnode_endpoint_info(endpoint, &info) == RNS_OK);
    }
    assert(info.pending_packets == 0U && info.packets_sent == 3U);
    while (peer_process(first_master, &peer) != 0U) {}
    assert(peer.data_count == 3U && peer.data_length == sizeof(large));

    static const uint8_t incoming[] = {0x44U, RNS_KISS_FEND};
    uint8_t encoded[16];
    size_t encoded_length = encode_frame(CMD_DATA, incoming, sizeof(incoming),
                                         encoded, sizeof(encoded));
    assert(write(first_master, encoded, 3U) == 3);
    endpoint_poll_ok(endpoint, &capture);
    assert(capture.count == 0U);
    assert(write(first_master, encoded + 3U, encoded_length - 3U) ==
           (ssize_t)(encoded_length - 3U));
    endpoint_poll_ok(endpoint, &capture);
    assert(capture.count == 1U && capture.length == sizeof(incoming) &&
           memcmp(capture.data, incoming, sizeof(incoming)) == 0);
    static const uint8_t malformed[] = {
        RNS_KISS_FEND, CMD_DATA, RNS_KISS_FESC, 0x01U, RNS_KISS_FEND
    };
    assert(write(first_master, malformed, sizeof(malformed)) ==
           (ssize_t)sizeof(malformed));
    endpoint_poll_ok(endpoint, &capture);
    assert(rns_rnode_endpoint_info(endpoint, &info) == RNS_OK);
    assert(info.malformed_frames == 1U);

    assert(close(first_master) == 0);
    size_t processed = 0U;
    assert(rns_rnode_endpoint_poll(endpoint, 8U, capture_frame, &capture,
                                   &processed) == RNS_ERROR_IO);
    int second_master = new_pty(second_path);
    assert(unlink(link_path) == 0);
    assert(symlink(second_path, link_path) == 0);
    fake_now += 5.0;
    endpoint_poll_ok(endpoint, &capture);
    fake_now += 0.01;
    memset(&peer, 0, sizeof(peer));
    complete_endpoint_handshake(endpoint, second_master, &peer, &capture);
    assert(rns_rnode_endpoint_info(endpoint, &info) == RNS_OK);
    assert(info.connections_established == 2U && info.connections_lost == 1U);

    assert(rns_rnode_endpoint_leave(endpoint) == RNS_OK);
    peer_process(second_master, &peer);
    rns_rnode_endpoint_destroy(endpoint);
    assert(close(second_master) == 0);
    assert(unlink(link_path) == 0);
    assert(rmdir(directory) == 0);
}

static void test_old_firmware_rejected(void) {
    char path[128];
    int master = new_pty(path);
    rns_rnode_options_t options;
    rns_rnode_options_init(&options);
    options.device = path;
    options.frequency = 915000000U;
    options.bandwidth = 125000U;
    options.tx_power = 17U;
    options.spreading_factor = 8U;
    options.coding_rate = 5U;
    options.startup_delay_seconds = 0.0;
    options.clock = fake_clock;
    rns_rnode_endpoint_t *endpoint = NULL;
    assert(rns_rnode_endpoint_create(&endpoint, &options) == RNS_OK);
    capture_t capture = {0};
    endpoint_poll_ok(endpoint, &capture);
    uint8_t discarded[128];
    assert(drain_bytes(master, discarded, sizeof(discarded)) > 0U);
    static const uint8_t detect = 0x46U;
    static const uint8_t firmware[] = {1U, 51U};
    write_frame(master, CMD_DETECT, &detect, 1U);
    write_frame(master, CMD_FW_VERSION, firmware, sizeof(firmware));
    size_t processed = 0U;
    assert(rns_rnode_endpoint_poll(endpoint, 8U, capture_frame, &capture,
                                   &processed) == RNS_ERROR_UNSUPPORTED);
    rns_rnode_info_t info;
    assert(rns_rnode_endpoint_info(endpoint, &info) == RNS_OK);
    assert(info.state == RNS_RNODE_DOWN &&
           info.last_error == RNS_ERROR_UNSUPPORTED);
    rns_rnode_endpoint_destroy(endpoint);
    assert(close(master) == 0);
}

static void test_pinned_configuration_validation(void) {
    char path[128];
    int master = new_pty(path);
    rns_rnode_options_t options;
    rns_rnode_options_init(&options);
    options.device = path;
    options.frequency = 915000000U;
    options.bandwidth = 125000U;
    options.tx_power = 17U;
    options.spreading_factor = 8U;
    options.coding_rate = 5U;
    options.short_airtime_limit_set = true;
    options.short_airtime_limit_hundredths = 2500U;
    options.long_airtime_limit_set = true;
    options.long_airtime_limit_hundredths = 5000U;
    options.startup_delay_seconds = 0.0;
    options.clock = fake_clock;
    rns_rnode_endpoint_t *endpoint = NULL;
    assert(rns_rnode_endpoint_create(&endpoint, &options) == RNS_OK);
    peer_t peer = {
        .suppress_frequency_echo = true,
        .mismatch_cr_echo = true,
        .suppress_short_lock_echo = true,
        .mismatch_long_lock_echo = true,
    };
    capture_t capture = {0};

    /* RNS 1.5.2 intentionally accepts these reports: frequency is checked
     * only when present, while CR and airtime-lock reports are not startup
     * gates. Preserve that surprising behavior for device compatibility. */
    complete_endpoint_handshake(endpoint, master, &peer, &capture);
    rns_rnode_info_t info;
    assert(rns_rnode_endpoint_info(endpoint, &info) == RNS_OK);
    assert(info.reported_frequency == 0U);
    assert(info.reported_coding_rate == options.coding_rate + 1U);
    rns_rnode_endpoint_destroy(endpoint);
    assert(close(master) == 0);

    master = new_pty(path);
    assert(rns_rnode_endpoint_create(&endpoint, &options) == RNS_OK);
    memset(&peer, 0, sizeof(peer));
    peer.mismatch_bandwidth_echo = true;
    memset(&capture, 0, sizeof(capture));
    endpoint_poll_ok(endpoint, &capture);
    peer_process(master, &peer);
    endpoint_poll_ok(endpoint, &capture);
    peer_process(master, &peer);
    endpoint_poll_ok(endpoint, &capture);
    assert(rns_rnode_endpoint_info(endpoint, &info) == RNS_OK);
    assert(info.state == RNS_RNODE_CONFIGURING);
    fake_now += options.validation_timeout_seconds + 0.01;
    size_t processed = 0U;
    assert(rns_rnode_endpoint_poll(endpoint, 8U, capture_frame, &capture,
                                   &processed) == RNS_ERROR_PROTOCOL);
    assert(rns_rnode_endpoint_info(endpoint, &info) == RNS_OK);
    assert(info.state == RNS_RNODE_DOWN &&
           info.last_error == RNS_ERROR_PROTOCOL);
    rns_rnode_endpoint_destroy(endpoint);
    assert(close(master) == 0);
}

static void test_runtime(void) {
    char path[128];
    int master = new_pty(path);
    rns_config_t config;
    rns_config_init(&config);
    config.share_instance = false;
    config.share_instance_configured = true;
    config.interface_count = 1U;
    rns_config_interface_t *item = &config.interfaces[0];
    strcpy(item->name, "RNode test");
    item->type = RNS_CONFIG_RNODE;
    item->type_set = true;
    item->enabled = true;
    strcpy(item->device, path);
    item->speed = 115200U;
    item->frequency = 915000000U;
    item->bandwidth = 125000U;
    item->tx_power = 17;
    item->spreading_factor = 8U;
    item->coding_rate = 5U;

    rns_runtime_options_t runtime_options = {0};
    runtime_options.reconnect_clock = fake_clock;
    rns_runtime_t *runtime = NULL;
    assert(rns_runtime_create(&runtime, &config, &runtime_options) == RNS_OK);
    fake_now += 2.0;
    size_t processed = 0U;
    peer_t peer = {0};
    rns_runtime_interface_info_t info;
    memset(&info, 0, sizeof(info));
    for (size_t attempt = 0U;
         attempt < 16U && info.state != RNS_RUNTIME_INTERFACE_UP;
         ++attempt) {
        assert(rns_runtime_poll(runtime, 8U, &processed) == RNS_OK);
        (void)peer_process(master, &peer);
        assert(rns_runtime_interface_info(runtime, 0U, &info) == RNS_OK);
    }
    assert(rns_runtime_interface_info(runtime, 0U, &info) == RNS_OK);
    assert(info.state == RNS_RUNTIME_INTERFACE_UP);

    static const uint8_t payload[] = {9U};
    rns_packet packet = {0};
    memset(packet.destination_hash, 0x31, sizeof(packet.destination_hash));
    packet.data = payload;
    packet.data_length = sizeof(payload);
    uint8_t raw[RNS_MTU];
    size_t raw_length = 0U;
    assert(rns_packet_encode(&packet, raw, sizeof(raw), &raw_length));
    assert(rns_runtime_send(runtime, 0U, raw, raw_length) == RNS_OK);
    assert(rns_runtime_poll(runtime, 8U, &processed) == RNS_OK);
    peer_process(master, &peer);
    assert(peer.data_count == 1U && peer.data_length == raw_length &&
           memcmp(peer.data, raw, raw_length) == 0);
    rns_runtime_destroy(runtime);
    assert(close(master) == 0);
}

int main(void) {
    test_endpoint();
    test_old_firmware_rejected();
    test_pinned_configuration_validation();
    test_runtime();
    return 0;
}
