#ifndef NOMAD_CHAT_TUI_H
#define NOMAD_CHAT_TUI_H

#include <stdbool.h>
#include <stdio.h>

typedef struct tui_state tui_state_t;

int nomad_tui_run(const char *identity_path, const char *store_path);
int nomad_tui_run_destination(const char *identity_path, const char *store_path,
                              const char *destination_hex);
int nomad_tui_run_config(const char *config_path, const char *identity_path,
                         const char *store_path, const char *destination_hex);
int nomad_tui_dump(const char *identity_path, const char *store_path,
                   const char *destination_hex, FILE *output);

/* App-internal deterministic input seam used by headless UI tests. */
bool tui_dispatch_key(tui_state_t *state, int key);

#endif
