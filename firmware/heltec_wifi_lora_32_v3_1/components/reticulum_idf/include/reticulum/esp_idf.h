#ifndef RETICULUM_ESP_IDF_H
#define RETICULUM_ESP_IDF_H

#include <stddef.h>
#include <stdint.h>

#include "reticulum/status.h"
#include "reticulum/storage.h"

#ifdef __cplusplus
extern "C" {
#endif

#define RNS_ESP_NVS_DEFAULT_NAMESPACE "reticulum"
#define RNS_ESP_WALLCLOCK_MIN_MS UINT64_C(1577836800000)

typedef struct rns_esp_nvs_storage_config {
    const char *namespace_name;
    size_t maximum_value_size;
} rns_esp_nvs_storage_config_t;

/* Install the process-wide ESP-IDF platform provider for a radio-only build.
 * This enables the internal SAR-ADC entropy source and keeps it enabled while
 * cryptographic randomness can be requested. Call once during app_main before
 * identity generation or any Reticulum object is created. */
rns_status_t rns_esp_platform_install(void);

/* Install the process-wide ESP-IDF crypto provider after the platform
 * provider. Hashing and AES use mbedTLS; Ed25519 and X25519 use the pinned
 * ESP Component Registry libsodium component. */
rns_status_t rns_esp_crypto_install(void);

/* Run deterministic known-answer checks through the installed provider.
 * Firmware calls this at boot before loading identity material. */
rns_status_t rns_esp_crypto_self_test(void);

/* Disable the radio-only entropy source before initializing ADC, Wi-Fi or
 * Bluetooth. Random requests fail closed afterwards. The caller must first
 * quiesce cryptographic users. Re-enabling is allowed only when those
 * subsystems are stopped and no ADC user is active. */
rns_status_t rns_esp_entropy_enable_radio_only(void);
void rns_esp_entropy_disable(void);

/* The V3.1 has no battery-backed wall clock. Wall time therefore starts
 * invalid on every cold boot and is never restored from NVS. An authenticated
 * application control path may set it for the current boot. */
rns_status_t rns_esp_wallclock_set_ms(uint64_t milliseconds);

/* Open bounded NVS storage for the literal "identity" and "config" records.
 * This never erases the NVS partition; callers own the returned handle. */
rns_status_t rns_esp_nvs_storage_open(
    const rns_esp_nvs_storage_config_t *config, rns_storage_t **storage);

#ifdef __cplusplus
}
#endif

#endif
