#define _DARWIN_C_SOURCE
#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "reticulum/framing.h"
#include "reticulum/packet.h"
#include "reticulum/runtime.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <util.h>
#else
#include <pty.h>
#endif

static double now_value;

static double test_clock(void *context) {
    (void)context;
    return now_value;
}

int main(void) {
    int master = -1;
    int slave = -1;
    char device[128];
    assert(openpty(&master, &slave, device, NULL, NULL) == 0);
    assert(close(slave) == 0);
    int flags = fcntl(master, F_GETFL, 0);
    assert(flags >= 0 && fcntl(master, F_SETFL, flags | O_NONBLOCK) == 0);

    rns_config_t config;
    rns_config_init(&config);
    config.share_instance = false;
    config.share_instance_configured = true;
    config.interface_count = 1U;
    rns_config_interface_t *item = &config.interfaces[0];
    strcpy(item->name, "KISS test");
    item->type = RNS_CONFIG_KISS;
    item->type_set = true;
    item->enabled = true;
    strcpy(item->device, device);
    item->speed = 9600U;
    item->data_bits = 8U;
    item->parity = 'N';
    item->stop_bits = 1U;

    rns_runtime_options_t options = {0};
    options.reconnect_clock = test_clock;
    rns_runtime_t *runtime = NULL;
    assert(rns_runtime_create(&runtime, &config, &options) == RNS_OK);
    rns_runtime_interface_info_t info;
    assert(rns_runtime_interface_info(runtime, 0U, &info) == RNS_OK);
    assert(info.state == RNS_RUNTIME_INTERFACE_STARTING);
    now_value = 2.0;
    size_t processed = 0U;
    assert(rns_runtime_poll(runtime, 8U, &processed) == RNS_OK);
    assert(rns_runtime_interface_info(runtime, 0U, &info) == RNS_OK);
    assert(info.state == RNS_RUNTIME_INTERFACE_UP);

    uint8_t startup[128];
    assert(read(master, startup, sizeof(startup)) > 0);
    static const uint8_t payload[] = {1U, 2U};
    rns_packet packet = {0};
    memset(packet.destination_hash, 0x44, sizeof(packet.destination_hash));
    packet.data = payload;
    packet.data_length = sizeof(payload);
    uint8_t raw[RNS_MTU];
    size_t raw_length = 0U;
    assert(rns_packet_encode(&packet, raw, sizeof(raw), &raw_length));
    uint8_t encoded[2U * RNS_MTU + 3U];
    size_t encoded_length = 0U;
    assert(rns_kiss_encode(0U, raw, raw_length, encoded, sizeof(encoded),
                           &encoded_length) == RNS_OK);
    assert(write(master, encoded, encoded_length) == (ssize_t)encoded_length);
    assert(rns_runtime_poll(runtime, 8U, &processed) == RNS_OK);
    assert(processed == 1U);
    assert(rns_runtime_interface_info(runtime, 0U, &info) == RNS_OK);
    assert(info.packets_received == 1U && info.bytes_received == raw_length);

    assert(rns_runtime_send(runtime, 0U, raw, raw_length) == RNS_OK);
    assert(rns_runtime_poll(runtime, 8U, &processed) == RNS_OK);
    uint8_t outgoing[2U * RNS_MTU + 3U];
    ssize_t count = read(master, outgoing, sizeof(outgoing));
    assert(count > 0 && outgoing[0] == RNS_KISS_FEND);
    assert(rns_runtime_interface_info(runtime, 0U, &info) == RNS_OK);
    assert(info.packets_sent == 1U && info.bytes_sent == raw_length);

    rns_runtime_destroy(runtime);
    assert(close(master) == 0);
    return 0;
}
