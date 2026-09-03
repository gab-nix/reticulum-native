#ifndef NOMAD_TUI_RENDER_H
#define NOMAD_TUI_RENDER_H

#include "tui_state.h"

#include <stdio.h>

/* Draws the current screen. The caller refreshes filters first. */
void tui_render_draw(tui_state_t *state);
/*
 * Writes the deterministic headless snapshot. It never touches curses, the
 * terminal size, stdin or the store, so it stays usable under TERM=dumb.
 */
int tui_render_dump(const tui_state_t *state, FILE *output);
/* Pure row-window seam used to keep the selected RRC action on small screens. */
size_t tui_render_rrc_first_item(tui_rrc_item_t selected, size_t visible_rows);

#endif
