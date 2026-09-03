#include "reticulum/config.h"
#include "reticulum/local.h"

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
    "    enabled = Yes\n"
    "    group_id = nomad-lan\n"
    "    discovery_scope = ORGANISATION\n"
    "    multicast_address_type = PERMANENT\n"
    "    discovery_port = 48555\n"
    "    data_port = 49555\n"
    "    devices = en0, eth0\n"
    "    ignored_devices = tun0\n"
    "    bitrate = 12000000\n";

static const char kiss_config[] =
    "[reticulum]\nshare_instance = No\n[interfaces]\n[[TNC]]\n"
    "type = KISSInterface\nenabled = Yes\nport = /dev/tty.test\n"
    "flow_control = Yes\npreamble = 420\ntxtail = 30\n"
    "persistence = 128\nslottime = 40\ndatabits = 7\n"
    "parity = even\nstopbits = 2\n";

int main(void) {
    static const char unsupported[] =
        "[interfaces]\n[[Mystery]]\ntype = PipeInterface\nenabled = Yes\n";
    static const char invalid[] =
        "[interfaces]\n[[Broken]]\ntype = TCPClientInterface\nenabled = Yes\n"
        "target_port = 1234\n";
    static const char shared[] =
        "[reticulum]\nshare_instance = Yes\nshared_instance_type = tcp\n"
        "instance_name = nomad\nshared_instance_port = 48123\n"
        "instance_control_port = 48124\n";
    static const char shared_unix[] =
        "[reticulum]\nshare_instance = Yes\nshared_instance_type = unix\n";
    static const char rnode_config[] =
        "[reticulum]\nshare_instance = No\n[interfaces]\n[[Radio]]\n"
        "type = RNodeInterface\nenabled = Yes\nport = /dev/ttyACM0\n"
        "frequency = 915000000\nbandwidth = 125000\ntxpower = 17\n"
        "spreadingfactor = 8\ncodingrate = 5\nflow_control = Yes\n"
        "airtime_limit_short = 25.5\nairtime_limit_long = 50.00\n";
    static const char rnode_tcp[] =
        "[interfaces]\n[[Radio]]\ntype = RNodeInterface\nenabled = Yes\n"
        "port = tcp://127.0.0.1\nfrequency = 915000000\n"
        "bandwidth = 125000\ntxpower = 17\nspreadingfactor = 8\n"
        "codingrate = 5\n";
    static const char invalid_auto[] =
        "[interfaces]\n[[LAN]]\ntype = AutoInterface\nenabled = Yes\n"
        "discovery_scope = universe\n";
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
    assert(strcmp(config.interfaces[3].group_id, "nomad-lan") == 0);
    assert(strcmp(config.interfaces[3].discovery_scope, "organisation") == 0);
    assert(strcmp(config.interfaces[3].multicast_address_type, "permanent") == 0);
    assert(config.interfaces[3].discovery_port == 48555u &&
           config.interfaces[3].data_port == 49555u &&
           config.interfaces[3].bitrate == 12000000u);
    assert(strcmp(config.interfaces[3].devices, "en0, eth0") == 0 &&
           strcmp(config.interfaces[3].ignored_devices, "tun0") == 0);
    assert(rns_config_emit(&config, emitted, sizeof(emitted), &emitted_length) == RNS_OK);
    assert(strstr(emitted, "    configured_bitrate = 12000000\n") != NULL);
    assert(strstr(emitted, "\n    bitrate = ") == NULL);
    assert(rns_config_parse(emitted, emitted_length, &reparsed, &diagnostic) == RNS_OK);
    assert(reparsed.interface_count == config.interface_count);
    assert(reparsed.interfaces[2].tx_power == 17);
    assert(strcmp(reparsed.interfaces[3].group_id, "nomad-lan") == 0 &&
           reparsed.interfaces[3].discovery_port == 48555u &&
           reparsed.interfaces[3].data_port == 49555u);
    assert(reparsed.shared_instance_port == 37428U);
    assert(reparsed.instance_control_port == 37429U);

    assert(rns_config_parse(unsupported, sizeof(unsupported) - 1U,
                            &config, &diagnostic) == RNS_ERROR_UNSUPPORTED);
    assert(diagnostic.line == 3U && strstr(diagnostic.message, "PipeInterface") != NULL);
    assert(rns_config_parse(invalid, sizeof(invalid) - 1U,
                            &config, &diagnostic) == RNS_ERROR_PROTOCOL);
    assert(strstr(diagnostic.message, "target_host") != NULL);
    assert(rns_config_parse(invalid_auto, sizeof(invalid_auto) - 1u, &config,
                            &diagnostic) == RNS_ERROR_PROTOCOL);
    assert(strstr(diagnostic.message, "discovery_scope") != NULL);
    assert(rns_config_parse(valid_config, sizeof(valid_config) - 1U,
                            &config, &diagnostic) == RNS_OK);
    assert(rns_config_emit(&config, tiny, sizeof(tiny), &emitted_length) == RNS_ERROR_OVERFLOW);
    assert(rns_config_parse(shared, sizeof(shared) - 1U, &config, &diagnostic) ==
           RNS_OK);
    assert(config.share_instance &&
           config.shared_instance_type == RNS_CONFIG_SHARED_INSTANCE_TCP);
    assert(strcmp(config.instance_name, "nomad") == 0);
    assert(config.shared_instance_port == 48123U &&
           config.instance_data_port == 48123U &&
           config.instance_control_port == 48124U);
    rns_local_options_t local_options;
    assert(rns_local_options_from_config(&config, RNS_LOCAL_ROLE_AUTO,
                                         &local_options) == RNS_OK);
    assert(local_options.port == 48123U);
    assert(rns_config_parse(shared_unix, sizeof(shared_unix) - 1U, &config,
                            &diagnostic) == RNS_OK);
    assert(rns_local_options_from_config(&config, RNS_LOCAL_ROLE_AUTO,
                                         &local_options) ==
           RNS_ERROR_UNSUPPORTED);
    assert(rns_config_parse(kiss_config, sizeof(kiss_config) - 1U, &config,
                            &diagnostic) == RNS_OK);
    assert(config.interfaces[0].speed == 9600U);
    assert(config.interfaces[0].preamble_ms == 420U &&
           config.interfaces[0].tx_tail_ms == 30U &&
           config.interfaces[0].slot_time_ms == 40U &&
           config.interfaces[0].persistence == 128U &&
           config.interfaces[0].flow_control &&
           config.interfaces[0].data_bits == 7U &&
           config.interfaces[0].parity == 'E' &&
           config.interfaces[0].stop_bits == 2U);
    assert(rns_config_emit(&config, emitted, sizeof(emitted),
                           &emitted_length) == RNS_OK);
    assert(rns_config_parse(emitted, emitted_length, &reparsed, &diagnostic) ==
           RNS_OK);
    assert(rns_config_parse(rnode_config, sizeof(rnode_config) - 1U, &config,
                            &diagnostic) == RNS_OK);
    assert(config.interfaces[0].speed == 115200U &&
           config.interfaces[0].short_airtime_limit_set &&
           config.interfaces[0].short_airtime_limit_hundredths == 2550U &&
           config.interfaces[0].long_airtime_limit_set &&
           config.interfaces[0].long_airtime_limit_hundredths == 5000U);
    assert(rns_config_emit(&config, emitted, sizeof(emitted),
                           &emitted_length) == RNS_OK);
    assert(rns_config_parse(emitted, emitted_length, &reparsed, &diagnostic) ==
           RNS_OK);
    assert(reparsed.interfaces[0].short_airtime_limit_hundredths == 2550U);
    assert(rns_config_parse(rnode_tcp, sizeof(rnode_tcp) - 1U, &config,
                            &diagnostic) == RNS_ERROR_UNSUPPORTED);
    assert(strstr(diagnostic.message, "POSIX serial") != NULL);
    return 0;
}
