#ifndef NOMAD_TUI_EDITOR_H
#define NOMAD_TUI_EDITOR_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define TUI_EDITOR_MAX 1024u

/*
 * A bounded UTF-8 editor shared by the composer, the search field and
 * the address field. Editing is UTF-8 aware: multi-byte sequences arrive one
 * byte at a time from the terminal, are staged until complete, and are never
 * split by cursor motion or deletion. The buffer is always NUL-terminated.
 */
typedef struct {
    char text[TUI_EDITOR_MAX + 1u];
    size_t capacity;
    size_t length;
    size_t cursor;
    uint8_t pending[4];
    size_t pending_length;
} tui_editor_t;

typedef enum {
    TUI_EDIT_BACKSPACE,
    TUI_EDIT_DELETE,
    TUI_EDIT_LEFT,
    TUI_EDIT_RIGHT,
    TUI_EDIT_HOME,
    TUI_EDIT_END,
    TUI_EDIT_KILL_TO_START,
    TUI_EDIT_KILL_TO_END,
    TUI_EDIT_KILL_WORD
} tui_edit_command_t;

/* capacity is clamped to TUI_EDITOR_MAX. */
void tui_editor_init(tui_editor_t *editor, size_t capacity);
void tui_editor_clear(tui_editor_t *editor);

const char *tui_editor_text(const tui_editor_t *editor);
size_t tui_editor_length(const tui_editor_t *editor);
size_t tui_editor_cursor(const tui_editor_t *editor);
/* Cursor position measured in display columns rather than bytes. */
size_t tui_editor_column(const tui_editor_t *editor);
bool tui_editor_empty(const tui_editor_t *editor);
/* UTF-8-safe horizontal view using terminal cell widths. */
bool tui_editor_view(const tui_editor_t *editor, size_t columns,
    size_t *byte_offset, size_t *cursor_column);

/* Accepts one terminal byte. Control bytes are rejected as editing commands. */
bool tui_editor_insert_byte(tui_editor_t *editor, unsigned char byte);
/* Inserts a complete, valid UTF-8 run at the cursor. */
bool tui_editor_insert(tui_editor_t *editor, const char *utf8, size_t length);
bool tui_editor_apply(tui_editor_t *editor, tui_edit_command_t command);
/* Wrapped cursor projection and vertical movement share the render layout. */
void tui_editor_position(const tui_editor_t *editor, size_t columns,
                         size_t *row, size_t *column);
bool tui_editor_move_vertical(tui_editor_t *editor, size_t columns, int delta);

#endif
