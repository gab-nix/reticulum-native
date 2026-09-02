#include "reticulum/config.h"

#include <assert.h>
#include <stddef.h>
#include <string.h>

static const char valid_config[] =
    "[reticulum]\n"
    "  enable_transport = Yes\n"
    "  share_instance = No\n"
    "[interfaces]\n"
    "  [[Backbone]]\n"
    "    type = TCPClientInterface\n"
    "    enabled = Yes\n"
    "    target_host = 127.0.0.1\n"
    "    target_port = 4242\n"
    "  [[LAN]]\n"
    "    type = UDPInterface\n"
    "    enabled = Yes\n"
    "    listen_ip = 0.0.0.0\n"
    "    listen_port = 5000\n"
    "    forward_ip = 255.255.255.255\n"
    "    forward_port = 5000\n"
    "  [[Radio]]\n"
    "    type = RNodeInterface\n"
    "    enabled = Yes\n"
    "    port = /dev/ttyUSB0\n"
    "    speed = 115200\n"
    "    frequency = 868000000\n"
    "    bandwidth = 125000\n"
    "    txpower = 17\n"
    "    spreadingfactor = 8\n"
    "    codingrate = 5\n"
    "  [[Discovery]]\n"
    "    type = AutoInterface\n"
    "    enabled = Yes\n";

int main(void) {
    static const char unsupported[] =
        "[interfaces]\n[[Mystery]]\ntype = PipeInterface\nenabled = Yes\n";
    static const char invalid[] =
        "[interfaces]\n[[Broken]]\ntype = TCPClientInterface\nenabled = Yes\n"
        "target_port = 1234\n";
    rns_config_t config;
    rns_config_t reparsed;
    rns_config_diagnostic_t diagnostic;
    char emitted[4096];
    char tiny[8];
    size_t emitted_length = 0U;

    assert(rns_config_parse(valid_config, sizeof(valid_config) - 1U,
                            &config, &diagnostic) == RNS_OK);
    assert(config.enable_transport && !config.share_instance);
    assert(config.interface_count == 4U);
    assert(config.interfaces[0].type == RNS_CONFIG_TCP_CLIENT);
    assert(config.interfaces[1].forward_port == 5000U);
    assert(config.interfaces[2].frequency == 868000000U);
    assert(config.interfaces[3].type == RNS_CONFIG_AUTO);
    assert(rns_config_emit(&config, emitted, sizeof(emitted), &emitted_length) == RNS_OK);
    assert(rns_config_parse(emitted, emitted_length, &reparsed, &diagnostic) == RNS_OK);
    assert(reparsed.interface_count == config.interface_count);
    assert(reparsed.interfaces[2].tx_power == 17);

    assert(rns_config_parse(unsupported, sizeof(unsupported) - 1U,
                            &config, &diagnostic) == RNS_ERROR_UNSUPPORTED);
    assert(diagnostic.line == 3U && strstr(diagnostic.message, "PipeInterface") != NULL);
    assert(rns_config_parse(invalid, sizeof(invalid) - 1U,
                            &config, &diagnostic) == RNS_ERROR_PROTOCOL);
    assert(strstr(diagnostic.message, "target_host") != NULL);
    assert(rns_config_parse(valid_config, sizeof(valid_config) - 1U,
                            &config, &diagnostic) == RNS_OK);
    assert(rns_config_emit(&config, tiny, sizeof(tiny), &emitted_length) == RNS_ERROR_OVERFLOW);
    return 0;
}

