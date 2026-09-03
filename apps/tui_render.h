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
/* Bounded single-line projection used by curses and headless tests. */
void tui_render_message_metadata(const tui_message_metadata_t *metadata,
                                 char *output, size_t capacity);
/* NULL means the node satisfies the unchanged verified-selection policy. */
const char *tui_render_node_propagation_reason(const rns_node_record *node);
/* Pure row-window seam used to keep the selected RRC action on small screens. */
size_t tui_render_rrc_first_item(tui_rrc_item_t selected, size_t visible_rows);

#endif
