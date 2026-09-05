#ifndef NOMAD_TUI_HOST_H
#define NOMAD_TUI_HOST_H
#include "tui_settings.h"
#include "reticulum/hosted_node.h"

typedef struct {
    rns_hosted_node_t *node;
    rns_status_t error;
    uint64_t next_announce_ms;
    size_t announces;
} tui_host_t;

/* App-owned configuration/polling; library owns requests, identity and files. */
rns_status_t tui_host_start(tui_host_t *host, rns_runtime_t *runtime,
    const rns_identity *identity, const tui_settings_t *settings);
void tui_host_stop(tui_host_t *host);
rns_status_t tui_host_announce(tui_host_t *host, const tui_settings_t *settings, uint64_t now);
void tui_host_poll(tui_host_t *host, const tui_settings_t *settings, uint64_t now);
#endif
