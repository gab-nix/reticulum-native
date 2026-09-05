#include "tui_paths.h"

#include "reticulum/config.h"
#include "reticulum/hal.h"
#include "reticulum/node_registry.h"
#include "reticulum/path_store.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static double synthetic_clock(void *context) { (void)context; return 100.0; }

static void populate_full_snapshot(rns_runtime_t *runtime) {
    rns_transport_config config = {.path_capacity = RNS_NODE_REGISTRY_MAX,
        .dedupe_capacity = 1u, .reverse_capacity = 1u,
        .random_blob_history = RNS_TRANSPORT_MAX_RANDOM_BLOBS,
        .path_lifetime = 3000.0, .dedupe_lifetime = 5.0,
        .reverse_lifetime = 480.0, .clock = synthetic_clock};
    rns_transport transport;
    assert(rns_transport_init(&transport, &config));
    for (size_t i = 0u; i < RNS_NODE_REGISTRY_MAX; ++i) {
        rns_path_entry *entry = &transport.paths[i];
        entry->occupied = 1; entry->has_identity = 1;
        entry->destination_hash[0] = (uint8_t)(i >> 8u);
        entry->destination_hash[1] = (uint8_t)i;
        memset(entry->identity_public_key, (int)(i % 251u + 1u), 64u);
        entry->updated_at = 100.0; entry->expires_at = 3100.0;
        entry->random_blob_count = RNS_TRANSPORT_MAX_RANDOM_BLOBS;
        entry->announce_timebase = RNS_TRANSPORT_MAX_RANDOM_BLOBS;
        for (size_t j = 0u; j < entry->random_blob_count; ++j)
            entry->random_blobs[j][9] = (uint8_t)(j + 1u);
    }
    uint64_t wall; size_t length, count;
    assert(rns_hal_wallclock_ms(&wall) == RNS_OK);
    assert(rns_path_store_encode(&transport, wall, NULL, 0u, &length, &count) == RNS_ERROR_OVERFLOW);
    assert(length > 64u * 1024u && count == RNS_NODE_REGISTRY_MAX);
    uint8_t *bytes = malloc(length); assert(bytes != NULL);
    assert(rns_path_store_encode(&transport, wall, bytes, length, &length, &count) == RNS_OK);
    assert(rns_runtime_paths_import(runtime, wall, bytes, length, &count) == RNS_OK);
    assert(count == RNS_NODE_REGISTRY_MAX);
    free(bytes); rns_transport_free(&transport);
}

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
    rns_runtime_options_t options = {0};
    options.path_capacity = RNS_NODE_REGISTRY_MAX;
    assert(rns_runtime_create(&first, &config, &options) == RNS_OK);
    assert(rns_runtime_create(&second, &config, &options) == RNS_OK);

    size_t count = 99u;
    assert(tui_paths_load(second, missing, &count) == TUI_PATHS_MISSING);
    assert(count == 0u);
    assert(tui_paths_save(first, path, &count));
    assert(count == 0u);
    count = 99u;
    assert(tui_paths_load(second, path, &count) == TUI_PATHS_LOADED);
    assert(count == 0u);

    populate_full_snapshot(first);
    assert(tui_paths_save(first, path, &count) && count == RNS_NODE_REGISTRY_MAX);
    struct stat info; assert(stat(path, &info) == 0 && info.st_size > 64 * 1024);
    assert(tui_paths_load(second, path, &count) == TUI_PATHS_LOADED);
    assert(count == RNS_NODE_REGISTRY_MAX);
    uint8_t destination[16] = {0}; rns_path_entry restored;
    assert(rns_runtime_path_lookup(second, destination, &restored) == RNS_OK);
    assert(restored.has_identity && restored.identity_public_key[0] == 1u);
    FILE *file = fopen(path, "r+b");
    assert(file != NULL);
    int byte = fgetc(file);
    assert(byte != EOF && fseek(file, 0L, SEEK_SET) == 0);
    assert(fputc(byte ^ 0xff, file) != EOF);
    assert(fclose(file) == 0);
    count = 99u;
    assert(tui_paths_load(second, path, &count) == TUI_PATHS_INVALID);
    assert(count == 0u);
    assert(rns_runtime_path_lookup(second, destination, &restored) == RNS_OK);
    assert(restored.has_identity);

    file = fopen(path, "wb"); assert(file != NULL);
    assert(ftruncate(fileno(file), 2 * 1024 * 1024 + 1) == 0 && fclose(file) == 0);
    assert(tui_paths_load(second, path, &count) == TUI_PATHS_INVALID);
    assert(rns_runtime_path_lookup(second, destination, &restored) == RNS_OK);

    assert(unlink(path) == 0);
    assert(symlink(missing, path) == 0);
    assert(tui_paths_load(second, path, &count) == TUI_PATHS_IO_ERROR);
    assert(unlink(path) == 0);

    rns_runtime_destroy(second);
    rns_runtime_destroy(first);
    assert(rmdir(directory) == 0);
    return 0;
}
