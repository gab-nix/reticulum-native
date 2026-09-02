#ifndef NOMAD_TUI_RENDER_H
#define NOMAD_TUI_RENDER_H

#include "tui_state.h"

#include <stdio.h>

/* Draws the current screen. The caller refreshes filters first. */
void tui_render_draw(const tui_state_t *state);
/*
 * Writes the deterministic headless snapshot. It never touches curses, the
 * terminal size, stdin or the store, so it stays usable under TERM=dumb.
 */
int tui_render_dump(const tui_state_t *state, FILE *output);

#endif
