#ifndef NOMAD_TUI_STATE_H
#define NOMAD_TUI_STATE_H

#include "tui_editor.h"
#include "tui_rrc.h"
#include "tui_settings.h"

#include "reticulum/browser.h"
#include "reticulum/identity.h"
#include "reticulum/lxmf_peer_store.h"
#include "reticulum/lxmf_router.h"
#include "reticulum/lxmf_store.h"
#include "reticulum/micron.h"
#include "reticulum/node_registry.h"
#include "reticulum/runtime.h"
#include "reticulum/ratchet_store.h"

#define TUI_MAX_CONTACTS 64u
#define TUI_MAX_MESSAGES 256u
#define TUI_NOTE_MAX 48u
#define TUI_STATUS_MAX 160u
#define TUI_PATH_MAX 1024u
#define TUI_ADDRESS_DIGITS 32u
#define TUI_COMPOSER_CAPACITY 1024u
#define TUI_SEARCH_CAPACITY 80u

typedef enum {
    TUI_TRUST_TRUSTED,
    TUI_TRUST_UNKNOWN,
    TUI_TRUST_UNTRUSTED
} tui_trust_t;

typedef enum {
    TUI_SCREEN_CONVERSATIONS,
    TUI_SCREEN_NETWORK,
    TUI_SCREEN_BROWSER,
    TUI_SCREEN_NODE,
    TUI_SCREEN_SETTINGS,
    TUI_SCREEN_GUIDE,
    TUI_SCREEN_LOGS,
    TUI_SCREEN_RRC,
    TUI_SCREEN_INTERFACES,
    TUI_SCREEN_CONFIG,
    TUI_SCREEN_COUNT
} tui_screen_t;

typedef enum {
    TUI_FIELD_NONE,
    TUI_FIELD_COMPOSE,
    TUI_FIELD_SEARCH,
    TUI_FIELD_NODE_SEARCH,
    TUI_FIELD_ADDRESS,
    TUI_FIELD_SETTING,
    TUI_FIELD_RRC
} tui_field_t;

typedef enum {
    TUI_SETTING_DISPLAY_NAME,
    TUI_SETTING_STAMP_COST,
    TUI_SETTING_ANNOUNCE_AT_START,
    TUI_SETTING_ANNOUNCE_INTERVAL,
    TUI_SETTING_PROPAGATION_NODE,
    TUI_SETTING_PROPAGATION_SYNC,
    TUI_SETTING_ANNOUNCE_NOW,
    TUI_SETTING_COUNT
} tui_setting_item_t;

typedef enum {
    TUI_OVERLAY_NONE,
    TUI_OVERLAY_HELP,
    TUI_OVERLAY_PEER,
    TUI_OVERLAY_NODE_ACTIONS
} tui_overlay_t;

typedef enum {
    TUI_PROPAGATION_NOT_SELECTED,
    TUI_PROPAGATION_WAITING_ANNOUNCE,
    TUI_PROPAGATION_STALE,
    TUI_PROPAGATION_DISABLED,
    TUI_PROPAGATION_INVALID_COST,
    TUI_PROPAGATION_READY
} tui_propagation_state_t;

typedef struct {
    uint8_t peer[LXMF_DESTINATION_LENGTH];
    size_t messages;
    size_t unread;
    double latest;
    tui_trust_t trust;
    bool pinned;
    bool blocked;
    char note[TUI_NOTE_MAX];
    char draft[LXMF_PEER_DRAFT_MAX + 1u];
    size_t draft_len;
    bool draft_dirty;
} tui_contact_t;

typedef struct {
    lxmf_store_message_t value;
    uint8_t content[LXMF_STORE_MAX_CONTENT];
} tui_message_t;

/*
 * The whole client state. Filtering results are cached in visible/thread and
 * rebuilt by tui_state_refresh() once per frame, so rendering never rescans
 * the message history per row.
 */
typedef struct tui_state {
    rns_identity identity;
    uint8_t local[LXMF_DESTINATION_LENGTH];

    lxmf_store_t store;
    lxmf_peer_store_t peer_store;
    lxmf_ticket_store_t *ticket_store;
    rns_ratchet_store_t *ratchet_store;
    char peer_store_path[TUI_PATH_MAX];
    char node_store_path[TUI_PATH_MAX];
    char ticket_store_path[TUI_PATH_MAX];
    char ratchet_store_path[TUI_PATH_MAX];

    tui_contact_t contacts[TUI_MAX_CONTACTS];
    size_t contact_count;
    tui_message_t *messages;
    size_t message_count;

    size_t selected;
    size_t scroll;

    size_t visible[TUI_MAX_CONTACTS];
    size_t visible_count;
    size_t thread[TUI_MAX_MESSAGES];
    size_t thread_count;
    bool filter_dirty;

    tui_editor_t composer;
    tui_editor_t search;
    tui_editor_t node_search;
    tui_editor_t address;
    tui_editor_t setting;
    tui_field_t field;
    tui_overlay_t overlay;
    tui_screen_t screen;
    tui_trust_t tab;

    rns_node_registry nodes;
    /*
     * The Network selection is held by destination, not by list position: the
     * registry re-sorts as announces arrive, so an index would silently move
     * the cursor onto a different node.
     */
    uint8_t node_selection[LXMF_DESTINATION_LENGTH];
    bool has_node_selection;
    size_t interface_selected;

    rns_config_t parsed_config;
    rns_config_diagnostic_t config_diagnostic;
    char config_path[TUI_PATH_MAX];
    bool config_attempted;
    bool config_valid;

    rns_runtime_t *runtime;
    lxmf_router_t router;
    bool router_ready;
    uint64_t router_polled_ms;
    uint64_t last_announce_ms;
    uint64_t next_announce_ms;
    bool startup_announce_pending;
    bool has_announce_result;
    rns_status_t last_announce_result;
    rns_identity resolved_identity;
    rns_identity resolved_propagation_identity;
    lxmf_router_propagation_sync_status_t propagation_sync;
    lxmf_delivery_method_t compose_delivery_method;
    bool send_attempted;
    bool send_ok;

    rns_browser_t *browser;
    rns_browser_state_t browser_state;
    rns_micron_page page;
    rns_micron_history history;
    size_t link_selected;
    size_t page_scroll;
    char url[RNS_MICRON_TEXT_MAX];

    tui_settings_t settings;
    char settings_path[TUI_SETTINGS_PATH_MAX + 1u];
    tui_setting_item_t setting_selected;
    bool settings_load_error;

    tui_rrc_model_t rrc;

    char status[TUI_STATUS_MAX];
} tui_state_t;

/* config_path may be NULL, which leaves the client offline. */
int tui_state_open(tui_state_t *state, const char *identity_path,
                   const char *store_path, const char *destination_hex,
                   const char *config_path);
void tui_state_close(tui_state_t *state);
/* True when at least one interface is up and has not reported an error. */
bool tui_state_link_ready(const tui_state_t *state);
/* Performs one round of bounded, non-blocking network and browser work. */
void tui_state_poll(tui_state_t *state);
/* Rebuilds the cached filter results when they are stale. */
void tui_state_refresh(tui_state_t *state);
void tui_state_set_status(tui_state_t *state, const char *format, ...);

const tui_contact_t *tui_state_contact(const tui_state_t *state, size_t index);
const tui_contact_t *tui_state_selected_contact(const tui_state_t *state);
size_t tui_state_thread_count(const tui_state_t *state);
const tui_message_t *tui_state_thread_message(const tui_state_t *state, size_t index);
bool tui_state_outgoing(const tui_state_t *state, const tui_message_t *message);

void tui_state_select_offset(tui_state_t *state, int delta);
void tui_state_set_tab(tui_state_t *state, tui_trust_t tab);
void tui_state_scroll_by(tui_state_t *state, int lines);
/* Selects, creating if needed, the conversation with peer. */
bool tui_state_open_conversation(tui_state_t *state, const uint8_t peer[LXMF_DESTINATION_LENGTH]);
lxmf_status_t tui_state_queue_message(tui_state_t *state);
/* Persists the active composer into the selected conversation. */
void tui_state_save_draft(tui_state_t *state);
void tui_state_persist_contacts(tui_state_t *state);
void tui_state_set_trust(tui_state_t *state, tui_trust_t trust);
void tui_state_toggle_pin(tui_state_t *state);
void tui_state_toggle_block(tui_state_t *state);
/* Reads app-owned preferences; the LXMF router enforces the returned policy. */
bool tui_state_source_blocked(const tui_state_t *state,
                               const uint8_t source[LXMF_SOURCE_LENGTH]);
void tui_state_toggle_note(tui_state_t *state);

size_t tui_state_node_count(const tui_state_t *state);
/* Copies the sorted node list. Returns the number written. */
size_t tui_state_node_list(const tui_state_t *state, rns_node_record *out,
                           size_t capacity);
bool tui_state_selected_node(const tui_state_t *state, rns_node_record *record);
/* Position of the selection in the sorted list, or 0 when there is none. */
size_t tui_state_node_position(const tui_state_t *state);
/* Moves the selection by delta entries, clamped to the list. */
void tui_state_node_move(tui_state_t *state, int delta);
/* Only Nomad Network nodes serve pages. */
bool tui_state_node_serves_pages(const rns_node_record *node);
void tui_state_request_path(tui_state_t *state);

size_t tui_state_interface_count(const tui_state_t *state);
bool tui_state_interface_info(const tui_state_t *state, size_t index,
                              rns_runtime_interface_info_t *info);
void tui_state_interface_move(tui_state_t *state, int delta);

size_t tui_state_link_count(const tui_state_t *state);
const rns_micron_span *tui_state_link(const tui_state_t *state, size_t index);
bool tui_state_browse(tui_state_t *state, const char *url, bool push_history);
void tui_state_browse_selected(tui_state_t *state);
void tui_state_browse_back(tui_state_t *state);
void tui_state_browse_node(tui_state_t *state, const rns_node_record *node);
/* Returns true when an in-flight page request was cancelled. */
bool tui_state_browse_cancel(tui_state_t *state);

void tui_state_setting_move(tui_state_t *state, int delta);
/* Starts editing, toggles a setting, or sends an announcement as applicable. */
void tui_state_setting_activate(tui_state_t *state);
bool tui_state_setting_apply(tui_state_t *state);
void tui_state_setting_cancel(tui_state_t *state);
bool tui_state_save_settings(tui_state_t *state);
bool tui_state_announce(tui_state_t *state);
/* Resolves only costs retained from a verified LXMF delivery announce. */
bool tui_state_peer_stamp_cost(
    const tui_state_t *state,
    const uint8_t destination[LXMF_DESTINATION_LENGTH], uint8_t *cost);
/* Reports whether the saved node has a current verified propagation announce. */
tui_propagation_state_t tui_state_propagation_state(
    const tui_state_t *state, rns_node_record *record, uint8_t *stamp_cost);
/* Explicitly selects a verified enabled Network entry and persists it. */
bool tui_state_use_propagation_node(tui_state_t *state,
                                    const rns_node_record *record);
/* Starts or cancels one explicit list/download/ack transaction. Sync starts
 * only while the saved propagation node has a fresh verified announce. */
bool tui_state_propagation_sync_start(tui_state_t *state);
bool tui_state_propagation_sync_cancel(tui_state_t *state);
/* Pure UI projection of immutable router status, also used by headless tests. */
void tui_state_apply_propagation_sync(
    tui_state_t *state,
    const lxmf_router_propagation_sync_status_t *status);
/* Direct remains the default; propagated messages never silently downgrade. */
void tui_state_toggle_delivery_method(tui_state_t *state);

void tui_state_rrc_move(tui_state_t *state, int delta);
void tui_state_rrc_activate(tui_state_t *state, uint64_t now_ms);
bool tui_state_rrc_apply(tui_state_t *state);
void tui_state_rrc_cancel(tui_state_t *state);
/* Persists the active RRC message editor as a recoverable unsent draft. */
bool tui_state_rrc_update_draft(tui_state_t *state);

/* Pure state updates used by router callbacks and unit tests. */
void tui_state_apply_router_event(tui_state_t *state,
                                  const lxmf_router_event_t *event);
void tui_state_apply_signature(tui_state_t *state,
                               const uint8_t message_id[LXMF_MESSAGE_ID_LENGTH],
                               lxmf_signature_state_t signature);

#endif
