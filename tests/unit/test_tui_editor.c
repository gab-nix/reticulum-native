#include "tui_editor.h"

#include <assert.h>
#include <string.h>

static void insert_text(tui_editor_t *editor, const char *text) {
    for (const char *cursor = text; *cursor != '\0'; ++cursor)
        assert(tui_editor_insert_byte(editor, (unsigned char)*cursor));
}

static void test_ascii_editing(void) {
    tui_editor_t editor;
    tui_editor_init(&editor, 16u);
    assert(tui_editor_empty(&editor));
    insert_text(&editor, "hello world");
    assert(strcmp(tui_editor_text(&editor), "hello world") == 0);
    assert(tui_editor_length(&editor) == 11u);
    assert(tui_editor_cursor(&editor) == 11u);

    assert(tui_editor_apply(&editor, TUI_EDIT_HOME));
    assert(tui_editor_cursor(&editor) == 0u);
    assert(!tui_editor_apply(&editor, TUI_EDIT_BACKSPACE));
    assert(tui_editor_apply(&editor, TUI_EDIT_DELETE));
    assert(strcmp(tui_editor_text(&editor), "ello world") == 0);
    assert(tui_editor_apply(&editor, TUI_EDIT_END));
    assert(tui_editor_apply(&editor, TUI_EDIT_KILL_WORD));
    assert(strcmp(tui_editor_text(&editor), "ello ") == 0);
    assert(tui_editor_apply(&editor, TUI_EDIT_KILL_TO_START));
    assert(tui_editor_empty(&editor));
    assert(!tui_editor_apply(&editor, TUI_EDIT_KILL_TO_END));
}

static void test_capacity_and_control(void) {
    tui_editor_t editor;
    tui_editor_init(&editor, 4u);
    insert_text(&editor, "abcd");
    /* The bound is enforced and the buffer is left untouched. */
    assert(!tui_editor_insert_byte(&editor, 'e'));
    assert(strcmp(tui_editor_text(&editor), "abcd") == 0);
    /* Control bytes are editing commands, never content. */
    assert(!tui_editor_insert_byte(&editor, 0x1bu));
    assert(!tui_editor_insert_byte(&editor, '\n'));
    assert(!tui_editor_insert_byte(&editor, 0x7fu));
    assert(tui_editor_length(&editor) == 4u);
    tui_editor_clear(&editor);
    assert(tui_editor_empty(&editor) && tui_editor_cursor(&editor) == 0u);
}

static void test_utf8_editing(void) {
    tui_editor_t editor;
    tui_editor_init(&editor, 32u);
    /* A multi-byte sequence arrives one terminal byte at a time. */
    assert(tui_editor_insert_byte(&editor, 0xc3u));
    assert(tui_editor_length(&editor) == 0u);
    assert(tui_editor_insert_byte(&editor, 0xa9u));
    assert(tui_editor_length(&editor) == 2u);
    insert_text(&editor, "llo");
    assert(strcmp(tui_editor_text(&editor), "\xc3\xa9llo") == 0);
    assert(tui_editor_column(&editor) == 4u);

    /* Cursor motion and deletion never split a sequence. */
    assert(tui_editor_apply(&editor, TUI_EDIT_HOME));
    assert(tui_editor_apply(&editor, TUI_EDIT_RIGHT));
    assert(tui_editor_cursor(&editor) == 2u);
    assert(tui_editor_apply(&editor, TUI_EDIT_LEFT));
    assert(tui_editor_cursor(&editor) == 0u);
    assert(tui_editor_apply(&editor, TUI_EDIT_RIGHT));
    assert(tui_editor_apply(&editor, TUI_EDIT_BACKSPACE));
    assert(strcmp(tui_editor_text(&editor), "llo") == 0);
}

static void test_invalid_utf8(void) {
    tui_editor_t editor;
    tui_editor_init(&editor, 32u);
    /* Stray continuation bytes and overlong leads are refused outright. */
    assert(!tui_editor_insert_byte(&editor, 0x80u));
    assert(!tui_editor_insert_byte(&editor, 0xc0u));
    assert(!tui_editor_insert_byte(&editor, 0xf5u));
    assert(tui_editor_empty(&editor));
    /* A truncated sequence followed by a printable byte keeps only the byte. */
    assert(tui_editor_insert_byte(&editor, 0xe2u));
    assert(tui_editor_insert_byte(&editor, 'a'));
    assert(strcmp(tui_editor_text(&editor), "a") == 0);
    /* A surrogate encoding completes its length but fails validation. */
    tui_editor_clear(&editor);
    assert(tui_editor_insert_byte(&editor, 0xedu));
    assert(tui_editor_insert_byte(&editor, 0xa0u));
    assert(!tui_editor_insert_byte(&editor, 0x80u));
    assert(tui_editor_empty(&editor));
}

static void test_horizontal_view(void) {
    tui_editor_t editor; tui_editor_init(&editor, 1024u);
    insert_text(&editor, "abcdef\xc3\xa9ghijkl");
    size_t offset, cursor;
    assert(tui_editor_view(&editor, 5u, &offset, &cursor));
    assert(strcmp(tui_editor_text(&editor) + offset, "ijkl") == 0 && cursor == 4u);
    assert(tui_editor_apply(&editor, TUI_EDIT_HOME));
    assert(tui_editor_view(&editor, 5u, &offset, &cursor) && offset == 0u && cursor == 0u);
    assert(!tui_editor_view(&editor, 0u, &offset, &cursor));
    assert(tui_editor_apply(&editor, TUI_EDIT_END));
    assert(tui_editor_view(&editor, 8u, &offset, &cursor));
    assert(tui_editor_text(&editor)[offset] == '\xc3' && cursor == 7u);
}

int main(void) {
    test_horizontal_view();
    test_ascii_editing();
    test_capacity_and_control();
    test_utf8_editing();
    test_invalid_utf8();
    return 0;
}
