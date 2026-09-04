#include "reticulum/esp_idf.h"

#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <string.h>
#include <time.h>

#include "nvs.h"
#include "reticulum/storage.h"
#include "reticulum/storage_record.h"

#define SLOT_CAPACITY 4200U

typedef struct fake_slot {
    char name[12];
    uint8_t data[SLOT_CAPACITY];
    size_t length;
    int present;
} fake_slot_t;

static fake_slot_t slots[4];
static fake_slot_t pending;
static int fail_commit;
static int grow_second_read;
static int grow_final_query;
static int existing_length_queries;
static atomic_int concurrent_nvs_calls;
static atomic_int active_nvs_calls;
static atomic_int writers_ready;
static atomic_int writers_go;

static void tiny_delay(void) {
    const struct timespec value = {0, 100000L};
    (void)nanosleep(&value, NULL);
}

static void enter_nvs(void) {
    if (atomic_fetch_add(&active_nvs_calls, 1) != 0)
        atomic_store(&concurrent_nvs_calls, 1);
    tiny_delay();
}

static void leave_nvs(void) { (void)atomic_fetch_sub(&active_nvs_calls, 1); }

static fake_slot_t *find_slot(const char *name) {
    size_t index;
    for (index = 0U; index < 4U; index++)
        if (slots[index].present && strcmp(slots[index].name, name) == 0)
            return &slots[index];
    return NULL;
}

static fake_slot_t *create_slot(const char *name) {
    size_t index;
    fake_slot_t *slot = find_slot(name);
    if (slot != NULL) return slot;
    for (index = 0U; index < 4U; index++) {
        if (!slots[index].present) {
            (void)strncpy(slots[index].name, name, sizeof(slots[index].name) - 1U);
            slots[index].present = 1;
            return &slots[index];
        }
    }
    return NULL;
}

esp_err_t nvs_open(const char *namespace_name, int open_mode,
                   nvs_handle_t *out_handle) {
    assert(strcmp(namespace_name, "reticulum") == 0);
    assert(open_mode == NVS_READWRITE);
    *out_handle = 1U;
    return ESP_OK;
}

void nvs_close(nvs_handle_t handle) { assert(handle == 1U); }

esp_err_t nvs_get_blob(nvs_handle_t handle, const char *key, void *output,
                       size_t *length) {
    fake_slot_t *slot;
    esp_err_t result = ESP_OK;
    assert(handle == 1U && length != NULL);
    enter_nvs();
    slot = find_slot(key);
    if (slot == NULL) {
        result = ESP_ERR_NVS_NOT_FOUND;
    } else if (output == NULL) {
        existing_length_queries++;
        if (grow_final_query && existing_length_queries == 2) {
            grow_final_query = 0;
            *length = slot->length + 1U;
        } else {
            *length = slot->length;
        }
    } else if (grow_second_read) {
        grow_second_read = 0;
        *length = slot->length + 1U;
        result = ESP_ERR_NVS_INVALID_LENGTH;
    } else if (*length < slot->length) {
        *length = slot->length;
        result = ESP_ERR_NVS_INVALID_LENGTH;
    } else {
        memcpy(output, slot->data, slot->length);
        *length = slot->length;
    }
    leave_nvs();
    return result;
}

esp_err_t nvs_set_blob(nvs_handle_t handle, const char *key, const void *value,
                       size_t length) {
    assert(handle == 1U && length <= sizeof(pending.data));
    enter_nvs();
    memset(&pending, 0, sizeof(pending));
    (void)strncpy(pending.name, key, sizeof(pending.name) - 1U);
    memcpy(pending.data, value, length);
    pending.length = length;
    pending.present = 1;
    leave_nvs();
    return ESP_OK;
}

esp_err_t nvs_erase_key(nvs_handle_t handle, const char *key) {
    fake_slot_t *slot;
    assert(handle == 1U);
    enter_nvs();
    slot = find_slot(key);
    if (slot != NULL) memset(slot, 0, sizeof(*slot));
    leave_nvs();
    return slot != NULL ? ESP_OK : ESP_ERR_NVS_NOT_FOUND;
}

esp_err_t nvs_commit(nvs_handle_t handle) {
    fake_slot_t *slot;
    assert(handle == 1U);
    enter_nvs();
    if (fail_commit) {
        fail_commit = 0;
        leave_nvs();
        return 0x1100;
    }
    if (pending.present) {
        slot = create_slot(pending.name);
        assert(slot != NULL);
        *slot = pending;
        memset(&pending, 0, sizeof(pending));
    }
    leave_nvs();
    return ESP_OK;
}

static void *writer(void *context) {
    rns_storage_t *storage = context;
    unsigned index;
    const uint8_t value = 0x55U;
    (void)atomic_fetch_add(&writers_ready, 1);
    while (atomic_load(&writers_go) == 0) tiny_delay();
    for (index = 0U; index < 10U; index++)
        assert(rns_storage_write_atomic(storage, "identity", &value, 1U) == RNS_OK);
    return NULL;
}

int main(void) {
    rns_storage_t *storage = NULL;
    rns_storage_t *invalid_storage = NULL;
    rns_esp_nvs_storage_config_t config;
    uint8_t output[8] = {0U};
    const uint8_t first[] = {1U, 2U, 3U};
    const uint8_t second[] = {4U, 5U};
    size_t length = 0U;
    pthread_t threads[2];
    fake_slot_t *slot_a;
    fake_slot_t *slot_b;

    config.namespace_name = "";
    config.maximum_value_size = 4U;
    assert(rns_esp_nvs_storage_open(&config, &invalid_storage) ==
           RNS_ERROR_INVALID_ARGUMENT);
    config.namespace_name = NULL;
    config.maximum_value_size = RNS_STORAGE_RECORD_MAX_PAYLOAD + 1U;
    assert(rns_esp_nvs_storage_open(&config, &invalid_storage) ==
           RNS_ERROR_INVALID_ARGUMENT);

    assert(rns_esp_nvs_storage_open(NULL, &storage) == RNS_OK);
    assert(rns_storage_write_atomic(storage, "other", first, sizeof(first)) ==
           RNS_ERROR_INVALID_ARGUMENT);
    assert(rns_storage_write_atomic(storage, "identity", first, sizeof(first)) == RNS_OK);
    assert(rns_storage_read(storage, "identity", output, 1U, &length) ==
           RNS_ERROR_OVERFLOW && length == sizeof(first));
    assert(rns_storage_read(storage, "identity", output, sizeof(output), &length) == RNS_OK);
    assert(length == sizeof(first) && memcmp(output, first, length) == 0);
    assert(rns_storage_write_atomic(storage, "identity", first,
                                    RNS_STORAGE_RECORD_MAX_PAYLOAD + 1U) ==
           RNS_ERROR_OVERFLOW);

    /* This models a hypothetical buffered NVS implementation where a failed
     * commit leaves the previously committed slot readable. ESP-IDF 5.5 writes
     * directly and currently treats commit as a no-op; target validation is a
     * separate release gate. */
    fail_commit = 1;
    assert(rns_storage_write_atomic(storage, "identity", second, sizeof(second)) ==
           RNS_ERROR_IO);
    assert(rns_storage_read(storage, "identity", output, sizeof(output), &length) == RNS_OK);
    assert(length == sizeof(first) && memcmp(output, first, length) == 0);

    assert(rns_storage_write_atomic(storage, "identity", second, sizeof(second)) == RNS_OK);
    slot_a = find_slot("identity_a");
    slot_b = find_slot("identity_b");
    assert(slot_a != NULL && slot_b != NULL);
    slot_b->data[slot_b->length - 1U] ^= 1U;
    assert(rns_storage_read(storage, "identity", output, sizeof(output), &length) == RNS_OK);
    assert(length == sizeof(first) && memcmp(output, first, length) == 0);
    slot_a->data[slot_a->length - 1U] ^= 1U;
    assert(rns_storage_read(storage, "identity", output, sizeof(output), &length) ==
           RNS_ERROR_PROTOCOL);
    assert(rns_storage_write_atomic(storage, "identity", first, sizeof(first)) ==
           RNS_ERROR_PROTOCOL);
    slot_a->data[slot_a->length - 1U] ^= 1U;
    slot_b->data[slot_b->length - 1U] ^= 1U;

    assert(rns_storage_write_atomic(storage, "config", first, sizeof(first)) == RNS_OK);
    slot_a = find_slot("config_a");
    assert(slot_a != NULL);
    slot_a->data[slot_a->length - 1U] ^= 1U;
    assert(rns_storage_read(storage, "config", output, sizeof(output), &length) ==
           RNS_ERROR_PROTOCOL);
    slot_a->data[slot_a->length - 1U] ^= 1U;

    grow_second_read = 1;
    assert(rns_storage_read(storage, "identity", output, sizeof(output), &length) ==
           RNS_ERROR_OVERFLOW);

    existing_length_queries = 0;
    grow_final_query = 1;
    assert(rns_storage_read(storage, "config", output, sizeof(output), &length) ==
           RNS_ERROR_PROTOCOL);

    atomic_store(&writers_ready, 0);
    atomic_store(&writers_go, 0);
    assert(pthread_create(&threads[0], NULL, writer, storage) == 0);
    assert(pthread_create(&threads[1], NULL, writer, storage) == 0);
    while (atomic_load(&writers_ready) != 2) tiny_delay();
    atomic_store(&writers_go, 1);
    assert(pthread_join(threads[0], NULL) == 0);
    assert(pthread_join(threads[1], NULL) == 0);
    assert(atomic_load(&concurrent_nvs_calls) == 0);

    assert(rns_storage_remove(storage, "identity") == RNS_OK);
    assert(rns_storage_read(storage, "identity", output, sizeof(output), &length) ==
           RNS_ERROR_NOT_FOUND);
    assert(rns_storage_remove(storage, "identity") == RNS_OK);
    assert(rns_storage_remove(storage, "other") == RNS_ERROR_INVALID_ARGUMENT);
    rns_storage_destroy(storage);
    return 0;
}
