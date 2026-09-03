#ifndef NOMAD_TUI_SETTINGS_H
#define NOMAD_TUI_SETTINGS_H

#include "reticulum/lxmf_router.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define TUI_SETTINGS_PATH_MAX 1024u
#define TUI_SETTINGS_MIN_ANNOUNCE_MINUTES 30u
#define TUI_SETTINGS_DEFAULT_ANNOUNCE_MINUTES 360u
#define TUI_SETTINGS_UNKNOWN_MAX 1024u
#define TUI_SETTINGS_RRC_HUB_ADDRESS_MAX 32u
#define TUI_SETTINGS_RRC_PUBLIC_IDENTITY_MAX 128u
#define TUI_SETTINGS_RRC_NICK_MAX 32u
#define TUI_SETTINGS_RRC_ROOM_MAX 64u
#define TUI_SETTINGS_RRC_DRAFT_MAX 350u

typedef struct {
    uint16_t format_version;
    char display_name[LXMF_DISPLAY_NAME_MAX + 1u];
    size_t display_name_len;
    bool has_stamp_cost;
    uint8_t stamp_cost;
    bool announce_at_start;
    uint32_t announce_interval_minutes;
    bool has_propagation_node;
    uint8_t propagation_node[LXMF_DESTINATION_LENGTH];
    char rrc_hub_address[TUI_SETTINGS_RRC_HUB_ADDRESS_MAX + 1u];
    char rrc_public_identity[TUI_SETTINGS_RRC_PUBLIC_IDENTITY_MAX + 1u];
    char rrc_nick[TUI_SETTINGS_RRC_NICK_MAX + 1u];
    char rrc_last_room[TUI_SETTINGS_RRC_ROOM_MAX + 1u];
    char rrc_draft[TUI_SETTINGS_RRC_DRAFT_MAX + 1u];
    bool rrc_auto_reconnect;
    /* Complete unrecognised v2 TLV records, retained verbatim on save. */
    uint8_t unknown_records[TUI_SETTINGS_UNKNOWN_MAX];
    size_t unknown_records_length;
} tui_settings_t;

void tui_settings_defaults(tui_settings_t *settings);
bool tui_settings_valid(const tui_settings_t *settings);

/* Pure helpers used by the caller-polled announcement scheduler. */
uint64_t tui_settings_interval_ms(const tui_settings_t *settings);
bool tui_settings_announce_due(bool startup_pending, uint64_t next_announce_ms,
                               uint64_t now_ms);

/* Encodes the bounded LXMF delivery announce payload; no packet work occurs here. */
lxmf_status_t tui_settings_encode_announce(const tui_settings_t *settings,
                                           uint8_t *output, size_t capacity,
                                           size_t *written);

/*
 * Loads a versioned settings file. A missing file is not an error: defaults
 * are returned and found is false. Malformed or unreadable files return false
 * without replacing settings. Version 1 is migrated in memory; unknown v2
 * records are retained verbatim when the settings are saved again.
 */
bool tui_settings_load(const char *path, tui_settings_t *settings, bool *found);

/* Writes through path.tmp, fsyncs it, renames it, then syncs the parent. */
bool tui_settings_save(const char *path, const tui_settings_t *settings);

#endif
