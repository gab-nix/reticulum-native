#include "tui_interfaces.h"

#include <stdio.h>
#include <string.h>

static void clip_line(char line[TUI_INTERFACE_LINE_MAX], size_t width) {
    size_t limit = width;
    if (limit >= TUI_INTERFACE_LINE_MAX) limit = TUI_INTERFACE_LINE_MAX - 1u;
    line[limit] = '\0';
}

void tui_interfaces_init(tui_interfaces_model_t *model) {
    if (model != NULL) memset(model, 0, sizeof *model);
}

void tui_interfaces_update(tui_interfaces_model_t *model,
                           const rns_runtime_interface_info_t *items,
                           size_t count) {
    if (model == NULL || (items == NULL && count != 0u)) return;
    size_t old_index = model->selected_index;
    uint64_t old_id = model->selected_id;
    bool had_selection = model->has_selection;
    if (count > TUI_INTERFACE_CAPACITY) count = TUI_INTERFACE_CAPACITY;
    if (count != 0u) memcpy(model->items, items, count * sizeof items[0]);
    model->count = count;
    if (count == 0u) {
        model->selected_index = 0u;
        model->selected_id = 0u;
        model->has_selection = false;
        return;
    }
    size_t selected = count;
    if (had_selection) {
        for (size_t i = 0u; i < count; ++i) {
            if (model->items[i].id == old_id) {
                selected = i;
                break;
            }
        }
    }
    if (selected == count) selected = old_index < count ? old_index : count - 1u;
    model->selected_index = selected;
    model->selected_id = model->items[selected].id;
    model->has_selection = true;
}

void tui_interfaces_move(tui_interfaces_model_t *model, int delta) {
    if (model == NULL || model->count == 0u) return;
    size_t selected = model->selected_index < model->count
                          ? model->selected_index : 0u;
    if (delta < 0) {
        size_t amount = (size_t)(-(int64_t)delta);
        selected = amount > selected ? 0u : selected - amount;
    } else if (delta > 0) {
        size_t amount = (size_t)delta;
        selected = amount >= model->count - selected
                       ? model->count - 1u : selected + amount;
    }
    model->selected_index = selected;
    model->selected_id = model->items[selected].id;
    model->has_selection = true;
}

size_t tui_interfaces_first(const tui_interfaces_model_t *model,
                            size_t visible_items) {
    if (model == NULL || model->count == 0u || visible_items == 0u) return 0u;
    size_t selected = model->selected_index < model->count
                          ? model->selected_index : 0u;
    return selected >= visible_items ? selected - visible_items + 1u : 0u;
}

const char *tui_interfaces_state_name(rns_runtime_interface_state_t state) {
    switch (state) {
        case RNS_RUNTIME_INTERFACE_DISABLED: return "disabled";
        case RNS_RUNTIME_INTERFACE_STARTING: return "starting";
        case RNS_RUNTIME_INTERFACE_UP: return "up";
        case RNS_RUNTIME_INTERFACE_DOWN: return "down";
        case RNS_RUNTIME_INTERFACE_UNSUPPORTED: return "unsupported";
    }
    return "unknown";
}

void tui_interfaces_format(const rns_runtime_interface_info_t *info,
                           size_t width,
                           char lines[3][TUI_INTERFACE_LINE_MAX]) {
    if (lines == NULL) return;
    memset(lines, 0, 3u * TUI_INTERFACE_LINE_MAX);
    if (info == NULL || width == 0u) return;
    if (width < 72u) {
        (void)snprintf(lines[0], TUI_INTERFACE_LINE_MAX,
                       "%.12s %.8s %.3s e:%.7s", info->name,
                       rns_config_interface_type_name(info->type),
                       tui_interfaces_state_name(info->state),
                       rns_status_string(info->last_error));
        (void)snprintf(lines[1], TUI_INTERFACE_LINE_MAX,
                       "P:%llu/%llu B:%llu/%llu D:%llu",
                       (unsigned long long)info->packets_received,
                       (unsigned long long)info->packets_sent,
                       (unsigned long long)info->bytes_received,
                       (unsigned long long)info->bytes_sent,
                       (unsigned long long)info->packets_dropped);
        (void)snprintf(lines[2], TUI_INTERFACE_LINE_MAX,
                       "C:%llu/%llu/%llu Peer:%llu",
                       (unsigned long long)info->connection_attempts,
                       (unsigned long long)info->connections_established,
                       (unsigned long long)info->connections_lost,
                       (unsigned long long)info->peers);
    } else {
        (void)snprintf(lines[0], TUI_INTERFACE_LINE_MAX,
                       "%s  %s  %s  error:%s", info->name,
                       rns_config_interface_type_name(info->type),
                       tui_interfaces_state_name(info->state),
                       rns_status_string(info->last_error));
        (void)snprintf(lines[1], TUI_INTERFACE_LINE_MAX,
                       "packets rx:%llu tx:%llu  bytes rx:%llu tx:%llu  drop:%llu",
                       (unsigned long long)info->packets_received,
                       (unsigned long long)info->packets_sent,
                       (unsigned long long)info->bytes_received,
                       (unsigned long long)info->bytes_sent,
                       (unsigned long long)info->packets_dropped);
        (void)snprintf(lines[2], TUI_INTERFACE_LINE_MAX,
                       "connections attempts:%llu established:%llu lost:%llu  peers:%llu",
                       (unsigned long long)info->connection_attempts,
                       (unsigned long long)info->connections_established,
                       (unsigned long long)info->connections_lost,
                       (unsigned long long)info->peers);
    }
    for (size_t i = 0u; i < 3u; ++i) clip_line(lines[i], width);
}
