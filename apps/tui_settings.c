#include "tui_settings.h"

#include "tui_text.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define SETTINGS_V1_HEADER_SIZE 40u
#define SETTINGS_V2_HEADER_SIZE 20u
#define SETTINGS_V2_BODY_MAX 2048u

enum settings_record_type {
    SETTINGS_RECORD_DISPLAY_NAME = 1,
    SETTINGS_RECORD_GENERAL = 2,
    SETTINGS_RECORD_RRC_HUB = 10,
    SETTINGS_RECORD_RRC_IDENTITY = 11,
    SETTINGS_RECORD_RRC_NICK = 12,
    SETTINGS_RECORD_RRC_ROOM = 13,
    SETTINGS_RECORD_RRC_DRAFT = 14,
    SETTINGS_RECORD_RRC_RECONNECT = 15
};

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

static uint32_t v1_settings_crc(const uint8_t header[SETTINGS_V1_HEADER_SIZE],
                                const uint8_t *name, size_t name_length) {
    uint32_t crc = crc32_update(UINT32_MAX, header + 8u, 28u);
    return ~crc32_update(crc, name, name_length);
}

void tui_settings_defaults(tui_settings_t *settings) {
    static const char default_name[] = "Anonymous Peer";
    if (settings == NULL) return;
    memset(settings, 0, sizeof *settings);
    settings->format_version = 2u;
    memcpy(settings->display_name, default_name, sizeof default_name);
    settings->display_name_len = sizeof default_name - 1u;
    settings->announce_at_start = true;
    settings->announce_interval_minutes = TUI_SETTINGS_DEFAULT_ANNOUNCE_MINUTES;
    settings->rrc_auto_reconnect = true;
}

static bool stored_text_valid(const char *text, size_t capacity,
                              bool allow_control) {
    size_t length = strnlen(text, capacity + 1u);
    if (length > capacity ||
        !tui_utf8_valid((const uint8_t *)text, length)) return false;
    if (!allow_control)
        for (size_t i = 0u; i < length; ++i) {
            unsigned char byte = (unsigned char)text[i];
            if (byte < 0x20u || byte == 0x7fu) return false;
        }
    return true;
}

static bool hex_or_empty(const char *text, size_t digits) {
    size_t length = strnlen(text, digits + 1u);
    if (length == 0u) return true;
    if (length != digits) return false;
    for (size_t i = 0u; i < length; ++i)
        if (!((text[i] >= '0' && text[i] <= '9') ||
              (text[i] >= 'a' && text[i] <= 'f') ||
              (text[i] >= 'A' && text[i] <= 'F'))) return false;
    return true;
}

static bool known_record(uint16_t type) {
    return type == SETTINGS_RECORD_DISPLAY_NAME ||
           type == SETTINGS_RECORD_GENERAL || type == SETTINGS_RECORD_RRC_HUB ||
           type == SETTINGS_RECORD_RRC_IDENTITY ||
           type == SETTINGS_RECORD_RRC_NICK || type == SETTINGS_RECORD_RRC_ROOM ||
           type == SETTINGS_RECORD_RRC_DRAFT ||
           type == SETTINGS_RECORD_RRC_RECONNECT;
}

static bool unknown_records_valid(const tui_settings_t *settings) {
    size_t offset = 0u;
    if (settings->unknown_records_length > TUI_SETTINGS_UNKNOWN_MAX) return false;
    while (offset < settings->unknown_records_length) {
        if (settings->unknown_records_length - offset < 4u) return false;
        uint16_t type = get16(settings->unknown_records + offset);
        size_t length = get16(settings->unknown_records + offset + 2u);
        if (known_record(type) ||
            length > settings->unknown_records_length - offset - 4u) return false;
        offset += 4u + length;
    }
    return true;
}

bool tui_settings_valid(const tui_settings_t *settings) {
    if (settings == NULL || settings->format_version < 2u ||
        settings->display_name_len > LXMF_DISPLAY_NAME_MAX ||
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
    return hex_or_empty(settings->rrc_hub_address,
                        TUI_SETTINGS_RRC_HUB_ADDRESS_MAX) &&
           hex_or_empty(settings->rrc_public_identity,
                        TUI_SETTINGS_RRC_PUBLIC_IDENTITY_MAX) &&
           stored_text_valid(settings->rrc_nick, TUI_SETTINGS_RRC_NICK_MAX,
                             false) &&
           stored_text_valid(settings->rrc_last_room, TUI_SETTINGS_RRC_ROOM_MAX,
                             false) &&
           stored_text_valid(settings->rrc_draft, TUI_SETTINGS_RRC_DRAFT_MAX,
                             true) && unknown_records_valid(settings);
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

static uint32_t v2_settings_crc(const uint8_t header[SETTINGS_V2_HEADER_SIZE],
                                const uint8_t *body, size_t body_length) {
    uint32_t crc = crc32_update(UINT32_MAX, header + 8u, 8u);
    return ~crc32_update(crc, body, body_length);
}

static bool load_v1(FILE *file, tui_settings_t *loaded) {
    uint8_t header[SETTINGS_V1_HEADER_SIZE];
    uint8_t name[LXMF_DISPLAY_NAME_MAX];
    if (fread(header, 1u, sizeof header, file) != sizeof header ||
        memcmp(header, "NOMSET\0\0", 8u) != 0 || get16(header + 8u) != 1u ||
        get16(header + 10u) != SETTINGS_V1_HEADER_SIZE) return false;
    uint16_t name_length = get16(header + 12u);
    if (name_length > LXMF_DISPLAY_NAME_MAX ||
        (name_length != 0u && fread(name, 1u, name_length, file) != name_length) ||
        fgetc(file) != EOF ||
        v1_settings_crc(header, name, name_length) != get32(header + 36u))
        return false;
    uint8_t flags = header[14];
    if ((flags & 0xf8u) != 0u || header[15] > 254u) return false;
    tui_settings_defaults(loaded);
    memcpy(loaded->display_name, name, name_length);
    loaded->display_name[name_length] = '\0';
    loaded->display_name_len = name_length;
    loaded->announce_at_start = (flags & 0x01u) != 0u;
    loaded->has_stamp_cost = (flags & 0x02u) != 0u;
    loaded->stamp_cost = header[15];
    loaded->has_propagation_node = (flags & 0x04u) != 0u;
    loaded->announce_interval_minutes = get32(header + 16u);
    memcpy(loaded->propagation_node, header + 20u,
           sizeof loaded->propagation_node);
    return tui_settings_valid(loaded);
}

static bool set_text(char *target, size_t capacity, const uint8_t *data,
                     size_t length) {
    if (length > capacity || memchr(data, '\0', length) != NULL) return false;
    memcpy(target, data, length);
    target[length] = '\0';
    return true;
}

static bool load_v2(FILE *file, uint16_t version, tui_settings_t *loaded) {
    uint8_t header[SETTINGS_V2_HEADER_SIZE];
    uint8_t body[SETTINGS_V2_BODY_MAX];
    bool seen_display = false, seen_general = false, seen_hub = false;
    bool seen_identity = false, seen_nick = false, seen_room = false;
    bool seen_draft = false, seen_reconnect = false;
    if (fread(header, 1u, sizeof header, file) != sizeof header ||
        memcmp(header, "NOMSET\0\0", 8u) != 0 ||
        get16(header + 8u) != version || version < 2u ||
        get16(header + 10u) != SETTINGS_V2_HEADER_SIZE) return false;
    size_t body_length = get32(header + 12u);
    if (body_length > sizeof body ||
        (body_length != 0u && fread(body, 1u, body_length, file) != body_length) ||
        fgetc(file) != EOF ||
        v2_settings_crc(header, body, body_length) != get32(header + 16u))
        return false;
    tui_settings_defaults(loaded);
    loaded->format_version = version;
    size_t offset = 0u;
    while (offset < body_length) {
        if (body_length - offset < 4u) return false;
        const uint8_t *record = body + offset;
        uint16_t type = get16(record);
        size_t length = get16(record + 2u);
        if (length > body_length - offset - 4u) return false;
        const uint8_t *value = record + 4u;
        switch (type) {
            case SETTINGS_RECORD_DISPLAY_NAME:
                if (seen_display || length > LXMF_DISPLAY_NAME_MAX ||
                    !set_text(loaded->display_name, LXMF_DISPLAY_NAME_MAX,
                              value, length)) return false;
                loaded->display_name_len = length;
                seen_display = true;
                break;
            case SETTINGS_RECORD_GENERAL: {
                if (seen_general || length != 22u || (value[0] & 0xf8u) != 0u ||
                    value[1] > 254u) return false;
                loaded->announce_at_start = (value[0] & 0x01u) != 0u;
                loaded->has_stamp_cost = (value[0] & 0x02u) != 0u;
                loaded->has_propagation_node = (value[0] & 0x04u) != 0u;
                loaded->stamp_cost = value[1];
                loaded->announce_interval_minutes = get32(value + 2u);
                memcpy(loaded->propagation_node, value + 6u,
                       sizeof loaded->propagation_node);
                seen_general = true;
                break;
            }
            case SETTINGS_RECORD_RRC_HUB:
                if (seen_hub || !set_text(loaded->rrc_hub_address,
                    TUI_SETTINGS_RRC_HUB_ADDRESS_MAX, value, length)) return false;
                seen_hub = true;
                break;
            case SETTINGS_RECORD_RRC_IDENTITY:
                if (seen_identity || !set_text(loaded->rrc_public_identity,
                    TUI_SETTINGS_RRC_PUBLIC_IDENTITY_MAX, value, length)) return false;
                seen_identity = true;
                break;
            case SETTINGS_RECORD_RRC_NICK:
                if (seen_nick || !set_text(loaded->rrc_nick,
                    TUI_SETTINGS_RRC_NICK_MAX, value, length)) return false;
                seen_nick = true;
                break;
            case SETTINGS_RECORD_RRC_ROOM:
                if (seen_room || !set_text(loaded->rrc_last_room,
                    TUI_SETTINGS_RRC_ROOM_MAX, value, length)) return false;
                seen_room = true;
                break;
            case SETTINGS_RECORD_RRC_DRAFT:
                if (seen_draft || !set_text(loaded->rrc_draft,
                    TUI_SETTINGS_RRC_DRAFT_MAX, value, length)) return false;
                seen_draft = true;
                break;
            case SETTINGS_RECORD_RRC_RECONNECT:
                if (seen_reconnect || length != 1u || value[0] > 1u) return false;
                loaded->rrc_auto_reconnect = value[0] != 0u;
                seen_reconnect = true;
                break;
            default:
                if (4u + length > TUI_SETTINGS_UNKNOWN_MAX -
                        loaded->unknown_records_length) return false;
                memcpy(loaded->unknown_records + loaded->unknown_records_length,
                       record, 4u + length);
                loaded->unknown_records_length += 4u + length;
                break;
        }
        offset += 4u + length;
    }
    return tui_settings_valid(loaded);
}

bool tui_settings_load(const char *path, tui_settings_t *settings, bool *found) {
    uint8_t prefix[12];
    tui_settings_t loaded;
    if (found != NULL) *found = false;
    if (path == NULL || settings == NULL || strlen(path) > TUI_SETTINGS_PATH_MAX)
        return false;
    FILE *file = fopen(path, "rb");
    if (file == NULL) {
        if (errno != ENOENT) return false;
        tui_settings_defaults(settings);
        return true;
    }
    bool okay = fread(prefix, 1u, sizeof prefix, file) == sizeof prefix &&
                memcmp(prefix, "NOMSET\0\0", 8u) == 0 &&
                fseek(file, 0L, SEEK_SET) == 0;
    if (okay) {
        uint16_t version = get16(prefix + 8u);
        if (version == 1u) okay = load_v1(file, &loaded);
        else if (version >= 2u) okay = load_v2(file, version, &loaded);
        else okay = false;
    }
    if (fclose(file) != 0) okay = false;
    if (!okay) return false;
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

static bool append_record(uint8_t *body, size_t capacity, size_t *used,
                          uint16_t type, const uint8_t *value, size_t length) {
    if (length > UINT16_MAX || length > capacity - *used ||
        4u > capacity - *used - length) return false;
    put16(body + *used, type);
    put16(body + *used + 2u, (uint16_t)length);
    if (length != 0u) memcpy(body + *used + 4u, value, length);
    *used += 4u + length;
    return true;
}

bool tui_settings_save(const char *path, const tui_settings_t *settings) {
    uint8_t header[SETTINGS_V2_HEADER_SIZE] = {0};
    uint8_t body[SETTINGS_V2_BODY_MAX];
    uint8_t general[22u];
    size_t used = 0u;
    char temporary[TUI_SETTINGS_PATH_MAX + 5u];
    if (path == NULL || !tui_settings_valid(settings) ||
        strlen(path) > TUI_SETTINGS_PATH_MAX) return false;
    int length = snprintf(temporary, sizeof temporary, "%s.tmp", path);
    if (length <= 0 || (size_t)length >= sizeof temporary) return false;
    memset(general, 0, sizeof general);
    general[0] = (uint8_t)((settings->announce_at_start ? 0x01u : 0u) |
                           (settings->has_stamp_cost ? 0x02u : 0u) |
                           (settings->has_propagation_node ? 0x04u : 0u));
    general[1] = settings->stamp_cost;
    put32(general + 2u, settings->announce_interval_minutes);
    memcpy(general + 6u, settings->propagation_node,
           sizeof settings->propagation_node);
    uint8_t reconnect = settings->rrc_auto_reconnect ? 1u : 0u;
    bool encoded = append_record(body, sizeof body, &used,
            SETTINGS_RECORD_DISPLAY_NAME,
            (const uint8_t *)settings->display_name, settings->display_name_len) &&
        append_record(body, sizeof body, &used, SETTINGS_RECORD_GENERAL,
                      general, sizeof general) &&
        append_record(body, sizeof body, &used, SETTINGS_RECORD_RRC_HUB,
            (const uint8_t *)settings->rrc_hub_address,
            strlen(settings->rrc_hub_address)) &&
        append_record(body, sizeof body, &used, SETTINGS_RECORD_RRC_IDENTITY,
            (const uint8_t *)settings->rrc_public_identity,
            strlen(settings->rrc_public_identity)) &&
        append_record(body, sizeof body, &used, SETTINGS_RECORD_RRC_NICK,
            (const uint8_t *)settings->rrc_nick, strlen(settings->rrc_nick)) &&
        append_record(body, sizeof body, &used, SETTINGS_RECORD_RRC_ROOM,
            (const uint8_t *)settings->rrc_last_room,
            strlen(settings->rrc_last_room)) &&
        append_record(body, sizeof body, &used, SETTINGS_RECORD_RRC_DRAFT,
            (const uint8_t *)settings->rrc_draft, strlen(settings->rrc_draft)) &&
        append_record(body, sizeof body, &used, SETTINGS_RECORD_RRC_RECONNECT,
                      &reconnect, 1u);
    if (!encoded || settings->unknown_records_length > sizeof body - used)
        return false;
    memcpy(body + used, settings->unknown_records,
           settings->unknown_records_length);
    used += settings->unknown_records_length;
    memcpy(header, "NOMSET\0\0", 8u);
    put16(header + 8u, settings->format_version);
    put16(header + 10u, SETTINGS_V2_HEADER_SIZE);
    put32(header + 12u, (uint32_t)used);
    put32(header + 16u, v2_settings_crc(header, body, used));

    int descriptor = open(temporary, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (descriptor < 0) return false;
    bool okay = write_all(descriptor, header, sizeof header) &&
                write_all(descriptor, body, used) &&
                fsync(descriptor) == 0;
    if (close(descriptor) != 0) okay = false;
    if (okay && rename(temporary, path) != 0) okay = false;
    if (okay) okay = sync_parent(path);
    if (!okay) (void)unlink(temporary);
    return okay;
}
