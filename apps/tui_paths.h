#ifndef RETICULUM_TUI_PATHS_H
#define RETICULUM_TUI_PATHS_H

#include "reticulum/runtime.h"

#include <stddef.h>

typedef enum {
    TUI_PATHS_MISSING,
    TUI_PATHS_LOADED,
    TUI_PATHS_INVALID,
    TUI_PATHS_IO_ERROR
} tui_paths_load_result_t;

/* Application-owned persistence for the library's portable path snapshot.
 * Files are bounded, checksummed by libreticulum and never partially applied. */
tui_paths_load_result_t tui_paths_load(rns_runtime_t *runtime,
                                       const char *path,
                                       size_t *loaded_count);
bool tui_paths_save(const rns_runtime_t *runtime, const char *path,
                    size_t *saved_count);

#endif
