#include "tui_settings.h"

#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void put16(uint8_t *out, uint16_t value) {
    out[0] = (uint8_t)(value >> 8);
    out[1] = (uint8_t)value;
}

static void put32(uint8_t *out, uint32_t value) {
    out[0] = (uint8_t)(value >> 24);
    out[1] = (uint8_t)(value >> 16);
    out[2] = (uint8_t)(value >> 8);
    out[3] = (uint8_t)value;
}

static uint16_t get16(const uint8_t *in) {
    return (uint16_t)(((uint16_t)in[0] << 8) | in[1]);
}

static uint32_t get32(const uint8_t *in) {
    return ((uint32_t)in[0] << 24) | ((uint32_t)in[1] << 16) |
           ((uint32_t)in[2] << 8) | in[3];
}

static uint32_t crc_update(uint32_t crc, const uint8_t *data, size_t length) {
    while (length-- != 0u) {
        crc ^= *data++;
        for (unsigned bit = 0u; bit < 8u; ++bit)
            crc = (crc >> 1) ^ (0xedb88320u & (uint32_t)(0u - (crc & 1u)));
    }
    return crc;
}

static uint32_t v1_crc(const uint8_t *header, const uint8_t *name,
                       size_t length) {
    return ~crc_update(crc_update(UINT32_MAX, header + 8u, 28u), name, length);
}

static uint32_t v2_crc(const uint8_t *header, const uint8_t *body,
                       size_t length) {
    return ~crc_update(crc_update(UINT32_MAX, header + 8u, 8u), body, length);
}

static void write_legacy(const char *path) {
    static const uint8_t name[] = "Legacy";
    uint8_t header[40] = {0};
    memcpy(header, "NOMSET\0\0", 8u);
    put16(header + 8u, 1u);
    put16(header + 10u, 40u);
    put16(header + 12u, sizeof name - 1u);
    header[14] = 0x01u;
    put32(header + 16u, 360u);
    put32(header + 36u, v1_crc(header, name, sizeof name - 1u));
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    assert(fd >= 0);
    assert(write(fd, header, sizeof header) == (ssize_t)sizeof header);
    assert(write(fd, name, sizeof name - 1u) == (ssize_t)(sizeof name - 1u));
    assert(close(fd) == 0);
}

static void add_unknown_record(const char *path) {
    uint8_t file[4096];
    FILE *input = fopen(path, "rb");
    assert(input != NULL);
    size_t length = fread(file, 1u, sizeof file, input);
    assert(!ferror(input) && fclose(input) == 0);
    assert(length >= 20u && length + 7u <= sizeof file);
    size_t body_length = get32(file + 12u);
    assert(length == 20u + body_length);
    static const uint8_t unknown[] = {0x77u, 0x77u, 0x00u, 0x03u,
                                     'x', 'y', 'z'};
    memcpy(file + length, unknown, sizeof unknown);
    length += sizeof unknown;
    body_length += sizeof unknown;
    /* A future writer that keeps the v2 TLV framing must not be downgraded. */
    file[9] = 3u;
    put32(file + 12u, (uint32_t)body_length);
    put32(file + 16u, v2_crc(file, file + 20u, body_length));
    int fd = open(path, O_WRONLY | O_TRUNC);
    assert(fd >= 0 && write(fd, file, length) == (ssize_t)length);
    assert(close(fd) == 0);
}

static void inject_record_nul(const char *path, uint16_t wanted_type) {
    uint8_t file[4096];
    FILE *input = fopen(path, "rb");
    assert(input != NULL);
    size_t file_length = fread(file, 1u, sizeof file, input);
    assert(!ferror(input) && fclose(input) == 0 && file_length >= 20u);
    size_t body_length = get32(file + 12u);
    assert(file_length == 20u + body_length);
    size_t offset = 20u;
    bool found = false;
    while (offset < file_length) {
        assert(file_length - offset >= 4u);
        uint16_t type = get16(file + offset);
        size_t length = get16(file + offset + 2u);
        assert(length <= file_length - offset - 4u);
        if (type == wanted_type) {
            assert(length >= 2u);
            file[offset + 5u] = 0u;
            found = true;
            break;
        }
        offset += 4u + length;
    }
    assert(found);
    put32(file + 16u, v2_crc(file, file + 20u, body_length));
    int fd = open(path, O_WRONLY | O_TRUNC);
    assert(fd >= 0 && write(fd, file, file_length) == (ssize_t)file_length);
    assert(close(fd) == 0);
}

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
    assert(settings.rrc_auto_reconnect);
    assert(tui_settings_interval_ms(&settings) == UINT64_C(21600000));
    assert(tui_settings_announce_due(true, 0u, 1u));
    assert(!tui_settings_announce_due(true, 20u, 19u));
    assert(tui_settings_announce_due(true, 20u, 20u));
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
    memcpy(settings.rrc_hub_address,
           "00112233445566778899aabbccddeeff", 33u);
    memset(settings.rrc_public_identity, 'a', 128u);
    settings.rrc_public_identity[128] = '\0';
    memcpy(settings.rrc_nick, "Rei", 4u);
    memcpy(settings.rrc_last_room, "lobby", 6u);
    memcpy(settings.rrc_draft, "unfinished", 11u);
    settings.rrc_auto_reconnect = false;
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
    assert(strcmp(loaded.rrc_hub_address, settings.rrc_hub_address) == 0);
    assert(strcmp(loaded.rrc_public_identity,
                  settings.rrc_public_identity) == 0);
    assert(strcmp(loaded.rrc_nick, "Rei") == 0);
    assert(strcmp(loaded.rrc_last_room, "lobby") == 0);
    assert(strcmp(loaded.rrc_draft, "unfinished") == 0);
    assert(!loaded.rrc_auto_reconnect);

    /* Length-delimited text may not hide bytes after an embedded NUL. */
    static const uint16_t text_records[] = {1u, 10u, 14u};
    for (size_t i = 0u; i < sizeof text_records / sizeof text_records[0]; ++i) {
        assert(tui_settings_save(path, &settings));
        inject_record_nul(path, text_records[i]);
        tui_settings_t unchanged;
        tui_settings_defaults(&unchanged);
        memcpy(unchanged.display_name, "Keep", 5u);
        unchanged.display_name_len = 4u;
        tui_settings_t before = unchanged;
        assert(!tui_settings_load(path, &unchanged, &found));
        assert(memcmp(&unchanged, &before, sizeof unchanged) == 0);
    }
    assert(tui_settings_save(path, &settings));

    add_unknown_record(path);
    assert(tui_settings_load(path, &loaded, &found) && found);
    static const uint8_t unknown[] = {0x77u, 0x77u, 0x00u, 0x03u,
                                     'x', 'y', 'z'};
    assert(loaded.unknown_records_length == sizeof unknown);
    assert(loaded.format_version == 3u);
    assert(memcmp(loaded.unknown_records, unknown, sizeof unknown) == 0);
    memcpy(loaded.rrc_nick, "New", 4u);
    assert(tui_settings_save(path, &loaded));
    tui_settings_t retained;
    assert(tui_settings_load(path, &retained, &found) && found);
    assert(retained.unknown_records_length == sizeof unknown);
    assert(retained.format_version == 3u);
    assert(memcmp(retained.unknown_records, unknown, sizeof unknown) == 0);

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

    loaded = retained;
    memcpy(loaded.display_name, "Keep", 5u);
    loaded.display_name_len = 4u;
    descriptor = open(path, O_WRONLY | O_TRUNC);
    assert(descriptor >= 0);
    assert(write(descriptor, "bad", 3u) == 3);
    assert(close(descriptor) == 0);
    assert(!tui_settings_load(path, &loaded, NULL));
    assert(strcmp(loaded.display_name, "Keep") == 0);

    write_legacy(path);
    assert(tui_settings_load(path, &loaded, &found) && found);
    assert(strcmp(loaded.display_name, "Legacy") == 0);
    assert(loaded.rrc_auto_reconnect);
    assert(loaded.rrc_hub_address[0] == '\0' && loaded.rrc_draft[0] == '\0');
    assert(tui_settings_save(path, &loaded));
    uint8_t prefix[10];
    descriptor = open(path, O_RDONLY);
    assert(descriptor >= 0 && read(descriptor, prefix, sizeof prefix) ==
           (ssize_t)sizeof prefix);
    assert(close(descriptor) == 0);
    assert(prefix[8] == 0u && prefix[9] == 2u);
    assert(unlink(path) == 0);
    return 0;
}
