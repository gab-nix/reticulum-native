#include "tui_editor.h"

#include "tui_text.h"

bool tui_editor_view(const tui_editor_t *editor, size_t columns,
    size_t *byte_offset, size_t *cursor_column) {
    if (editor == NULL || byte_offset == NULL || cursor_column == NULL || columns == 0u)
        return false;
    size_t cursor = tui_editor_column(editor);
    size_t offset = 0u, skipped = 0u;
    while (offset < editor->cursor && cursor - skipped >= columns) {
        size_t length = tui_utf8_length((const uint8_t *)editor->text + offset, editor->length - offset);
        if (length == 0u) return false;
        skipped += tui_text_cell_width(editor->text + offset, editor->length - offset);
        offset += length;
    }
    while (offset < editor->cursor && tui_text_cell_width(editor->text + offset,
             editor->length - offset) == 0u)
        offset += tui_utf8_length((const uint8_t *)editor->text + offset, editor->length - offset);
    *byte_offset = offset; *cursor_column = cursor - skipped;
    return true;
}

#include <ctype.h>
#include <string.h>

static size_t lead_width(uint8_t lead) {
    if (lead >= 0xc2u && lead <= 0xdfu) return 2u;
    if (lead >= 0xe0u && lead <= 0xefu) return 3u;
    if (lead >= 0xf0u && lead <= 0xf4u) return 4u;
    return 0u;
}

/* Start of the UTF-8 sequence that ends immediately before offset. */
static size_t sequence_start(const tui_editor_t *editor, size_t offset) {
    size_t start = offset;
    while (start > 0u) {
        --start;
        if (((uint8_t)editor->text[start] & 0xc0u) != 0x80u) break;
    }
    return start;
}

static void erase(tui_editor_t *editor, size_t start, size_t end) {
    memmove(editor->text + start, editor->text + end, editor->length - end + 1u);
    editor->length -= end - start;
    editor->cursor = start;
}

void tui_editor_init(tui_editor_t *editor, size_t capacity) {
    if (editor == NULL) return;
    memset(editor, 0, sizeof *editor);
    editor->capacity = capacity < TUI_EDITOR_MAX ? capacity : TUI_EDITOR_MAX;
}

void tui_editor_clear(tui_editor_t *editor) {
    if (editor == NULL) return;
    editor->text[0] = '\0';
    editor->length = 0u;
    editor->cursor = 0u;
    editor->pending_length = 0u;
}

const char *tui_editor_text(const tui_editor_t *editor) {
    return editor != NULL ? editor->text : "";
}

size_t tui_editor_length(const tui_editor_t *editor) {
    return editor != NULL ? editor->length : 0u;
}

size_t tui_editor_cursor(const tui_editor_t *editor) {
    return editor != NULL ? editor->cursor : 0u;
}

size_t tui_editor_column(const tui_editor_t *editor) {
    if (editor == NULL) return 0u;
    size_t column = 0u;
    for (size_t at = 0u; at < editor->cursor;) {
        size_t n = tui_utf8_length((const uint8_t *)editor->text + at, editor->length - at);
        column += tui_text_cell_width(editor->text + at, editor->length - at);
        at += n != 0u ? n : 1u;
    }
    return column;
}

bool tui_editor_empty(const tui_editor_t *editor) {
    return editor == NULL || editor->length == 0u;
}

bool tui_editor_insert(tui_editor_t *editor, const char *utf8, size_t length) {
    if (editor == NULL || (utf8 == NULL && length != 0u)) return false;
    if (length == 0u) return true;
    if (editor->length + length > editor->capacity) return false;
    if (!tui_utf8_valid((const uint8_t *)utf8, length)) return false;
    memmove(editor->text + editor->cursor + length, editor->text + editor->cursor,
            editor->length - editor->cursor + 1u);
    memcpy(editor->text + editor->cursor, utf8, length);
    editor->cursor += length;
    editor->length += length;
    return true;
}

bool tui_editor_insert_byte(tui_editor_t *editor, unsigned char byte) {
    if (editor == NULL) return false;
    if (editor->pending_length != 0u) {
        if ((byte & 0xc0u) != 0x80u) {
            /* A new lead byte abandons the truncated sequence in progress. */
            editor->pending_length = 0u;
            return tui_editor_insert_byte(editor, byte);
        }
        editor->pending[editor->pending_length++] = byte;
        if (editor->pending_length < lead_width(editor->pending[0])) return true;
        size_t complete = editor->pending_length;
        editor->pending_length = 0u;
        return tui_editor_insert(editor, (const char *)editor->pending, complete);
    }
    if (byte >= 0x20u && byte < 0x7fu) {
        char value = (char)byte;
        return tui_editor_insert(editor, &value, 1u);
    }
    if (lead_width(byte) == 0u) return false;
    editor->pending[0] = byte;
    editor->pending_length = 1u;
    return true;
}

bool tui_editor_apply(tui_editor_t *editor, tui_edit_command_t command) {
    if (editor == NULL) return false;
    editor->pending_length = 0u;
    switch (command) {
        case TUI_EDIT_BACKSPACE:
            if (editor->cursor == 0u) return false;
            erase(editor, sequence_start(editor, editor->cursor), editor->cursor);
            return true;
        case TUI_EDIT_DELETE: {
            if (editor->cursor >= editor->length) return false;
            size_t width = tui_utf8_length((const uint8_t *)editor->text + editor->cursor,
                                           editor->length - editor->cursor);
            erase(editor, editor->cursor, editor->cursor + (width != 0u ? width : 1u));
            return true;
        }
        case TUI_EDIT_LEFT:
            if (editor->cursor == 0u) return false;
            editor->cursor = sequence_start(editor, editor->cursor);
            return true;
        case TUI_EDIT_RIGHT: {
            if (editor->cursor >= editor->length) return false;
            size_t width = tui_utf8_length((const uint8_t *)editor->text + editor->cursor,
                                           editor->length - editor->cursor);
            editor->cursor += width != 0u ? width : 1u;
            return true;
        }
        case TUI_EDIT_HOME:
            editor->cursor = 0u;
            return true;
        case TUI_EDIT_END:
            editor->cursor = editor->length;
            return true;
        case TUI_EDIT_KILL_TO_START:
            if (editor->cursor == 0u) return false;
            erase(editor, 0u, editor->cursor);
            return true;
        case TUI_EDIT_KILL_TO_END:
            if (editor->cursor >= editor->length) return false;
            editor->text[editor->cursor] = '\0';
            editor->length = editor->cursor;
            return true;
        case TUI_EDIT_KILL_WORD: {
            size_t start = editor->cursor;
            while (start > 0u && isspace((unsigned char)editor->text[start - 1u])) --start;
            while (start > 0u && !isspace((unsigned char)editor->text[start - 1u])) --start;
            if (start == editor->cursor) return false;
            erase(editor, start, editor->cursor);
            return true;
        }
    }
    return false;
}

void tui_editor_position(const tui_editor_t *editor, size_t columns,
                         size_t *row, size_t *column) {
    if (row == NULL || column == NULL) return;
    *row = 0u; *column = 0u;
    if (editor == NULL || columns == 0u) return;
    size_t offset = 0u, start, bytes;
    while (tui_text_wrap_next(editor->text, editor->length, columns,
                              &offset, &start, &bytes)) {
        bool last = offset == editor->length &&
                    (bytes == 0u || editor->text[offset - 1u] != '\n');
        if (editor->cursor < offset || (last && editor->cursor == offset)) {
            for (size_t i = start; i < editor->cursor && i < start + bytes;) {
                size_t n = tui_utf8_length((const uint8_t *)editor->text + i,
                                           editor->length - i);
                *column += tui_text_cell_width(editor->text + i, editor->length - i);
                i += n != 0u ? n : 1u;
            }
            if (*column >= columns) { ++*row; *column = 0u; }
            return;
        }
        ++*row;
    }
}

bool tui_editor_move_vertical(tui_editor_t *editor, size_t columns, int delta) {
    if (editor == NULL || columns == 0u || delta == 0) return false;
    size_t row, column;
    tui_editor_position(editor, columns, &row, &column);
    if (delta < 0 && row == 0u) return false;
    size_t target = delta < 0 ? row - 1u : row + 1u;
    size_t offset = 0u, start = editor->length, bytes = 0u, current = 0u;
    while (tui_text_wrap_next(editor->text, editor->length, columns,
                              &offset, &start, &bytes)) {
        if (current++ == target) {
            size_t at = start, cells = 0u;
            while (at < start + bytes) {
                size_t n = tui_utf8_length((const uint8_t *)editor->text + at,
                                           editor->length - at);
                size_t w = tui_text_cell_width(editor->text + at, editor->length - at);
                if (cells + w > column) break;
                cells += w; at += n != 0u ? n : 1u;
            }
            editor->cursor = at; editor->pending_length = 0u;
            size_t actual_row, actual_column;
            tui_editor_position(editor, columns, &actual_row, &actual_column);
            if (actual_row > target && at > start)
                editor->cursor = sequence_start(editor, at);
            return true;
        }
    }
    if (delta > 0) { editor->cursor = editor->length; return true; }
    return false;
}
