#include "tui_settings.h"

#include <assert.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main(void) {
    char path[] = "/tmp/nomad-settings-XXXXXX";
    int descriptor = mkstemp(path);
    assert(descriptor >= 0);
    assert(close(descriptor) == 0);
    assert(unlink(path) == 0);

    tui_settings_t settings;
    bool found = true;
    assert(tui_settings_load(path, &settings, &found));
    assert(!found);
    assert(strcmp(settings.display_name, "Anonymous Peer") == 0);
    assert(settings.announce_at_start);
    assert(settings.announce_interval_minutes == 360u);
    assert(!settings.has_stamp_cost && !settings.has_propagation_node);
    assert(tui_settings_interval_ms(&settings) == UINT64_C(21600000));
    assert(tui_settings_announce_due(true, 0u, 1u));
    assert(!tui_settings_announce_due(false, 0u, 1u));
    assert(!tui_settings_announce_due(false, 20u, 19u));
    assert(tui_settings_announce_due(false, 20u, 20u));

    memcpy(settings.display_name, "Rei", 4u);
    settings.display_name_len = 3u;
    settings.has_stamp_cost = true;
    settings.stamp_cost = 8u;
    settings.announce_at_start = false;
    settings.announce_interval_minutes = 90u;
    settings.has_propagation_node = true;
    for (size_t i = 0u; i < sizeof settings.propagation_node; ++i)
        settings.propagation_node[i] = (uint8_t)(i + 1u);
    assert(tui_settings_save(path, &settings));

    tui_settings_t loaded;
    found = false;
    assert(tui_settings_load(path, &loaded, &found) && found);
    assert(strcmp(loaded.display_name, "Rei") == 0);
    assert(loaded.has_stamp_cost && loaded.stamp_cost == 8u);
    assert(!loaded.announce_at_start && loaded.announce_interval_minutes == 90u);
    assert(loaded.has_propagation_node);
    assert(memcmp(loaded.propagation_node, settings.propagation_node,
                  sizeof loaded.propagation_node) == 0);

    uint8_t announce[32];
    size_t announce_length = 0u;
    static const uint8_t expected_announce[] = {
        0x93u, 0xc4u, 0x03u, 'R', 'e', 'i', 0x08u, 0x91u, 0x00u
    };
    assert(tui_settings_encode_announce(&loaded, announce, sizeof announce,
                                        &announce_length) == LXMF_OK);
    assert(announce_length == sizeof expected_announce);
    assert(memcmp(announce, expected_announce, sizeof expected_announce) == 0);

    loaded.announce_interval_minutes = 29u;
    assert(!tui_settings_valid(&loaded));
    loaded = settings;
    loaded.stamp_cost = 255u;
    assert(!tui_settings_valid(&loaded));
    loaded = settings;
    loaded.display_name[1] = '\n';
    assert(!tui_settings_valid(&loaded));

    descriptor = open(path, O_WRONLY | O_TRUNC);
    assert(descriptor >= 0);
    assert(write(descriptor, "bad", 3u) == 3);
    assert(close(descriptor) == 0);
    assert(!tui_settings_load(path, &loaded, NULL));
    assert(unlink(path) == 0);
    return 0;
}
