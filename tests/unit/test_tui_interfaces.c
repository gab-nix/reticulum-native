#include "tui_interfaces.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static rns_runtime_interface_info_t make_info(uint64_t id, const char *name) {
    rns_runtime_interface_info_t info = {0};
    info.id = id;
    assert(snprintf(info.name, sizeof info.name, "%s", name) > 0);
    info.type = RNS_CONFIG_TCP_CLIENT;
    info.state = RNS_RUNTIME_INTERFACE_DOWN;
    info.last_error = RNS_ERROR_IO;
    info.packets_received = 11u;
    info.packets_sent = 12u;
    info.bytes_received = 101u;
    info.bytes_sent = 102u;
    info.packets_dropped = 3u;
    info.connection_attempts = 7u;
    info.connections_established = 5u;
    info.connections_lost = 4u;
    info.peers = 2u;
    return info;
}

static void test_selection_survives_updates(void) {
    tui_interfaces_model_t model;
    rns_runtime_interface_info_t initial[] = {
        make_info(10u, "alpha"), make_info(20u, "beta"),
        make_info(30u, "gamma")
    };
    tui_interfaces_init(&model);
    tui_interfaces_update(&model, initial, 3u);
    assert(model.has_selection && model.selected_id == 10u);
    tui_interfaces_move(&model, 1);
    assert(model.selected_index == 1u && model.selected_id == 20u);

    rns_runtime_interface_info_t reordered[] = {
        make_info(30u, "gamma"), make_info(10u, "alpha"),
        make_info(20u, "beta")
    };
    reordered[2].packets_received = 99u;
    tui_interfaces_update(&model, reordered, 3u);
    assert(model.selected_index == 2u && model.selected_id == 20u);
    assert(model.items[2].packets_received == 99u);
    assert(tui_interfaces_first(&model, 2u) == 1u);

    tui_interfaces_update(&model, reordered, 2u);
    assert(model.selected_index == 1u && model.selected_id == 10u);
    tui_interfaces_update(&model, NULL, 0u);
    assert(!model.has_selection && model.count == 0u);
}

static void test_complete_narrow_safe_format(void) {
    rns_runtime_interface_info_t info = make_info(42u, "uplink");
    char lines[3][TUI_INTERFACE_LINE_MAX];
    tui_interfaces_format(&info, 200u, lines);
    assert(strstr(lines[0], "uplink") != NULL);
    assert(strstr(lines[0], "TCPClientInterface") != NULL);
    assert(strstr(lines[0], "down") != NULL);
    assert(strstr(lines[0], "error:") != NULL);
    assert(strstr(lines[1], "packets rx:11 tx:12") != NULL);
    assert(strstr(lines[1], "bytes rx:101 tx:102") != NULL);
    assert(strstr(lines[1], "drop:3") != NULL);
    assert(strstr(lines[2], "attempts:7 established:5 lost:4") != NULL);
    assert(strstr(lines[2], "peers:2") != NULL);

    tui_interfaces_format(&info, 34u, lines);
    for (size_t i = 0u; i < 3u; ++i) assert(strlen(lines[i]) <= 34u);
    assert(strstr(lines[1], "P:11/12") != NULL);
    assert(strstr(lines[1], "B:101/102") != NULL);
    assert(strstr(lines[1], "D:3") != NULL);
    assert(strstr(lines[2], "C:7/5/4") != NULL);
    assert(strstr(lines[2], "Peer:2") != NULL);
    tui_interfaces_format(&info, 0u, lines);
    for (size_t i = 0u; i < 3u; ++i) assert(lines[i][0] == '\0');
}

int main(void) {
    test_selection_survives_updates();
    test_complete_narrow_safe_format();
    return 0;
}
