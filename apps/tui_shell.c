#include "tui_shell.h"

#include <limits.h>
#include <string.h>

void nomad_tui_shell_init(nomad_tui_shell_t *shell) {
    if (!shell) return;
    memset(shell, 0, sizeof(*shell));
    shell->active = NOMAD_SCREEN_CONVERSATIONS;
    shell->previous = NOMAD_SCREEN_CONVERSATIONS;
}

bool nomad_tui_shell_select(nomad_tui_shell_t *shell, nomad_screen_t screen) {
    if (!shell || screen < 0 || screen >= NOMAD_SCREEN_COUNT) return false;
    shell->previous = shell->active;
    shell->active = screen;
    shell->menu_focused = false;
    return true;
}

void nomad_tui_shell_next(nomad_tui_shell_t *shell) {
    if (!shell) return;
    (void)nomad_tui_shell_select(shell,
        (nomad_screen_t)(((unsigned)shell->active + 1u) % (unsigned)NOMAD_SCREEN_COUNT));
}

void nomad_tui_shell_previous(nomad_tui_shell_t *shell) {
    if (!shell) return;
    unsigned current = (unsigned)shell->active;
    unsigned previous = current == 0u ? (unsigned)NOMAD_SCREEN_COUNT - 1u : current - 1u;
    (void)nomad_tui_shell_select(shell, (nomad_screen_t)previous);
}

const char *nomad_tui_screen_name(nomad_screen_t screen) {
    static const char *names[NOMAD_SCREEN_COUNT] = {
        "Conversations", "Network", "Channels", "Log", "Interfaces", "Config", "Guide"
    };
    return screen >= 0 && screen < NOMAD_SCREEN_COUNT ? names[screen] : "Unknown";
}

void nomad_tui_shell_set_unread(nomad_tui_shell_t *shell, nomad_screen_t screen,
                                unsigned unread) {
    if (shell && screen >= 0 && screen < NOMAD_SCREEN_COUNT) shell->unread[screen] = unread;
}

unsigned nomad_tui_shell_total_unread(const nomad_tui_shell_t *shell) {
    unsigned total = 0;
    if (!shell) return 0;
    for (size_t i = 0; i < NOMAD_SCREEN_COUNT; ++i) {
        if (UINT_MAX - total < shell->unread[i]) return UINT_MAX;
        total += shell->unread[i];
    }
    return total;
}

bool nomad_tui_shell_push_overlay(nomad_tui_shell_t *shell, unsigned overlay_id) {
    if (!shell || overlay_id == 0u || shell->overlay_count >= NOMAD_OVERLAY_DEPTH) return false;
    shell->overlay_ids[shell->overlay_count++] = overlay_id;
    return true;
}

bool nomad_tui_shell_pop_overlay(nomad_tui_shell_t *shell, unsigned *overlay_id) {
    if (!shell || shell->overlay_count == 0u) return false;
    --shell->overlay_count;
    if (overlay_id) *overlay_id = shell->overlay_ids[shell->overlay_count];
    shell->overlay_ids[shell->overlay_count] = 0u;
    return true;
}

bool nomad_tui_shell_has_overlay(const nomad_tui_shell_t *shell) {
    return shell && shell->overlay_count > 0u;
}
