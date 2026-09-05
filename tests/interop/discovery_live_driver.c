#include "reticulum/runtime.h"
#include "reticulum/destination.h"
#include "reticulum/hal.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void hex(const uint8_t *bytes, size_t count) {
    for (size_t i = 0; i < count; ++i) printf("%02x", bytes[i]);
}
static void announce(rns_runtime_t *runtime, const rns_node_result *event, void *context) {
    (void)runtime; (void)context;
    uint8_t public_key[64];
    rns_identity_export_public(&event->announce_identity, public_key);
    printf("{\"event\":\"announce\",\"destination\":\""); hex(event->destination_hash, 16u);
    printf("\",\"public\":\""); hex(public_key, sizeof public_key); puts("\"}");
}
static bool port(const char *text, uint16_t *value) {
    char *end; errno = 0; unsigned long parsed = strtoul(text, &end, 10);
    if (errno != 0 || *end != 0 || parsed == 0 || parsed > 65535u) return false;
    *value = (uint16_t)parsed; return true;
}
static int digit(char byte) {
    if (byte >= '0' && byte <= '9') return byte - '0';
    if (byte >= 'a' && byte <= 'f') return byte - 'a' + 10;
    return -1;
}
int main(int argc, char **argv) {
    uint16_t local = 0, remote = 0;
    bool tcp = argc == 3 && strcmp(argv[1], "--tcp") == 0;
    if (argc != 3 || !port(argv[2], &remote) || (!tcp && !port(argv[1], &local))) return 2;
    setvbuf(stdout, NULL, _IOLBF, 0);
    int flags = fcntl(STDIN_FILENO, F_GETFL);
    if (flags < 0 || fcntl(STDIN_FILENO, F_SETFL, flags | O_NONBLOCK) < 0) return 3;
    rns_config_t config; rns_config_init(&config); config.interface_count = 1u;
    rns_config_interface_t *interface = &config.interfaces[0];
    strcpy(interface->name, "synthetic discovery");
    interface->type = tcp ? RNS_CONFIG_TCP_CLIENT : RNS_CONFIG_UDP;
    interface->type_set = interface->enabled = true;
    if (tcp) { strcpy(interface->target_host, "127.0.0.1"); interface->target_port = remote; }
    else {
        strcpy(interface->listen_ip, "127.0.0.1"); strcpy(interface->forward_ip, "127.0.0.1");
        interface->listen_port = local; interface->forward_port = remote;
    }
    rns_runtime_t *runtime = NULL; rns_identity identity;
    rns_runtime_options_t options = {.announce_callback = announce};
    if (!rns_identity_generate(&identity) || rns_runtime_create(&runtime, &config, &options) != RNS_OK) return 3;
    uint8_t destination[16]; const char *aspects[] = {"delivery"};
    int result = 1;
    if (!rns_destination_hash(&identity, "lxmf", aspects, 1u, destination) ||
        rns_runtime_register_destination(runtime, destination) != RNS_OK) goto done;
    uint64_t start, now;
    if (rns_hal_monotonic_ms(&start) != RNS_OK) goto done;
    bool ready = false; char command[34]; size_t used = 0;
    while (rns_hal_monotonic_ms(&now) == RNS_OK && now - start < 60000u) {
        size_t processed;
        if (rns_runtime_poll(runtime, 16u, &processed) != RNS_OK) goto done;
        rns_runtime_interface_info_t info;
        if (!ready && rns_runtime_interface_info(runtime, 0u, &info) == RNS_OK && info.state == RNS_RUNTIME_INTERFACE_UP) {
            if (rns_runtime_announce(runtime, &identity, "lxmf", aspects, 1u, NULL, 0u) != RNS_OK) goto done;
            uint8_t public_key[64]; rns_identity_export_public(&identity, public_key);
            printf("{\"event\":\"ready\",\"destination\":\""); hex(destination, 16u);
            printf("\",\"public\":\""); hex(public_key, sizeof public_key); puts("\"}"); ready = true;
        }
        char byte; ssize_t count = read(STDIN_FILENO, &byte, 1u);
        if (count == 1) {
            if (byte == '\n') {
                if (used == 1u && command[0] == 'q') { result = 0; break; }
                if (used != 33u || command[0] != 'r') goto done;
                uint8_t target[16];
                for (size_t i = 0; i < 16u; ++i) {
                    int high = digit(command[1u + i * 2u]), low = digit(command[2u + i * 2u]);
                    if (high < 0 || low < 0) goto done;
                    target[i] = (uint8_t)(high * 16 + low);
                }
                if (rns_runtime_request_path(runtime, target) != RNS_OK) goto done;
                used = 0;
            } else if (used < sizeof command) command[used++] = byte;
            else goto done;
        } else if (count == 0 || (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) goto done;
        (void)rns_hal_sleep_ms(1u);
    }
done:
    rns_runtime_destroy(runtime); rns_hal_secure_zero(&identity, sizeof identity);
    return result;
}
