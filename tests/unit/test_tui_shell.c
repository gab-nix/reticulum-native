#include "tui_shell.h"

#include <assert.h>
#include <limits.h>
#include <string.h>

int main(void) {
    nomad_tui_shell_t shell;
    unsigned overlay = 0;
    nomad_tui_shell_init(&shell);
    assert(shell.active == NOMAD_SCREEN_CONVERSATIONS);
    assert(strcmp(nomad_tui_screen_name(shell.active), "Conversations") == 0);
    nomad_tui_shell_next(&shell);
    assert(shell.active == NOMAD_SCREEN_NETWORK);
    nomad_tui_shell_previous(&shell);
    assert(shell.active == NOMAD_SCREEN_CONVERSATIONS);
    nomad_tui_shell_previous(&shell);
    assert(shell.active == NOMAD_SCREEN_GUIDE);
    assert(shell.previous == NOMAD_SCREEN_CONVERSATIONS);
    assert(!nomad_tui_shell_select(&shell, NOMAD_SCREEN_COUNT));

    nomad_tui_shell_set_unread(&shell, NOMAD_SCREEN_CONVERSATIONS, 3);
    nomad_tui_shell_set_unread(&shell, NOMAD_SCREEN_CHANNELS, 2);
    assert(nomad_tui_shell_total_unread(&shell) == 5);
    nomad_tui_shell_set_unread(&shell, NOMAD_SCREEN_LOG, UINT_MAX);
    assert(nomad_tui_shell_total_unread(&shell) == UINT_MAX);

    for (unsigned i = 1; i <= NOMAD_OVERLAY_DEPTH; ++i)
        assert(nomad_tui_shell_push_overlay(&shell, i));
    assert(!nomad_tui_shell_push_overlay(&shell, 99));
    assert(nomad_tui_shell_has_overlay(&shell));
    for (unsigned i = NOMAD_OVERLAY_DEPTH; i > 0; --i) {
        assert(nomad_tui_shell_pop_overlay(&shell, &overlay));
        assert(overlay == i);
    }
    assert(!nomad_tui_shell_pop_overlay(&shell, &overlay));
    return 0;
}
