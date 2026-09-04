#ifndef TEST_FAKE_NVS_H
#define TEST_FAKE_NVS_H

#include <stddef.h>
#include <stdint.h>

typedef int32_t esp_err_t;
typedef uint32_t nvs_handle_t;

#define ESP_OK 0
#define ESP_ERR_NO_MEM 0x101
#define ESP_ERR_NVS_NOT_FOUND 0x1102
#define ESP_ERR_NVS_INVALID_LENGTH 0x110c
#define ESP_ERR_NVS_READ_ONLY 0x1104
#define NVS_READWRITE 1

esp_err_t nvs_open(const char *namespace_name, int open_mode,
                   nvs_handle_t *out_handle);
void nvs_close(nvs_handle_t handle);
esp_err_t nvs_get_blob(nvs_handle_t handle, const char *key, void *output,
                       size_t *length);
esp_err_t nvs_set_blob(nvs_handle_t handle, const char *key, const void *value,
                       size_t length);
esp_err_t nvs_erase_key(nvs_handle_t handle, const char *key);
esp_err_t nvs_commit(nvs_handle_t handle);

#endif
