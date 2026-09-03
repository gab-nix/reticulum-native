#ifndef NOMAD_TUI_INTERFACES_H
#define NOMAD_TUI_INTERFACES_H

#include "reticulum/runtime.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Runtime may append one shared-instance interface to configured interfaces. */
#define TUI_INTERFACE_CAPACITY (RNS_CONFIG_MAX_INTERFACES + 1u)
#define TUI_INTERFACE_LINE_MAX 256u

typedef struct tui_interfaces_model {
    rns_runtime_interface_info_t items[TUI_INTERFACE_CAPACITY];
    size_t count;
    uint64_t selected_id;
    size_t selected_index;
    bool has_selection;
} tui_interfaces_model_t;

void tui_interfaces_init(tui_interfaces_model_t *model);
/* Copies a bounded public-runtime snapshot and preserves selection by id. */
void tui_interfaces_update(tui_interfaces_model_t *model,
                           const rns_runtime_interface_info_t *items,
                           size_t count);
void tui_interfaces_move(tui_interfaces_model_t *model, int delta);
/* First visible item for a selection-following viewport. */
size_t tui_interfaces_first(const tui_interfaces_model_t *model,
                            size_t visible_items);
const char *tui_interfaces_state_name(rns_runtime_interface_state_t state);
/* Formats three independently clipped, NUL-terminated terminal-safe lines. */
void tui_interfaces_format(const rns_runtime_interface_info_t *info,
                           size_t width,
                           char lines[3][TUI_INTERFACE_LINE_MAX]);

#endif
