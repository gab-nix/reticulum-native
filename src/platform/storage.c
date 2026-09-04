#include "reticulum/storage.h"

#include <string.h>

#include "reticulum/hal.h"

#define RNS_STORAGE_KEY_MAX 64U

struct rns_storage {
    const rns_storage_ops_t *ops;
    const rns_platform_ops_t *platform;
    void *context;
};

static int valid_key(const char *key) {
    size_t length = 0U;
    if (key == NULL) return 0;
    while (length <= RNS_STORAGE_KEY_MAX && key[length] != '\0') length++;
    return length != 0U && length <= RNS_STORAGE_KEY_MAX;
}

rns_status_t rns_storage_create(const rns_storage_ops_t *ops, void *context,
                                rns_storage_t **storage) {
    const rns_platform_ops_t *platform;
    rns_storage_t *created;
    if (storage == NULL || ops == NULL || ops->read == NULL ||
        ops->write_atomic == NULL || ops->remove == NULL) {
        return RNS_ERROR_INVALID_ARGUMENT;
    }
    *storage = NULL;
    platform = rns_platform_current();
    created = platform->allocate(platform->context, sizeof(*created));
    if (created == NULL) return RNS_ERROR_NO_MEMORY;
    created->ops = ops;
    created->platform = platform;
    created->context = context;
    *storage = created;
    return RNS_OK;
}

void rns_storage_destroy(rns_storage_t *storage) {
    if (storage == NULL) return;
    if (storage->ops->destroy != NULL) storage->ops->destroy(storage->context);
    storage->platform->deallocate(storage->platform->context, storage);
}

rns_status_t rns_storage_read(rns_storage_t *storage, const char *key,
                              uint8_t *output, size_t capacity, size_t *length) {
    if (storage == NULL || !valid_key(key) || length == NULL ||
        (output == NULL && capacity != 0U)) return RNS_ERROR_INVALID_ARGUMENT;
    *length = 0U;
    return storage->ops->read(storage->context, key, output, capacity, length);
}

rns_status_t rns_storage_write_atomic(rns_storage_t *storage, const char *key,
                                      const uint8_t *data, size_t length) {
    if (storage == NULL || !valid_key(key) || (data == NULL && length != 0U))
        return RNS_ERROR_INVALID_ARGUMENT;
    return storage->ops->write_atomic(storage->context, key, data, length);
}

rns_status_t rns_storage_remove(rns_storage_t *storage, const char *key) {
    if (storage == NULL || !valid_key(key)) return RNS_ERROR_INVALID_ARGUMENT;
    return storage->ops->remove(storage->context, key);
}
