#ifndef RETICULUM_TUI_MODEL_H
#define RETICULUM_TUI_MODEL_H

#include "reticulum/lxmf_store.h"

#define TUI_MODEL_MAX_CONVERSATIONS 64u
#define TUI_MODEL_MAX_MESSAGES 256u
#define TUI_MODEL_MAX_CONTENT 512u
#define TUI_MODEL_COMPOSER_CAPACITY 1024u

typedef struct {
    uint8_t peer[LXMF_DESTINATION_LENGTH];
    double latest_timestamp;
    lxmf_delivery_status_t latest_status;
    size_t message_count;
    size_t unread_count;
} tui_conversation_t;

typedef struct {
    uint8_t message_id[LXMF_MESSAGE_ID_LENGTH];
    uint8_t peer[LXMF_DESTINATION_LENGTH];
    double timestamp;
    lxmf_delivery_status_t status;
    bool outgoing;
    bool unread;
    size_t content_len;
    uint8_t content[TUI_MODEL_MAX_CONTENT];
} tui_message_t;

typedef struct {
    uint8_t local[LXMF_DESTINATION_LENGTH];
    tui_conversation_t conversations[TUI_MODEL_MAX_CONVERSATIONS];
    tui_message_t messages[TUI_MODEL_MAX_MESSAGES];
    size_t conversation_count;
    size_t message_count;
    size_t selected;
    size_t scroll;
    char composer[TUI_MODEL_COMPOSER_CAPACITY + 1u];
    size_t composer_len;
    size_t composer_cursor;
} tui_model_t;

typedef lxmf_status_t (*tui_model_queue_fn)(
    void *context, const uint8_t peer[LXMF_DESTINATION_LENGTH],
    const uint8_t *content, size_t content_len,
    lxmf_store_message_t *queued_message);

void tui_model_init(tui_model_t *model,
                    const uint8_t local[LXMF_DESTINATION_LENGTH]);
lxmf_status_t tui_model_load(tui_model_t *model, lxmf_store_t *store);
size_t tui_model_conversation_count(const tui_model_t *model);
const tui_conversation_t *tui_model_conversation(const tui_model_t *model,
                                                  size_t index);
bool tui_model_select(tui_model_t *model, size_t index);
size_t tui_model_selected(const tui_model_t *model);
bool tui_model_seed_conversation(
    tui_model_t *model, const uint8_t peer[LXMF_DESTINATION_LENGTH]);
const uint8_t *tui_model_selected_peer(const tui_model_t *model);
void tui_model_mark_selected_read(tui_model_t *model);
size_t tui_model_selected_message_count(const tui_model_t *model);
const tui_message_t *tui_model_selected_message(const tui_model_t *model,
                                                size_t index);
void tui_model_set_scroll(tui_model_t *model, size_t scroll);
size_t tui_model_scroll(const tui_model_t *model);

const char *tui_model_composer(const tui_model_t *model);
size_t tui_model_composer_len(const tui_model_t *model);
size_t tui_model_composer_cursor(const tui_model_t *model);
bool tui_model_composer_insert(tui_model_t *model, const char *utf8, size_t len);
bool tui_model_composer_backspace(tui_model_t *model);
bool tui_model_composer_left(tui_model_t *model);
bool tui_model_composer_right(tui_model_t *model);
void tui_model_composer_clear(tui_model_t *model);
lxmf_status_t tui_model_submit(tui_model_t *model, tui_model_queue_fn queue,
                               void *context);

#endif
