/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "reticulum/esp_idf.h"

#include <stdio.h>
#include <string.h>

#include "nvs.h"
#include "reticulum/hal.h"
#include "reticulum/storage_record.h"

#define RNS_ESP_NVS_NAMESPACE_MAX 15U
#define RNS_ESP_NVS_NAME_SIZE 12U

typedef struct esp_nvs_storage {
    nvs_handle_t handle;
    size_t maximum_value_size;
    rns_hal_mutex_t *lock;
} esp_nvs_storage_t;

typedef struct slot_metadata {
    int present;
    int valid;
    uint32_t generation;
    size_t payload_length;
} slot_metadata_t;

static int bounded_string(const char *value, size_t maximum) {
    size_t count = 0U;
    if (value == NULL) return 0;
    while (count <= maximum && value[count] != '\0') count++;
    return count != 0U && count <= maximum;
}

/* The first firmware milestone intentionally exposes only its two durable
 * records. Literal names make the NVS mapping collision-free by construction. */
static int valid_storage_key(const char *key) {
    return strcmp(key, "identity") == 0 || strcmp(key, "config") == 0;
}

static void slot_name(const char *key, char suffix,
                      char output[RNS_ESP_NVS_NAME_SIZE]) {
    (void)snprintf(output, RNS_ESP_NVS_NAME_SIZE, "%s_%c", key, suffix);
}

static rns_status_t map_nvs_error(esp_err_t error) {
    switch (error) {
        case ESP_OK: return RNS_OK;
        case ESP_ERR_NVS_NOT_FOUND: return RNS_ERROR_NOT_FOUND;
        case ESP_ERR_NVS_INVALID_LENGTH: return RNS_ERROR_OVERFLOW;
        case ESP_ERR_NO_MEM: return RNS_ERROR_NO_MEMORY;
        case ESP_ERR_NVS_READ_ONLY: return RNS_ERROR_UNSUPPORTED;
        default: return RNS_ERROR_IO;
    }
}

/* Caller holds storage->lock for the complete transaction. */
static rns_status_t read_slot_metadata(esp_nvs_storage_t *storage,
                                       const char *key, char suffix,
                                       slot_metadata_t *metadata) {
    char name[RNS_ESP_NVS_NAME_SIZE];
    uint8_t *record;
    size_t record_length = 0U;
    size_t allocation_size;
    esp_err_t result;
    rns_status_t status;
    memset(metadata, 0, sizeof(*metadata));
    slot_name(key, suffix, name);
    result = nvs_get_blob(storage->handle, name, NULL, &record_length);
    if (result == ESP_ERR_NVS_NOT_FOUND) return RNS_OK;
    if (result != ESP_OK) return map_nvs_error(result);
    metadata->present = 1;
    if (record_length < RNS_STORAGE_RECORD_HEADER_SIZE ||
        record_length > RNS_STORAGE_RECORD_HEADER_SIZE + storage->maximum_value_size)
        return RNS_OK;
    record = rns_hal_allocate(record_length);
    if (record == NULL) return RNS_ERROR_NO_MEMORY;
    allocation_size = record_length;
    result = nvs_get_blob(storage->handle, name, record, &record_length);
    if (result != ESP_OK) {
        rns_hal_secure_zero(record, allocation_size);
        rns_hal_deallocate(record);
        return map_nvs_error(result);
    }
    status = rns_storage_record_decode(key, record, record_length, NULL, 0U,
                                       &metadata->payload_length,
                                       &metadata->generation);
    if (status == RNS_ERROR_OVERFLOW ||
        (status == RNS_OK && metadata->payload_length == 0U)) {
        metadata->valid = 1;
        status = RNS_OK;
    } else if (status == RNS_ERROR_PROTOCOL) {
        status = RNS_OK;
    }
    rns_hal_secure_zero(record, allocation_size);
    rns_hal_deallocate(record);
    return status;
}

static rns_status_t nvs_storage_read(void *context, const char *key,
                                     uint8_t *output, size_t capacity,
                                     size_t *length) {
    esp_nvs_storage_t *storage = context;
    slot_metadata_t slots[2];
    char suffix;
    char name[RNS_ESP_NVS_NAME_SIZE];
    uint8_t *record;
    size_t record_length = 0U;
    size_t allocation_size;
    uint32_t generation;
    esp_err_t result;
    rns_status_t status;
    if (!valid_storage_key(key)) return RNS_ERROR_INVALID_ARGUMENT;
    status = rns_hal_mutex_lock(storage->lock);
    if (status != RNS_OK) return status;
    status = read_slot_metadata(storage, key, 'a', &slots[0]);
    if (status != RNS_OK) goto done;
    status = read_slot_metadata(storage, key, 'b', &slots[1]);
    if (status != RNS_OK) goto done;
    if (!slots[0].valid && !slots[1].valid) {
        status = slots[0].present || slots[1].present
                     ? RNS_ERROR_PROTOCOL : RNS_ERROR_NOT_FOUND;
        goto done;
    }
    status = rns_storage_record_select_slot(slots[0].valid, slots[0].generation,
                                            slots[1].valid, slots[1].generation,
                                            &suffix);
    if (status != RNS_OK) goto done;
    *length = suffix == 'a' ? slots[0].payload_length : slots[1].payload_length;
    if (capacity < *length || (*length != 0U && output == NULL)) {
        status = RNS_ERROR_OVERFLOW;
        goto done;
    }
    slot_name(key, suffix, name);
    result = nvs_get_blob(storage->handle, name, NULL, &record_length);
    if (result != ESP_OK) {
        status = map_nvs_error(result);
        goto done;
    }
    record = rns_hal_allocate(record_length);
    if (record == NULL) {
        status = RNS_ERROR_NO_MEMORY;
        goto done;
    }
    allocation_size = record_length;
    result = nvs_get_blob(storage->handle, name, record, &record_length);
    if (result == ESP_OK)
        status = rns_storage_record_decode(key, record, record_length, output,
                                           capacity, length, &generation);
    else
        status = map_nvs_error(result);
    rns_hal_secure_zero(record, allocation_size);
    rns_hal_deallocate(record);
done:
    if (rns_hal_mutex_unlock(storage->lock) != RNS_OK && status == RNS_OK)
        status = RNS_ERROR_IO;
    return status;
}

static rns_status_t nvs_storage_write(void *context, const char *key,
                                      const uint8_t *data, size_t length) {
    esp_nvs_storage_t *storage = context;
    slot_metadata_t slots[2];
    uint32_t generation;
    char suffix;
    char name[RNS_ESP_NVS_NAME_SIZE];
    uint8_t *record;
    size_t record_length;
    esp_err_t result;
    rns_status_t status;
    if (!valid_storage_key(key)) return RNS_ERROR_INVALID_ARGUMENT;
    if (length > storage->maximum_value_size) return RNS_ERROR_OVERFLOW;
    status = rns_hal_mutex_lock(storage->lock);
    if (status != RNS_OK) return status;
    status = read_slot_metadata(storage, key, 'a', &slots[0]);
    if (status != RNS_OK) goto done;
    status = read_slot_metadata(storage, key, 'b', &slots[1]);
    if (status != RNS_OK) goto done;
    if (!slots[0].valid && !slots[1].valid &&
        (slots[0].present || slots[1].present)) {
        status = RNS_ERROR_PROTOCOL;
        goto done;
    }
    status = rns_storage_record_next_slot(slots[0].valid, slots[0].generation,
                                          slots[1].valid, slots[1].generation,
                                          &suffix, &generation);
    if (status != RNS_OK) goto done;
    status = rns_storage_record_encoded_size(length, &record_length);
    if (status != RNS_OK) goto done;
    record = rns_hal_allocate(record_length);
    if (record == NULL) {
        status = RNS_ERROR_NO_MEMORY;
        goto done;
    }
    status = rns_storage_record_encode(key, generation, data, length, record,
                                       record_length, &record_length);
    if (status == RNS_OK) {
        slot_name(key, suffix, name);
        result = nvs_set_blob(storage->handle, name, record, record_length);
        if (result == ESP_OK) result = nvs_commit(storage->handle);
        status = map_nvs_error(result);
    }
    rns_hal_secure_zero(record, record_length);
    rns_hal_deallocate(record);
done:
    if (rns_hal_mutex_unlock(storage->lock) != RNS_OK && status == RNS_OK)
        status = RNS_ERROR_IO;
    return status;
}

static rns_status_t nvs_storage_remove(void *context, const char *key) {
    esp_nvs_storage_t *storage = context;
    char name_a[RNS_ESP_NVS_NAME_SIZE];
    char name_b[RNS_ESP_NVS_NAME_SIZE];
    esp_err_t result_a;
    esp_err_t result_b;
    rns_status_t status;
    if (!valid_storage_key(key)) return RNS_ERROR_INVALID_ARGUMENT;
    status = rns_hal_mutex_lock(storage->lock);
    if (status != RNS_OK) return status;
    slot_name(key, 'a', name_a);
    slot_name(key, 'b', name_b);
    result_a = nvs_erase_key(storage->handle, name_a);
    result_b = nvs_erase_key(storage->handle, name_b);
    if (result_a != ESP_OK && result_a != ESP_ERR_NVS_NOT_FOUND)
        status = map_nvs_error(result_a);
    else if (result_b != ESP_OK && result_b != ESP_ERR_NVS_NOT_FOUND)
        status = map_nvs_error(result_b);
    else
        status = map_nvs_error(nvs_commit(storage->handle));
    if (rns_hal_mutex_unlock(storage->lock) != RNS_OK && status == RNS_OK)
        status = RNS_ERROR_IO;
    return status;
}

static void nvs_storage_destroy(void *context) {
    esp_nvs_storage_t *storage = context;
    if (storage == NULL) return;
    nvs_close(storage->handle);
    rns_hal_mutex_destroy(storage->lock);
    rns_hal_secure_zero(storage, sizeof(*storage));
    rns_hal_deallocate(storage);
}

rns_status_t rns_esp_nvs_storage_open(
    const rns_esp_nvs_storage_config_t *config, rns_storage_t **storage) {
    static const rns_storage_ops_t ops = {
        nvs_storage_read, nvs_storage_write, nvs_storage_remove,
        nvs_storage_destroy
    };
    esp_nvs_storage_t *context;
    const char *namespace_name = RNS_ESP_NVS_DEFAULT_NAMESPACE;
    size_t maximum = RNS_STORAGE_RECORD_MAX_PAYLOAD;
    esp_err_t result;
    rns_status_t status;
    if (storage == NULL) return RNS_ERROR_INVALID_ARGUMENT;
    *storage = NULL;
    if (config != NULL) {
        if (config->namespace_name != NULL) namespace_name = config->namespace_name;
        if (config->maximum_value_size != 0U) maximum = config->maximum_value_size;
    }
    if (!bounded_string(namespace_name, RNS_ESP_NVS_NAMESPACE_MAX) ||
        maximum > RNS_STORAGE_RECORD_MAX_PAYLOAD)
        return RNS_ERROR_INVALID_ARGUMENT;
    context = rns_hal_allocate(sizeof(*context));
    if (context == NULL) return RNS_ERROR_NO_MEMORY;
    memset(context, 0, sizeof(*context));
    context->maximum_value_size = maximum;
    status = rns_hal_mutex_create(&context->lock);
    if (status != RNS_OK) {
        rns_hal_deallocate(context);
        return status;
    }
    result = nvs_open(namespace_name, NVS_READWRITE, &context->handle);
    if (result != ESP_OK) {
        rns_hal_mutex_destroy(context->lock);
        rns_hal_deallocate(context);
        return map_nvs_error(result);
    }
    status = rns_storage_create(&ops, context, storage);
    if (status != RNS_OK) nvs_storage_destroy(context);
    return status;
}
