#ifndef NOMAD_CHAT_TUI_H
#define NOMAD_CHAT_TUI_H

#include <stdio.h>

int nomad_tui_run(const char *identity_path, const char *store_path);
int nomad_tui_run_destination(const char *identity_path, const char *store_path,
                              const char *destination_hex);
int nomad_tui_run_config(const char *config_path, const char *identity_path,
                         const char *store_path, const char *destination_hex);
int nomad_tui_dump(const char *identity_path, const char *store_path,
                   const char *destination_hex, FILE *output);

#endif
