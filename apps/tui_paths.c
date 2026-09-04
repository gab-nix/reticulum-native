#include "tui_paths.h"

#include "reticulum/hal.h"

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define TUI_PATHS_FILE_MAX (64u * 1024u)
#define TUI_PATHS_PATH_MAX 1024u

static bool sync_parent(const char *path) {
    char directory[TUI_PATHS_PATH_MAX + 1u];
    size_t length = strnlen(path, sizeof directory);
    if (length == 0u || length >= sizeof directory) return false;
    memcpy(directory, path, length + 1u);
    char *slash = strrchr(directory, '/');
    if (slash == NULL) memcpy(directory, ".", 2u);
    else if (slash == directory) slash[1] = '\0';
    else *slash = '\0';
    int descriptor = open(directory, O_RDONLY | O_CLOEXEC);
    if (descriptor < 0) return false;
    bool okay = fsync(descriptor) == 0;
    (void)close(descriptor);
    return okay;
}

static bool write_all(int descriptor, const uint8_t *data, size_t length) {
    size_t written = 0u;
    while (written < length) {
        ssize_t count = write(descriptor, data + written, length - written);
        if (count > 0) written += (size_t)count;
        else if (count < 0 && errno == EINTR) continue;
        else return false;
    }
    return true;
}

tui_paths_load_result_t tui_paths_load(rns_runtime_t *runtime,
                                       const char *path,
                                       size_t *loaded_count) {
    if (loaded_count != NULL) *loaded_count = 0u;
    if (runtime == NULL || path == NULL || loaded_count == NULL)
        return TUI_PATHS_IO_ERROR;
    size_t path_length = strnlen(path, TUI_PATHS_PATH_MAX + 1u);
    if (path_length == 0u || path_length > TUI_PATHS_PATH_MAX)
        return TUI_PATHS_IO_ERROR;
    int descriptor = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (descriptor < 0)
        return errno == ENOENT ? TUI_PATHS_MISSING : TUI_PATHS_IO_ERROR;
    struct stat status;
    if (fstat(descriptor, &status) != 0 || !S_ISREG(status.st_mode) ||
        status.st_size <= 0 || (uintmax_t)status.st_size > TUI_PATHS_FILE_MAX) {
        (void)close(descriptor);
        return TUI_PATHS_INVALID;
    }
    size_t length = (size_t)status.st_size;
    uint8_t *data = malloc(length);
    if (data == NULL) {
        (void)close(descriptor);
        return TUI_PATHS_IO_ERROR;
    }
    size_t offset = 0u;
    bool okay = true;
    while (offset < length) {
        ssize_t count = read(descriptor, data + offset, length - offset);
        if (count > 0) offset += (size_t)count;
        else if (count < 0 && errno == EINTR) continue;
        else { okay = false; break; }
    }
    uint8_t trailing;
    ssize_t extra = okay ? read(descriptor, &trailing, 1u) : -1;
    if (close(descriptor) != 0) okay = false;
    if (!okay || extra != 0) {
        free(data);
        return TUI_PATHS_IO_ERROR;
    }
    uint64_t wall_time_ms = 0u;
    rns_status_t result = rns_hal_wallclock_ms(&wall_time_ms);
    if (result == RNS_OK)
        result = rns_runtime_paths_import(runtime, wall_time_ms, data, length,
                                          loaded_count);
    free(data);
    return result == RNS_OK ? TUI_PATHS_LOADED : TUI_PATHS_INVALID;
}

bool tui_paths_save(const rns_runtime_t *runtime, const char *path,
                    size_t *saved_count) {
    if (saved_count != NULL) *saved_count = 0u;
    if (runtime == NULL || path == NULL || saved_count == NULL) return false;
    size_t path_length = strnlen(path, TUI_PATHS_PATH_MAX + 1u);
    if (path_length == 0u || path_length > TUI_PATHS_PATH_MAX) return false;
    uint8_t *data = malloc(TUI_PATHS_FILE_MAX);
    if (data == NULL) return false;
    uint64_t wall_time_ms = 0u;
    size_t length = 0u;
    size_t encoded_count = 0u;
    rns_status_t result = rns_hal_wallclock_ms(&wall_time_ms);
    if (result == RNS_OK)
        result = rns_runtime_paths_export(runtime, wall_time_ms, data,
                                          TUI_PATHS_FILE_MAX, &length,
                                          &encoded_count);
    if (result != RNS_OK || length == 0u || length > TUI_PATHS_FILE_MAX) {
        free(data);
        return false;
    }
    char temporary[TUI_PATHS_PATH_MAX + sizeof ".tmp.XXXXXX"];
    int printed = snprintf(temporary, sizeof temporary, "%s.tmp.XXXXXX", path);
    if (printed <= 0 || (size_t)printed >= sizeof temporary) {
        free(data);
        return false;
    }
    int descriptor = mkstemp(temporary);
    bool okay = descriptor >= 0;
    if (okay)
        okay = write_all(descriptor, data, length) && fsync(descriptor) == 0;
    if (descriptor >= 0 && close(descriptor) != 0) okay = false;
    if (okay && rename(temporary, path) != 0) okay = false;
    if (okay) okay = sync_parent(path);
    if (!okay) (void)unlink(temporary);
    free(data);
    if (okay) *saved_count = encoded_count;
    return okay;
}
