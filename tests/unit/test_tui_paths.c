#include "tui_paths.h"

#include "reticulum/config.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

int main(void) {
    char directory[] = "/tmp/reticulum-tui-paths-XXXXXX";
    assert(mkdtemp(directory) != NULL);
    char path[1024];
    char missing[1024];
    assert(snprintf(path, sizeof path, "%s/paths", directory) > 0);
    assert(snprintf(missing, sizeof missing, "%s/missing", directory) > 0);

    rns_config_t config;
    rns_config_init(&config);
    rns_runtime_t *first = NULL;
    rns_runtime_t *second = NULL;
    assert(rns_runtime_create(&first, &config, NULL) == RNS_OK);
    assert(rns_runtime_create(&second, &config, NULL) == RNS_OK);

    size_t count = 99u;
    assert(tui_paths_load(second, missing, &count) == TUI_PATHS_MISSING);
    assert(count == 0u);
    assert(tui_paths_save(first, path, &count));
    assert(count == 0u);
    count = 99u;
    assert(tui_paths_load(second, path, &count) == TUI_PATHS_LOADED);
    assert(count == 0u);

    FILE *file = fopen(path, "r+b");
    assert(file != NULL);
    int byte = fgetc(file);
    assert(byte != EOF && fseek(file, 0L, SEEK_SET) == 0);
    assert(fputc(byte ^ 0xff, file) != EOF);
    assert(fclose(file) == 0);
    count = 99u;
    assert(tui_paths_load(second, path, &count) == TUI_PATHS_INVALID);
    assert(count == 0u);

    assert(unlink(path) == 0);
    assert(symlink(missing, path) == 0);
    assert(tui_paths_load(second, path, &count) == TUI_PATHS_IO_ERROR);
    assert(unlink(path) == 0);

    rns_runtime_destroy(second);
    rns_runtime_destroy(first);
    assert(rmdir(directory) == 0);
    return 0;
}
