#include "tui_settings.h"

#include "tui_text.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define SETTINGS_HEADER_SIZE 40u

static void put16(uint8_t *out, uint16_t value) {
    out[0] = (uint8_t)(value >> 8);
    out[1] = (uint8_t)value;
}

static uint16_t get16(const uint8_t *input) {
    return (uint16_t)(((uint16_t)input[0] << 8) | input[1]);
}

static void put32(uint8_t *out, uint32_t value) {
    out[0] = (uint8_t)(value >> 24);
    out[1] = (uint8_t)(value >> 16);
    out[2] = (uint8_t)(value >> 8);
    out[3] = (uint8_t)value;
}

static uint32_t get32(const uint8_t *input) {
    return ((uint32_t)input[0] << 24) | ((uint32_t)input[1] << 16) |
           ((uint32_t)input[2] << 8) | input[3];
}

static uint32_t crc32_update(uint32_t crc, const uint8_t *data, size_t length) {
    while (length-- != 0u) {
        crc ^= *data++;
        for (unsigned bit = 0u; bit < 8u; ++bit)
            crc = (crc >> 1) ^ (0xedb88320u & (uint32_t)(0u - (crc & 1u)));
    }
    return crc;
}

static uint32_t settings_crc(const uint8_t header[SETTINGS_HEADER_SIZE],
                             const uint8_t *name, size_t name_length) {
    uint32_t crc = crc32_update(UINT32_MAX, header + 8u, 28u);
    return ~crc32_update(crc, name, name_length);
}

void tui_settings_defaults(tui_settings_t *settings) {
    static const char default_name[] = "Anonymous Peer";
    if (settings == NULL) return;
    memset(settings, 0, sizeof *settings);
    memcpy(settings->display_name, default_name, sizeof default_name);
    settings->display_name_len = sizeof default_name - 1u;
    settings->announce_at_start = true;
    settings->announce_interval_minutes = TUI_SETTINGS_DEFAULT_ANNOUNCE_MINUTES;
}

bool tui_settings_valid(const tui_settings_t *settings) {
    if (settings == NULL || settings->display_name_len > LXMF_DISPLAY_NAME_MAX ||
        settings->display_name[settings->display_name_len] != '\0' ||
        !tui_utf8_valid((const uint8_t *)settings->display_name,
                        settings->display_name_len) ||
        settings->announce_interval_minutes < TUI_SETTINGS_MIN_ANNOUNCE_MINUTES)
        return false;
    for (size_t i = 0u; i < settings->display_name_len; ++i) {
        unsigned char byte = (unsigned char)settings->display_name[i];
        if (byte < 0x20u || byte == 0x7fu) return false;
    }
    if (settings->has_stamp_cost &&
        (settings->stamp_cost < 1u || settings->stamp_cost > 254u))
        return false;
    return true;
}

uint64_t tui_settings_interval_ms(const tui_settings_t *settings) {
    if (!tui_settings_valid(settings)) return 0u;
    return (uint64_t)settings->announce_interval_minutes * UINT64_C(60000);
}

bool tui_settings_announce_due(bool startup_pending, uint64_t next_announce_ms,
                               uint64_t now_ms) {
    return startup_pending ||
           (next_announce_ms != 0u && now_ms >= next_announce_ms);
}

lxmf_status_t tui_settings_encode_announce(const tui_settings_t *settings,
                                           uint8_t *output, size_t capacity,
                                           size_t *written) {
    lxmf_announce_data_t announce = {0};
    if (!tui_settings_valid(settings) || output == NULL || written == NULL)
        return LXMF_ERR_ARGUMENT;
    memcpy(announce.display_name, settings->display_name,
           settings->display_name_len);
    announce.display_name_len = settings->display_name_len;
    announce.has_stamp_cost = settings->has_stamp_cost;
    announce.stamp_cost = settings->stamp_cost;
    announce.features = LXMF_FEATURE_COMPRESSION;
    return lxmf_announce_encode(&announce, output, capacity, written);
}

static bool sync_parent(const char *path) {
    char directory[TUI_SETTINGS_PATH_MAX + 1u];
    size_t length = strlen(path);
    if (length > TUI_SETTINGS_PATH_MAX) return false;
    memcpy(directory, path, length + 1u);
    char *slash = strrchr(directory, '/');
    if (slash != NULL) {
        if (slash == directory) slash[1] = '\0';
        else *slash = '\0';
    } else {
        memcpy(directory, ".", 2u);
    }
    int descriptor = open(directory, O_RDONLY);
    if (descriptor < 0) return false;
    bool okay = fsync(descriptor) == 0;
    (void)close(descriptor);
    return okay;
}

bool tui_settings_load(const char *path, tui_settings_t *settings, bool *found) {
    uint8_t header[SETTINGS_HEADER_SIZE];
    tui_settings_t loaded;
    if (found != NULL) *found = false;
    if (path == NULL || settings == NULL || strlen(path) > TUI_SETTINGS_PATH_MAX)
        return false;
    tui_settings_defaults(settings);
    FILE *file = fopen(path, "rb");
    if (file == NULL) return errno == ENOENT;
    bool okay = fread(header, 1u, sizeof header, file) == sizeof header &&
                memcmp(header, "NOMSET\0\0", 8u) == 0 && get16(header + 8u) == 1u &&
                get16(header + 10u) == SETTINGS_HEADER_SIZE;
    uint16_t name_length = okay ? get16(header + 12u) : 0u;
    if (name_length > LXMF_DISPLAY_NAME_MAX) okay = false;
    uint8_t name[LXMF_DISPLAY_NAME_MAX];
    if (okay && name_length != 0u &&
        fread(name, 1u, name_length, file) != name_length)
        okay = false;
    if (okay && fgetc(file) != EOF) okay = false;
    if (fclose(file) != 0) okay = false;
    if (!okay || settings_crc(header, name, name_length) != get32(header + 36u))
        return false;

    memset(&loaded, 0, sizeof loaded);
    memcpy(loaded.display_name, name, name_length);
    loaded.display_name[name_length] = '\0';
    loaded.display_name_len = name_length;
    uint8_t flags = header[14];
    if ((flags & 0xf8u) != 0u || header[15] > 254u) return false;
    loaded.announce_at_start = (flags & 0x01u) != 0u;
    loaded.has_stamp_cost = (flags & 0x02u) != 0u;
    loaded.stamp_cost = header[15];
    loaded.has_propagation_node = (flags & 0x04u) != 0u;
    loaded.announce_interval_minutes = get32(header + 16u);
    memcpy(loaded.propagation_node, header + 20u,
           sizeof loaded.propagation_node);
    if (!tui_settings_valid(&loaded)) return false;
    *settings = loaded;
    if (found != NULL) *found = true;
    return true;
}

static bool write_all(int descriptor, const uint8_t *data, size_t length) {
    size_t offset = 0u;
    while (offset < length) {
        ssize_t written = write(descriptor, data + offset, length - offset);
        if (written <= 0) return false;
        offset += (size_t)written;
    }
    return true;
}

bool tui_settings_save(const char *path, const tui_settings_t *settings) {
    uint8_t header[SETTINGS_HEADER_SIZE] = {0};
    char temporary[TUI_SETTINGS_PATH_MAX + 5u];
    if (path == NULL || !tui_settings_valid(settings) ||
        strlen(path) > TUI_SETTINGS_PATH_MAX) return false;
    int length = snprintf(temporary, sizeof temporary, "%s.tmp", path);
    if (length <= 0 || (size_t)length >= sizeof temporary) return false;
    memcpy(header, "NOMSET\0\0", 8u);
    put16(header + 8u, 1u);
    put16(header + 10u, SETTINGS_HEADER_SIZE);
    put16(header + 12u, (uint16_t)settings->display_name_len);
    header[14] = (uint8_t)((settings->announce_at_start ? 0x01u : 0u) |
                           (settings->has_stamp_cost ? 0x02u : 0u) |
                           (settings->has_propagation_node ? 0x04u : 0u));
    header[15] = settings->stamp_cost;
    put32(header + 16u, settings->announce_interval_minutes);
    memcpy(header + 20u, settings->propagation_node,
           sizeof settings->propagation_node);
    put32(header + 36u,
          settings_crc(header, (const uint8_t *)settings->display_name,
                       settings->display_name_len));

    int descriptor = open(temporary, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (descriptor < 0) return false;
    bool okay = write_all(descriptor, header, sizeof header) &&
                write_all(descriptor, (const uint8_t *)settings->display_name,
                          settings->display_name_len) &&
                fsync(descriptor) == 0;
    if (close(descriptor) != 0) okay = false;
    if (okay && rename(temporary, path) != 0) okay = false;
    if (okay) okay = sync_parent(path);
    if (!okay) (void)unlink(temporary);
    return okay;
}
