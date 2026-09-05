#include "tui.h"
#include "tui_render.h"
#include "tui_state.h"
#include "reticulum/udp.h"
#include "reticulum/destination.h"
#include "reticulum/hal.h"
#include "reticulum/lxmf_propagation.h"

#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <curses.h>

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
    assert(tui_state_save_settings(state));
    state->propagation_sync.state = LXMF_PN_COMPLETE;
    state->propagation_sync.result = LXMF_ERR_SIGNATURE;
    state->propagation_sync.available = 3u;
    state->propagation_sync.received = 3u;
    state->propagation_sync.accepted = 2u;
    state->propagation_sync.rejected = 1u;
    state->propagation_sync.acknowledged = 2u;

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
    assert(strstr(output, "Propagation sync: complete active=no") != NULL);
    assert(strstr(output, "accepted=2") != NULL);
    assert(strstr(output, "rejected=1") != NULL);
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

    rns_runtime_interface_info_t runtime_items[2] = {0};
    runtime_items[0].id = 4u;
    memcpy(runtime_items[0].name, "radio", 6u);
    runtime_items[0].type = RNS_CONFIG_RNODE;
    runtime_items[0].state = RNS_RUNTIME_INTERFACE_UP;
    runtime_items[0].packets_received = 8u;
    runtime_items[0].bytes_received = 512u;
    runtime_items[0].packets_sent = 7u;
    runtime_items[0].bytes_sent = 448u;
    runtime_items[0].packets_dropped = 1u;
    runtime_items[0].connection_attempts = 3u;
    runtime_items[0].connections_established = 2u;
    runtime_items[0].connections_lost = 1u;
    runtime_items[0].peers = 6u;
    runtime_items[1].id = 9u;
    memcpy(runtime_items[1].name, "uplink", 7u);
    runtime_items[1].type = RNS_CONFIG_TCP_CLIENT;
    runtime_items[1].state = RNS_RUNTIME_INTERFACE_DOWN;
    runtime_items[1].last_error = RNS_ERROR_IO;
    tui_interfaces_update(&state->interfaces, runtime_items, 2u);
    tui_interfaces_move(&state->interfaces, 1);
    dump = tmpfile();
    assert(dump != NULL && tui_render_dump(state, dump) == 0);
    assert(fseek(dump, 0L, SEEK_SET) == 0);
    memset(output, 0, sizeof output);
    length = fread(output, 1u, sizeof output - 1u, dump);
    assert(!ferror(dump));
    output[length] = '\0';
    assert(strstr(output, "Interfaces: 2") != NULL);
    assert(strstr(output, "radio type=RNodeInterface state=up rx=8/512") != NULL);
    assert(strstr(output, "dropped=1 error=success") != NULL);
    assert(strstr(output,
                  "connections attempts=3 established=2 lost=1 peers=6 id=4") != NULL);
    assert(strstr(output, "> uplink type=TCPClientInterface state=down") != NULL);
    assert(fclose(dump) == 0);

    tui_state_set_status(state, "first diagnostic");
    tui_state_set_status(state, "second diagnostic");
    state->screen = TUI_SCREEN_LOGS;
    dump = tmpfile();
    assert(dump != NULL);
    assert(tui_render_dump(state, dump) == 0);
    assert(fseek(dump, 0, SEEK_SET) == 0);
    length = fread(output, 1u, sizeof output - 1u, dump);
    assert(!ferror(dump));
    output[length] = '\0';
    assert(strstr(output, "Screen: Logs") != NULL);
    assert(strstr(output, "first diagnostic") != NULL);
    assert(strstr(output, "second diagnostic") != NULL);
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
    assert(strstr(output, "Enter actions") != NULL);
    assert(fclose(dump) == 0);

    tui_settings_t persisted;
    bool found = false;
    assert(tui_settings_load(path, &persisted, &found) && found);
    assert(strcmp(persisted.display_name, "Rei") == 0);
    assert(!persisted.announce_at_start);
    assert(persisted.has_propagation_node);
    tui_state_t *restarted = make_state(path);
    restarted->settings = persisted;
    assert(restarted->propagation_sync.state == LXMF_PN_IDLE);
    assert(!restarted->propagation_sync.active);
    destroy_state(restarted);
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
    remove_sidecar(store_path, ".paths");
}

static void delivery_event(void *context, const lxmf_router_event_t *event) {
    /* Even a synchronous initial send callback must find its UI message;
     * otherwise later insertion can overwrite progress with QUEUED. */
    tui_state_t *state = context;
    bool cached = false;
    for (size_t i = 0u; i < state->message_count; ++i)
        if (memcmp(state->messages[i].value.message_id, event->message_id,
                   LXMF_MESSAGE_ID_LENGTH) == 0) cached = true;
    assert(cached);
    tui_state_apply_router_event(context, event);
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
    state->last_compose_timestamp_ms = UINT64_MAX;
    assert(tui_dispatch_key(state, '\n'));
    assert(strstr(state->status, "buffer bounds") != NULL);
    assert(strstr(state->status, "draft kept") != NULL);
    assert(strcmp(tui_editor_text(&state->composer), "a") == 0);
    assert(state->message_count == 0u);
    state->last_compose_timestamp_ms = 0u;
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
    rns_config_t config; rns_config_init(&config);
    assert(rns_runtime_create(&state->runtime, &config, NULL) == RNS_OK);
    lxmf_router_config_t router_config = {.identity = &state->identity,
        .store = &state->store, .runtime = state->runtime,
        .resolve_identity = tui_state_resolve_peer, .resolve_context = state,
        .event_callback = delivery_event, .event_context = state};
    assert(lxmf_router_init(&state->router, &router_config) == LXMF_OK);
    state->router_ready = true;
    state->field = TUI_FIELD_COMPOSE;
    assert(tui_editor_insert_byte(&state->composer, 'b'));
    assert(tui_dispatch_key(state, '\n'));
    assert(strstr(state->status, "waiting for peer identity") != NULL);
    assert(strstr(state->status, "failed") == NULL);
    assert(state->message_count == 2u);
    assert(tui_editor_empty(&state->composer));
    lxmf_router_destroy(&state->router);
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
    remove_sidecar(store_path, ".paths");
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
    remove_sidecar(store_path, ".paths");
    remove_sidecar(store_path, ".tickets");
    remove_sidecar(store_path, ".ratchets");
}

static void test_host_controls(void) {
    char root[] = "/tmp/nomad-host-ui-XXXXXX";
    assert(mkdtemp(root) != NULL);
    char page[512], settings_path[512];
    (void)snprintf(page, sizeof page, "%s/index.mu", root);
    (void)snprintf(settings_path, sizeof settings_path, "%s/settings", root);
    FILE *file = fopen(page, "wb");
    assert(file != NULL && fputs("Synthetic page", file) >= 0 && fclose(file) == 0);
    tui_state_t *state = make_state(settings_path);
    assert(!state->settings.host_enabled && state->host.node == NULL);
    assert(tui_dispatch_key(state, 'o') && state->screen == TUI_SCREEN_NODE);
    assert(tui_dispatch_key(state, '\n') && state->field == TUI_FIELD_HOST);
    assert(tui_editor_insert(&state->host_editor, "relative", 8u));
    assert(!tui_state_host_apply(state));
    assert(state->settings.host_pages_root[0] == 0);
    tui_editor_clear(&state->host_editor);
    assert(tui_editor_insert(&state->host_editor, root, strlen(root)));
    assert(tui_state_host_apply(state));
    assert(tui_dispatch_key(state, 'j') && state->host_selected == TUI_HOST_PAGES);
    assert(tui_dispatch_key(state, '\n') && state->field == TUI_FIELD_HOST);
    assert(tui_dispatch_key(state, 27) && state->field == TUI_FIELD_NONE);
    state->host_selected = TUI_HOST_TOGGLE;
    tui_state_host_activate(state); /* Offline failure preserves saved settings. */
    assert(state->host.node == NULL && !state->settings.host_enabled);
    rns_config_t config; rns_config_init(&config);
    assert(rns_runtime_create(&state->runtime, &config, NULL) == RNS_OK);
    assert(rns_identity_generate(&state->identity));
    tui_state_host_activate(state);
    if (state->host.node == NULL) fprintf(stderr, "Host test failure: %s\n", state->status);
    assert(state->host.node != NULL && state->settings.host_enabled);
    assert(rns_hosted_node_page_count(state->host.node) == 1u);
    assert(state->host.announces == 0u);
    tui_host_poll(&state->host, &state->settings, 1000u);
    assert(state->host.error != RNS_OK && state->host.next_announce_ms == 31000u);
    tui_host_poll(&state->host, &state->settings, 2000u);
    assert(state->host.next_announce_ms == 31000u);
    tui_host_poll(&state->host, &state->settings, 31000u);
    assert(state->host.next_announce_ms == 61000u);
    tui_settings_t loaded;
    assert(tui_settings_load(settings_path, &loaded, NULL) && loaded.host_enabled);
    assert(strcmp(loaded.host_pages_root, root) == 0);
    state->host_selected = TUI_HOST_ROOT;
    tui_state_host_activate(state);
    assert(state->field == TUI_FIELD_NONE); /* Active root cannot change. */
    file = tmpfile(); assert(file != NULL);
    assert(tui_render_dump(state, file) == 0 && fseek(file, 0, SEEK_SET) == 0);
    char output[2048] = {0};
    assert(fread(output, 1, sizeof output - 1u, file) > 0 && fclose(file) == 0);
    assert(strstr(output, "Screen: Node") && strstr(output, "Hosting: active"));
    state->host_selected = TUI_HOST_TOGGLE;
    tui_state_host_activate(state);
    assert(state->host.node == NULL && !state->settings.host_enabled);
    assert(tui_settings_load(settings_path, &loaded, NULL) && !loaded.host_enabled);
    assert(chmod(page, 0700) == 0);
    tui_state_host_activate(state);
    assert(state->host.node == NULL && state->host.error == RNS_ERROR_UNSUPPORTED);
    assert(chmod(page, 0600) == 0);
    memcpy(state->settings.host_pages, "index.mu;../secret", sizeof "index.mu;../secret");
    tui_state_host_activate(state);
    assert(state->host.node == NULL); /* Partial registration rolled back. */
    memcpy(state->settings.host_pages, "index.mu", sizeof "index.mu");
    char saved_path[TUI_SETTINGS_PATH_MAX + 1u];
    strcpy(saved_path, state->settings_path);
    (void)snprintf(state->settings_path, sizeof state->settings_path, "%s/absent/settings", root);
    tui_state_host_activate(state);
    assert(state->host.node == NULL && !state->settings.host_enabled);
    strcpy(state->settings_path, saved_path);
    tui_state_host_activate(state);
    assert(state->host.node != NULL);
    tui_host_stop(&state->host);
    rns_runtime_destroy(state->runtime);
    destroy_state(state);
    assert(unlink(page) == 0 && unlink(settings_path) == 0 && rmdir(root) == 0);
}

static void test_host_startup_and_announces(void) {
    char root[] = "/tmp/nomad-host-startup-XXXXXX";
    assert(mkdtemp(root) != NULL);
    char identity[512], store[512], config_path[512], page[512];
    char settings_path[sizeof store + sizeof ".settings"];
    (void)snprintf(identity, sizeof identity, "%s/identity", root);
    (void)snprintf(store, sizeof store, "%s/messages", root);
    (void)snprintf(config_path, sizeof config_path, "%s/network", root);
    int settings_length = snprintf(settings_path, sizeof settings_path, "%s.settings", store);
    assert(settings_length > 0 && (size_t)settings_length < sizeof settings_path);
    (void)snprintf(page, sizeof page, "%s/index.mu", root);
    FILE *identity_file = fopen(identity, "wb");
    assert(identity_file != NULL && fclose(identity_file) == 0);
    write_identity(identity);
    FILE *file = fopen(page, "wb"); assert(file != NULL && fputs("Synthetic", file) >= 0 && fclose(file) == 0);
    rns_udp_endpoint_t *reservation = NULL; rns_udp_address_t address;
    assert(rns_udp_endpoint_create(&reservation, RNS_UDP_IPV4) == RNS_OK);
    assert(rns_udp_bind(reservation, "127.0.0.1", 0u) == RNS_OK);
    assert(rns_udp_local_address(reservation, &address) == RNS_OK);
    rns_udp_endpoint_destroy(reservation);
    file = fopen(config_path, "wb"); assert(file != NULL);
    assert(fprintf(file, "[reticulum]\nshare_instance = No\n[interfaces]\n[[loopback]]\ntype = UDPInterface\nenabled = True\nlisten_ip = 127.0.0.1\nlisten_port = %u\nforward_ip = 127.0.0.1\nforward_port = %u\n",
        (unsigned)address.port, (unsigned)address.port) > 0 && fclose(file) == 0);
    tui_settings_t settings; tui_settings_defaults(&settings);
    strcpy(settings.host_pages_root, root); settings.host_enabled = true;
    assert(tui_settings_save(settings_path, &settings));
    tui_state_t *state = calloc(1u, sizeof *state); assert(state != NULL);
    assert(tui_state_open(state, identity, store, NULL, config_path) == 0);
    if (state->host.node == NULL) fprintf(stderr, "Startup test: %s; config: %s; enabled=%d\n",
        state->status, state->config_diagnostic.message, state->settings.host_enabled);
    assert(state->host.node != NULL && state->host.announces == 0u);
    tui_host_poll(&state->host, &state->settings, 1000u);
    assert(state->host.announces == 1u && state->host.error == RNS_OK);
    uint64_t next = state->host.next_announce_ms;
    assert(next == 1000u + tui_settings_interval_ms(&state->settings));
    tui_host_poll(&state->host, &state->settings, next - 1u);
    assert(state->host.announces == 1u);
    tui_host_poll(&state->host, &state->settings, next);
    assert(state->host.announces == 2u);
    state->screen = TUI_SCREEN_NODE; state->host_selected = TUI_HOST_TOGGLE;
    tui_state_host_activate(state);
    assert(state->host.node == NULL && !state->settings.host_enabled);
    tui_state_close(state); free(state);
    state = calloc(1u, sizeof *state); assert(state != NULL);
    assert(tui_state_open(state, identity, store, NULL, config_path) == 0);
    assert(state->host.node == NULL && !state->settings.host_enabled);
    tui_state_close(state); free(state);
    assert(unlink(identity) == 0 && unlink(store) == 0 && unlink(config_path) == 0 && unlink(page) == 0);
    const char *sidecars[] = {".settings", ".peers", ".nodes", ".paths", ".tickets", ".ratchets"};
    for (size_t i = 0; i < sizeof sidecars / sizeof sidecars[0]; ++i) remove_sidecar(store, sidecars[i]);
    assert(rmdir(root) == 0);
}

static void test_propagation_restart_without_reannounce(void) {
    char root[] = "/tmp/nomad-pn-restart-XXXXXX";
    assert(mkdtemp(root) != NULL);
    char identity[512], store[512], config_path[512];
    (void)snprintf(identity, sizeof identity, "%s/identity", root);
    (void)snprintf(store, sizeof store, "%s/messages", root);
    (void)snprintf(config_path, sizeof config_path, "%s/network", root);
    FILE *file = fopen(identity, "wb");
    assert(file != NULL && fclose(file) == 0);
    write_identity(identity);
    rns_udp_endpoint_t *reservation = NULL; rns_udp_address_t address;
    assert(rns_udp_endpoint_create(&reservation, RNS_UDP_IPV4) == RNS_OK);
    assert(rns_udp_bind(reservation, "127.0.0.1", 0u) == RNS_OK);
    assert(rns_udp_local_address(reservation, &address) == RNS_OK);
    rns_udp_endpoint_destroy(reservation);
    file = fopen(config_path, "wb"); assert(file != NULL);
    assert(fprintf(file, "[reticulum]\nshare_instance = No\n[interfaces]\n[[loopback]]\ntype = UDPInterface\nenabled = True\nlisten_ip = 127.0.0.1\nlisten_port = %u\nforward_ip = 127.0.0.1\nforward_port = %u\n",
        (unsigned)address.port, (unsigned)address.port) > 0 && fclose(file) == 0);
    tui_state_t *state = calloc(1u, sizeof *state); assert(state != NULL);
    assert(tui_state_open(state, identity, store, NULL, config_path) == 0);
    rns_identity peer; assert(rns_identity_generate(&peer));
    const char *aspects[] = {"propagation"}; uint8_t destination[16];
    assert(rns_destination_hash(&peer, "lxmf", aspects, 1u, destination));
    lxmf_pn_announce_t pn = {.enabled = true, .stamp_cost = 9u};
    uint8_t data[256]; size_t length;
    assert(lxmf_pn_announce_encode(&pn, data, sizeof data, &length) == LXMF_OK);
    assert(rns_runtime_announce(state->runtime, &peer, "lxmf", aspects, 1u, data, length) == RNS_OK);
    uint64_t start, now; assert(rns_hal_monotonic_ms(&start) == RNS_OK);
    const rns_node_record *node;
    do {
        size_t processed; assert(rns_runtime_poll(state->runtime, 8u, &processed) == RNS_OK);
        node = rns_node_registry_get(&state->nodes, destination);
        assert(rns_hal_monotonic_ms(&now) == RNS_OK);
    } while (node == NULL && now - start < 1000u);
    assert(node != NULL && tui_state_use_propagation_node(state, node));
    /* Force old registry monotonic time, as on a different boot. The imported
     * path, not this timestamp, controls the cached selected-node lifetime. */
    state->nodes.records[0].expires_at = 1.0;
    tui_state_close(state); free(state);
    state = calloc(1u, sizeof *state); assert(state != NULL);
    assert(tui_state_open(state, identity, store, NULL, config_path) == 0);
    assert(state->router_ready && state->router.config.propagation_node_identity != NULL);
    assert(tui_state_propagation_state(state, NULL, NULL) == TUI_PROPAGATION_READY);
    node = rns_node_registry_get(&state->nodes, destination);
    assert(node != NULL && !node->reachable);
    assert(rns_hal_monotonic_ms(&now) == RNS_OK);
    assert(node->expires_at > (double)now / 1000.0);
    tui_state_poll(state);
    assert(tui_state_propagation_state(state, NULL, NULL) == TUI_PROPAGATION_READY);
    assert(tui_state_propagation_sync_start(state));
    assert(state->propagation_sync.active);
    assert(tui_state_propagation_sync_cancel(state));
    tui_state_close(state); free(state);
    assert(unlink(identity) == 0 && unlink(store) == 0 && unlink(config_path) == 0);
    const char *sidecars[] = {".settings", ".peers", ".nodes", ".paths", ".tickets", ".ratchets"};
    for (size_t i = 0u; i < sizeof sidecars / sizeof sidecars[0]; ++i) remove_sidecar(store, sidecars[i]);
    assert(rmdir(root) == 0);
}

static void test_host_narrow_render(void) {
    FILE *input = tmpfile(), *output = tmpfile();
    assert(input != NULL && output != NULL);
    SCREEN *screen = newterm("xterm", output, input);
    assert(screen != NULL);
    assert(resizeterm(10, 38) == OK);
    tui_state_t *state = make_state("unused-render-only");
    state->screen = TUI_SCREEN_NODE;
    for (int selected = 0; selected < (int)TUI_HOST_COUNT; ++selected) {
        state->host_selected = (tui_host_item_t)selected;
        tui_render_draw(state);
        int row = selected == 0 ? 4 : 5;
        assert((mvinch(row, 1) & A_REVERSE) != 0u);
    }
    state->host_selected = TUI_HOST_ROOT;
    tui_editor_init(&state->host_editor, 512u);
    const char *long_path = "/a/very/long/path/with/a/visible/editable/suffix";
    assert(tui_editor_insert(&state->host_editor, long_path, strlen(long_path)));
    state->field = TUI_FIELD_HOST;
    tui_render_draw(state);
    int y, x; getyx(stdscr, y, x);
    assert(y == 7 && x == 37);
    char line[39];
    assert(mvinnstr(7, 0, line, 38) != ERR);
    assert(strstr(line, "suffix") != NULL);
    state->screen = TUI_SCREEN_NETWORK;
    assert(tui_dispatch_key(state, KEY_RESIZE));
    assert(tui_dispatch_key(state, 'j'));
    assert(state->field == TUI_FIELD_NONE);
    assert(strcmp(tui_editor_text(&state->host_editor), long_path) == 0);
    destroy_state(state);
    (void)endwin(); delscreen(screen);
    assert(fclose(input) == 0 && fclose(output) == 0);
}

int main(void) {
    test_propagation_restart_without_reannounce();
    test_host_startup_and_announces();
    test_host_narrow_render();
    test_host_controls();
    test_keys_dump_and_persistence();
    test_corruption_warning_survives_startup();
    test_submit_preserves_pending_status();
    test_rrc_settings_and_draft_restart();
    return 0;
}
