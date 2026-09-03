#include "tui_rrc.h"

#include <assert.h>
#include <string.h>

static rns_rrc_envelope_t message_envelope(uint64_t timestamp,
                                           const uint8_t *body,
                                           size_t body_length) {
    static const uint8_t room[] = "Lobby";
    static const uint8_t nick[] = "Rei";
    rns_rrc_envelope_t envelope = {0};
    envelope.version = RNS_RRC_VERSION;
    envelope.type = RNS_RRC_MESSAGE;
    envelope.timestamp_ms = timestamp;
    envelope.room = (rns_rrc_slice_t){room, sizeof room - 1u};
    envelope.nick = (rns_rrc_slice_t){nick, sizeof nick - 1u};
    envelope.body_cbor = (rns_rrc_slice_t){body, body_length};
    envelope.source[0] = (uint8_t)timestamp;
    return envelope;
}

int main(void) {
    tui_rrc_model_t model;
    tui_rrc_init(&model);
    assert(model.info.state == RNS_RRC_SESSION_DISCONNECTED);
    assert(model.info.welcome.max_message_bytes == 350u);
    assert(tui_rrc_edit_capacity(TUI_RRC_ITEM_HUB_ADDRESS) == 32u);
    assert(tui_rrc_edit_capacity(TUI_RRC_ITEM_HUB_IDENTITY) == 128u);
    assert(!tui_rrc_edit_apply(&model, TUI_RRC_ITEM_HUB_ADDRESS,
                               "abcd", 4u));
    static const char address[] = "00112233445566778899aabbccddeeff";
    assert(tui_rrc_edit_apply(&model, TUI_RRC_ITEM_HUB_ADDRESS,
                              address, sizeof address - 1u));
    assert(strcmp(model.hub_address, address) == 0);
    assert(tui_rrc_edit_apply(&model, TUI_RRC_ITEM_NICK, "Rei", 3u));
    assert(tui_rrc_edit_apply(&model, TUI_RRC_ITEM_ROOM, "lobby", 5u));
    assert(tui_rrc_edit_apply(&model, TUI_RRC_ITEM_MESSAGE, "hello", 5u));
    assert(!tui_rrc_connect_toggle(&model, NULL, &(rns_identity){0}, 0u));
    assert(strstr(model.status, "online Reticulum") != NULL);

    static const uint8_t body[] = {0x66u, 'h', 'i', '\n', 'x', 0x1bu, '!'};
    rns_rrc_envelope_t envelope = message_envelope(1u, body, sizeof body);
    tui_rrc_apply_envelope(&model, &envelope);
    assert(model.message_count == 1u);
    assert(strcmp(model.messages[0].room, "Lobby") == 0);
    assert(strcmp(model.messages[0].nick, "Rei") == 0);
    assert(strcmp(model.messages[0].body, "hi x !") == 0);

    static const uint8_t malformed[] = {0x61u, 'x', 0x00u};
    envelope = message_envelope(2u, malformed, sizeof malformed);
    tui_rrc_apply_envelope(&model, &envelope);
    assert(model.message_count == 1u);

    static const uint8_t short_body[] = {0x61u, 'x'};
    for (uint64_t i = 2u; i <= TUI_RRC_MAX_MESSAGES + 1u; ++i) {
        envelope = message_envelope(i, short_body, sizeof short_body);
        tui_rrc_apply_envelope(&model, &envelope);
    }
    assert(model.message_count == TUI_RRC_MAX_MESSAGES);
    assert(model.messages[0].timestamp_ms == 2u);
    assert(model.messages[TUI_RRC_MAX_MESSAGES - 1u].timestamp_ms ==
           TUI_RRC_MAX_MESSAGES + 1u);

    (void)strcpy(model.motd, "Welcome to the hub");
    model.room_count = 1u;
    (void)strcpy(model.rooms[0].name, "lobby");
    model.rooms[0].joined = true;
    model.rooms[0].desired = true;
    model.rooms[0].member_count = 3u;

    model.selected = TUI_RRC_ITEM_HUB_ADDRESS;
    tui_rrc_move(&model, -1);
    assert(model.selected == TUI_RRC_ITEM_SEND);
    tui_rrc_move(&model, 1);
    assert(model.selected == TUI_RRC_ITEM_HUB_ADDRESS);
    tui_rrc_close(&model);
    return 0;
}
