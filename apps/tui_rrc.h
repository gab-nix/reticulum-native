#ifndef NOMAD_TUI_RRC_H
#define NOMAD_TUI_RRC_H

#include "reticulum/rrc_session.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define TUI_RRC_HUB_ADDRESS_HEX 32u
#define TUI_RRC_PUBLIC_IDENTITY_HEX 128u
#define TUI_RRC_MAX_MESSAGES 64u
#define TUI_RRC_STATUS_MAX 160u

typedef enum tui_rrc_item {
    TUI_RRC_ITEM_HUB_ADDRESS = 0,
    TUI_RRC_ITEM_HUB_IDENTITY,
    TUI_RRC_ITEM_NICK,
    TUI_RRC_ITEM_CONNECT,
    TUI_RRC_ITEM_ROOM,
    TUI_RRC_ITEM_JOIN,
    TUI_RRC_ITEM_PART,
    TUI_RRC_ITEM_MESSAGE,
    TUI_RRC_ITEM_SEND,
    TUI_RRC_ITEM_COUNT
} tui_rrc_item_t;

typedef struct tui_rrc_message {
    rns_rrc_message_type_t type;
    uint64_t timestamp_ms;
    uint8_t source[RNS_RRC_SOURCE_SIZE];
    char room[RNS_RRC_DEFAULT_MAX_ROOM_BYTES + 1u];
    char nick[RNS_RRC_DEFAULT_MAX_NICK_BYTES + 1u];
    char body[RNS_RRC_DEFAULT_MAX_MESSAGE_BYTES + 1u];
} tui_rrc_message_t;

typedef struct tui_rrc_model {
    rns_rrc_session_t *session;
    rns_rrc_session_info_t info;
    char hub_address[TUI_RRC_HUB_ADDRESS_HEX + 1u];
    char hub_identity[TUI_RRC_PUBLIC_IDENTITY_HEX + 1u];
    char nick[RNS_RRC_DEFAULT_MAX_NICK_BYTES + 1u];
    char room[RNS_RRC_DEFAULT_MAX_ROOM_BYTES + 1u];
    char outgoing[RNS_RRC_DEFAULT_MAX_MESSAGE_BYTES + 1u];
    tui_rrc_message_t messages[TUI_RRC_MAX_MESSAGES];
    size_t message_count;
    tui_rrc_item_t selected;
    char status[TUI_RRC_STATUS_MAX];
} tui_rrc_model_t;

void tui_rrc_init(tui_rrc_model_t *model);
void tui_rrc_close(tui_rrc_model_t *model);
void tui_rrc_move(tui_rrc_model_t *model, int delta);
size_t tui_rrc_edit_capacity(tui_rrc_item_t item);
const char *tui_rrc_edit_value(const tui_rrc_model_t *model,
                               tui_rrc_item_t item);
bool tui_rrc_edit_apply(tui_rrc_model_t *model, tui_rrc_item_t item,
                        const char *value, size_t value_length);
bool tui_rrc_connect_toggle(tui_rrc_model_t *model, rns_runtime_t *runtime,
                            const rns_identity *local_identity,
                            uint64_t now_ms);
bool tui_rrc_join(tui_rrc_model_t *model);
bool tui_rrc_part(tui_rrc_model_t *model);
bool tui_rrc_send(tui_rrc_model_t *model);
void tui_rrc_poll(tui_rrc_model_t *model, uint64_t now_ms);

/* Pure callback seam used by deterministic app tests. */
void tui_rrc_apply_envelope(tui_rrc_model_t *model,
                            const rns_rrc_envelope_t *envelope);

#endif
