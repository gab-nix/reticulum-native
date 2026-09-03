#ifndef NOMAD_TUI_SETTINGS_H
#define NOMAD_TUI_SETTINGS_H

#include "reticulum/lxmf_router.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define TUI_SETTINGS_PATH_MAX 1024u
#define TUI_SETTINGS_MIN_ANNOUNCE_MINUTES 30u
#define TUI_SETTINGS_DEFAULT_ANNOUNCE_MINUTES 360u

typedef struct {
    char display_name[LXMF_DISPLAY_NAME_MAX + 1u];
    size_t display_name_len;
    bool has_stamp_cost;
    uint8_t stamp_cost;
    bool announce_at_start;
    uint32_t announce_interval_minutes;
    bool has_propagation_node;
    uint8_t propagation_node[LXMF_DESTINATION_LENGTH];
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
 * are returned and found is false. Malformed or unreadable files return false.
 */
bool tui_settings_load(const char *path, tui_settings_t *settings, bool *found);

/* Writes through path.tmp, fsyncs it, renames it, then syncs the parent. */
bool tui_settings_save(const char *path, const tui_settings_t *settings);

#endif
