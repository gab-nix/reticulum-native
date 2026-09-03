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
    assert(strstr(output, "stored; sync pending") != NULL);
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
    state->field = TUI_FIELD_COMPOSE;
    assert(tui_editor_insert_byte(&state->composer, 'a'));
    assert(tui_dispatch_key(state, '\n'));
    assert(strstr(state->status, "Queued locally") != NULL);
    assert(strstr(state->status, "failed") == NULL);
    assert(state->message_count == 1u);
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

int main(void) {
    test_keys_dump_and_persistence();
    test_corruption_warning_survives_startup();
    test_submit_preserves_pending_status();
    return 0;
}
