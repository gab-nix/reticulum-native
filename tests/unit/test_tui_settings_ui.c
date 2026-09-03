#include "tui.h"
#include "tui_render.h"
#include "tui_state.h"

#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static tui_state_t *make_state(const char *settings_path) {
    tui_state_t *state = calloc(1u, sizeof *state);
    assert(state != NULL);
    state->messages = calloc(TUI_MAX_MESSAGES, sizeof *state->messages);
    assert(state->messages != NULL);
    tui_editor_init(&state->composer, TUI_COMPOSER_CAPACITY);
    tui_editor_init(&state->search, TUI_SEARCH_CAPACITY);
    tui_editor_init(&state->node_search, TUI_SEARCH_CAPACITY);
    tui_editor_init(&state->address, TUI_ADDRESS_DIGITS);
    tui_editor_init(&state->setting, LXMF_DISPLAY_NAME_MAX);
    tui_settings_defaults(&state->settings);
    assert(snprintf(state->settings_path, sizeof state->settings_path, "%s",
                    settings_path) > 0);
    return state;
}

static void destroy_state(tui_state_t *state) {
    free(state->messages);
    free(state);
}

static void test_keys_dump_and_persistence(void) {
    char path[] = "/tmp/nomad-settings-ui-XXXXXX";
    int descriptor = mkstemp(path);
    assert(descriptor >= 0);
    assert(close(descriptor) == 0);
    assert(unlink(path) == 0);
    tui_state_t *state = make_state(path);

    assert(tui_dispatch_key(state, 'S'));
    assert(state->screen == TUI_SCREEN_SETTINGS);
    assert(tui_dispatch_key(state, '\n'));
    assert(state->field == TUI_FIELD_SETTING);
    assert(tui_dispatch_key(state, 21));
    assert(tui_dispatch_key(state, 'R'));
    assert(tui_dispatch_key(state, 'e'));
    assert(tui_dispatch_key(state, 'i'));
    assert(tui_dispatch_key(state, '\n'));
    assert(state->field == TUI_FIELD_NONE);
    assert(strcmp(state->settings.display_name, "Rei") == 0);

    state->setting_selected = TUI_SETTING_ANNOUNCE_AT_START;
    assert(tui_dispatch_key(state, '\n'));
    assert(!state->settings.announce_at_start);
    state->settings.has_stamp_cost = true;
    state->settings.stamp_cost = 8u;
    state->settings.has_propagation_node = true;
    memset(state->settings.propagation_node, 0x42,
           sizeof state->settings.propagation_node);

    FILE *dump = tmpfile();
    assert(dump != NULL);
    assert(tui_render_dump(state, dump) == 0);
    assert(fseek(dump, 0L, SEEK_SET) == 0);
    char output[2048] = {0};
    size_t length = fread(output, 1u, sizeof output - 1u, dump);
    assert(!ferror(dump));
    output[length] = '\0';
    assert(strstr(output, "Screen: Settings") != NULL);
    assert(strstr(output, "Display name: Rei") != NULL);
    assert(strstr(output, "enforced and advertised") != NULL);
    assert(strstr(output, "waiting for verified announce") != NULL);
    assert(strstr(output, "sync unavailable") != NULL);
    assert(fclose(dump) == 0);

    state->screen = TUI_SCREEN_INTERFACES;
    dump = tmpfile();
    assert(dump != NULL && tui_render_dump(state, dump) == 0);
    assert(fseek(dump, 0L, SEEK_SET) == 0);
    memset(output, 0, sizeof output);
    length = fread(output, 1u, sizeof output - 1u, dump);
    assert(!ferror(dump));
    output[length] = '\0';
    assert(strstr(output, "Screen: Interfaces") != NULL);
    assert(strstr(output, "Interfaces: 0") != NULL);
    assert(fclose(dump) == 0);

    state->screen = TUI_SCREEN_CONFIG;
    dump = tmpfile();
    assert(dump != NULL && tui_render_dump(state, dump) == 0);
    assert(fseek(dump, 0L, SEEK_SET) == 0);
    memset(output, 0, sizeof output);
    length = fread(output, 1u, sizeof output - 1u, dump);
    assert(!ferror(dump));
    output[length] = '\0';
    assert(strstr(output, "Validation: not loaded") != NULL);
    assert(fclose(dump) == 0);

    state->config_attempted = true;
    state->config_valid = true;
    assert(snprintf(state->config_path, sizeof state->config_path,
                    "%s", "/tmp/reticulum.conf") > 0);
    rns_config_init(&state->parsed_config);
    state->parsed_config.enable_transport = true;
    state->parsed_config.interface_count = 1u;
    rns_config_interface_t *interface = &state->parsed_config.interfaces[0];
    memcpy(interface->name, "uplink", 7u);
    interface->type = RNS_CONFIG_TCP_CLIENT;
    interface->type_set = true;
    interface->enabled = true;
    memcpy(interface->target_host, "127.0.0.1", 10u);
    interface->target_port = 4242u;
    dump = tmpfile();
    assert(dump != NULL && tui_render_dump(state, dump) == 0);
    assert(fseek(dump, 0L, SEEK_SET) == 0);
    memset(output, 0, sizeof output);
    length = fread(output, 1u, sizeof output - 1u, dump);
    assert(!ferror(dump));
    output[length] = '\0';
    assert(strstr(output, "Screen: Config") != NULL);
    assert(strstr(output, "Validation: valid") != NULL);
    assert(strstr(output, "uplink  TCP client  enabled  127.0.0.1:4242") != NULL);
    assert(fclose(dump) == 0);

    state->config_valid = false;
    state->config_diagnostic.line = 7u;
    memcpy(state->config_diagnostic.message, "invalid port", 13u);
    dump = tmpfile();
    assert(dump != NULL && tui_render_dump(state, dump) == 0);
    assert(fseek(dump, 0L, SEEK_SET) == 0);
    memset(output, 0, sizeof output);
    length = fread(output, 1u, sizeof output - 1u, dump);
    assert(!ferror(dump));
    output[length] = '\0';
    assert(strstr(output, "Validation: invalid") != NULL);
    assert(strstr(output, "line 7: invalid port") != NULL);
    assert(fclose(dump) == 0);

    state->screen = TUI_SCREEN_GUIDE;
    dump = tmpfile();
    assert(dump != NULL && tui_render_dump(state, dump) == 0);
    assert(fseek(dump, 0L, SEEK_SET) == 0);
    memset(output, 0, sizeof output);
    length = fread(output, 1u, sizeof output - 1u, dump);
    assert(!ferror(dump));
    output[length] = '\0';
    assert(strstr(output, "Screen: Guide") != NULL);
    assert(strstr(output, "Enter node actions") != NULL);
    assert(fclose(dump) == 0);

    tui_settings_t persisted;
    bool found = false;
    assert(tui_settings_load(path, &persisted, &found) && found);
    assert(strcmp(persisted.display_name, "Rei") == 0);
    assert(!persisted.announce_at_start);
    assert(unlink(path) == 0);
    destroy_state(state);
}

static void write_identity(const char *path) {
    rns_identity identity;
    uint8_t private_key[RNS_IDENTITY_PRIVATE_SIZE];
    assert(rns_identity_generate(&identity));
    assert(rns_identity_export_private(&identity, private_key));
    int descriptor = open(path, O_WRONLY | O_TRUNC);
    assert(descriptor >= 0);
    assert(write(descriptor, private_key, sizeof private_key) ==
           (ssize_t)sizeof private_key);
    assert(close(descriptor) == 0);
}

static void remove_sidecar(const char *base, const char *suffix) {
    char path[TUI_PATH_MAX + 16u];
    int written = snprintf(path, sizeof path, "%s%s", base, suffix);
    assert(written > 0 && (size_t)written < sizeof path);
    (void)unlink(path);
}

static void test_corruption_warning_survives_startup(void) {
    char identity_path[] = "/tmp/nomad-settings-identity-XXXXXX";
    char store_path[] = "/tmp/nomad-settings-store-XXXXXX";
    int identity_fd = mkstemp(identity_path);
    int store_fd = mkstemp(store_path);
    assert(identity_fd >= 0 && store_fd >= 0);
    assert(close(identity_fd) == 0 && close(store_fd) == 0);
    write_identity(identity_path);

    char settings_path[TUI_PATH_MAX + 16u];
    assert(snprintf(settings_path, sizeof settings_path, "%s.settings",
                    store_path) > 0);
    int descriptor = open(settings_path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    assert(descriptor >= 0);
    assert(write(descriptor, "corrupt", 7u) == 7);
    assert(close(descriptor) == 0);

    tui_state_t state;
    assert(tui_state_open(&state, identity_path, store_path, NULL, NULL) == 0);
    assert(state.settings_load_error);
    assert(strstr(state.status, "invalid") != NULL);
    tui_state_close(&state);

    assert(unlink(identity_path) == 0);
    assert(unlink(store_path) == 0);
    remove_sidecar(store_path, ".settings");
    remove_sidecar(store_path, ".peers");
    remove_sidecar(store_path, ".nodes");
}

static void test_submit_preserves_pending_status(void) {
    char identity_path[] = "/tmp/nomad-submit-identity-XXXXXX";
    char store_path[] = "/tmp/nomad-submit-store-XXXXXX";
    int identity_fd = mkstemp(identity_path);
    int store_fd = mkstemp(store_path);
    assert(identity_fd >= 0 && store_fd >= 0);
    assert(close(identity_fd) == 0 && close(store_fd) == 0);
    write_identity(identity_path);
    tui_state_t *state = calloc(1u, sizeof *state);
    assert(state != NULL);
    assert(tui_state_open(state, identity_path, store_path, NULL, NULL) == 0);
    uint8_t destination[LXMF_DESTINATION_LENGTH];
    memset(destination, 0x42, sizeof destination);
    assert(tui_state_open_conversation(state, destination));
    tui_state_toggle_delivery_method(state);
    assert(state->compose_delivery_method == LXMF_DELIVERY_METHOD_PROPAGATED);
    state->field = TUI_FIELD_COMPOSE;
    assert(tui_editor_insert_byte(&state->composer, 'a'));
    assert(tui_dispatch_key(state, '\n'));
    assert(strstr(state->status, "Queued locally") != NULL);
    assert(strstr(state->status, "failed") == NULL);
    assert(state->message_count == 1u);
    assert(state->messages[0].value.delivery.desired_method ==
           LXMF_DELIVERY_METHOD_PROPAGATED);
    tui_state_toggle_delivery_method(state);
    lxmf_peer_t peer;
    assert(lxmf_peer_store_get(&state->peer_store, destination, &peer) == LXMF_OK);
    assert(peer.draft_len == 0u);

    /* Exercise the pending identity/path branch without opening sockets. */
    state->router_ready = true;
    state->field = TUI_FIELD_COMPOSE;
    assert(tui_editor_insert_byte(&state->composer, 'b'));
    assert(tui_dispatch_key(state, '\n'));
    assert(strstr(state->status, "requesting a verified path") != NULL);
    assert(strstr(state->status, "failed") == NULL);
    assert(state->message_count == 2u);
    assert(tui_editor_empty(&state->composer));
    state->router_ready = false;
    assert(tui_dispatch_key(state, '\n'));
    assert(state->field == TUI_FIELD_COMPOSE);
    const char *draft = "unfinished";
    for (size_t i = 0u; draft[i] != '\0'; ++i)
        assert(tui_dispatch_key(state, (unsigned char)draft[i]));
    assert(tui_dispatch_key(state, 27));
    tui_state_close(state);
    free(state);

    state = calloc(1u, sizeof *state);
    assert(state != NULL);
    assert(tui_state_open(state, identity_path, store_path,
                          "42424242424242424242424242424242", NULL) == 0);
    assert(strcmp(tui_editor_text(&state->composer), "unfinished") == 0);
    tui_state_close(state);
    free(state);
    assert(unlink(identity_path) == 0);
    assert(unlink(store_path) == 0);
    remove_sidecar(store_path, ".settings");
    remove_sidecar(store_path, ".peers");
    remove_sidecar(store_path, ".nodes");
    remove_sidecar(store_path, ".tickets");
    remove_sidecar(store_path, ".ratchets");
}

static void replace_rrc_value(tui_state_t *state, tui_rrc_item_t item,
                              const char *value, bool submit) {
    state->rrc.selected = item;
    tui_state_rrc_activate(state, 0u);
    assert(state->field == TUI_FIELD_RRC);
    assert(tui_dispatch_key(state, 21));
    for (size_t i = 0u; value[i] != '\0'; ++i)
        assert(tui_dispatch_key(state, (unsigned char)value[i]));
    if (submit) {
        assert(tui_dispatch_key(state, '\n'));
        assert(state->field == TUI_FIELD_NONE);
    }
}

static void test_rrc_settings_and_draft_restart(void) {
    static const char address[] = "00112233445566778899aabbccddeeff";
    char public_identity[TUI_RRC_PUBLIC_IDENTITY_HEX + 1u];
    memset(public_identity, 'b', sizeof public_identity - 1u);
    public_identity[sizeof public_identity - 1u] = '\0';
    char identity_path[] = "/tmp/nomad-rrc-settings-identity-XXXXXX";
    char store_path[] = "/tmp/nomad-rrc-settings-store-XXXXXX";
    int identity_fd = mkstemp(identity_path);
    int store_fd = mkstemp(store_path);
    assert(identity_fd >= 0 && store_fd >= 0);
    assert(close(identity_fd) == 0 && close(store_fd) == 0);
    write_identity(identity_path);

    tui_state_t *state = calloc(1u, sizeof *state);
    assert(state != NULL &&
           tui_state_open(state, identity_path, store_path, NULL, NULL) == 0);
    state->screen = TUI_SCREEN_RRC;
    replace_rrc_value(state, TUI_RRC_ITEM_HUB_ADDRESS, address, true);
    replace_rrc_value(state, TUI_RRC_ITEM_HUB_IDENTITY, public_identity, true);
    replace_rrc_value(state, TUI_RRC_ITEM_NICK, "Rei", true);
    replace_rrc_value(state, TUI_RRC_ITEM_ROOM, "lobby", true);
    replace_rrc_value(state, TUI_RRC_ITEM_MESSAGE, "unfinished", false);
    assert(tui_dispatch_key(state, 27));
    assert(strcmp(state->rrc.outgoing, "unfinished") == 0);
    state->rrc.selected = TUI_RRC_ITEM_RECONNECT;
    tui_state_rrc_activate(state, 0u);
    assert(!state->rrc.auto_reconnect);
    tui_state_close(state);
    free(state);

    state = calloc(1u, sizeof *state);
    assert(state != NULL &&
           tui_state_open(state, identity_path, store_path, NULL, NULL) == 0);
    assert(strcmp(state->rrc.hub_address, address) == 0);
    assert(strcmp(state->rrc.hub_identity, public_identity) == 0);
    assert(strcmp(state->rrc.nick, "Rei") == 0);
    assert(strcmp(state->rrc.room, "lobby") == 0);
    assert(strcmp(state->rrc.outgoing, "unfinished") == 0);
    assert(!state->rrc.auto_reconnect);

    /* An invalid submitted address leaves both live and durable state intact. */
    state->screen = TUI_SCREEN_RRC;
    replace_rrc_value(state, TUI_RRC_ITEM_HUB_ADDRESS, "abcd", false);
    assert(tui_dispatch_key(state, '\n'));
    assert(state->field == TUI_FIELD_RRC);
    assert(strcmp(state->rrc.hub_address, address) == 0);
    assert(tui_dispatch_key(state, 27));
    tui_state_close(state);
    free(state);

    state = calloc(1u, sizeof *state);
    assert(state != NULL &&
           tui_state_open(state, identity_path, store_path, NULL, NULL) == 0);
    assert(strcmp(state->rrc.hub_address, address) == 0);
    assert(strcmp(state->rrc.outgoing, "unfinished") == 0);
    tui_state_close(state);
    free(state);

    assert(unlink(identity_path) == 0);
    assert(unlink(store_path) == 0);
    remove_sidecar(store_path, ".settings");
    remove_sidecar(store_path, ".peers");
    remove_sidecar(store_path, ".nodes");
    remove_sidecar(store_path, ".tickets");
    remove_sidecar(store_path, ".ratchets");
}

int main(void) {
    test_keys_dump_and_persistence();
    test_corruption_warning_survives_startup();
    test_submit_preserves_pending_status();
    test_rrc_settings_and_draft_restart();
    return 0;
}
