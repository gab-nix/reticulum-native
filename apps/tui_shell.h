#ifndef NOMAD_TUI_SHELL_H
#define NOMAD_TUI_SHELL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    NOMAD_SCREEN_CONVERSATIONS = 0,
    NOMAD_SCREEN_NETWORK,
    NOMAD_SCREEN_CHANNELS,
    NOMAD_SCREEN_LOG,
    NOMAD_SCREEN_INTERFACES,
    NOMAD_SCREEN_CONFIG,
    NOMAD_SCREEN_GUIDE,
    NOMAD_SCREEN_COUNT
} nomad_screen_t;

#define NOMAD_OVERLAY_DEPTH 8u

typedef struct {
    nomad_screen_t active;
    nomad_screen_t previous;
    unsigned unread[NOMAD_SCREEN_COUNT];
    unsigned overlay_ids[NOMAD_OVERLAY_DEPTH];
    size_t overlay_count;
    bool menu_focused;
    bool fullscreen;
} nomad_tui_shell_t;

void nomad_tui_shell_init(nomad_tui_shell_t *shell);
bool nomad_tui_shell_select(nomad_tui_shell_t *shell, nomad_screen_t screen);
void nomad_tui_shell_next(nomad_tui_shell_t *shell);
void nomad_tui_shell_previous(nomad_tui_shell_t *shell);
const char *nomad_tui_screen_name(nomad_screen_t screen);
void nomad_tui_shell_set_unread(nomad_tui_shell_t *shell, nomad_screen_t screen,
                                unsigned unread);
unsigned nomad_tui_shell_total_unread(const nomad_tui_shell_t *shell);
bool nomad_tui_shell_push_overlay(nomad_tui_shell_t *shell, unsigned overlay_id);
bool nomad_tui_shell_pop_overlay(nomad_tui_shell_t *shell, unsigned *overlay_id);
bool nomad_tui_shell_has_overlay(const nomad_tui_shell_t *shell);

#endif
