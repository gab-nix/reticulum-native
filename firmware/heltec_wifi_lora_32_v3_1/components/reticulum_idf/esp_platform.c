/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "reticulum/esp_idf.h"

#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/time.h>

#include "bootloader_random.h"
#include "esp_delay.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "reticulum/hal.h"

#define RNS_ESP_TASK_STACK_BYTES 4096U
#define RNS_ESP_TASK_PRIORITY (tskIDLE_PRIORITY + 1U)

typedef struct esp_thread {
    TaskHandle_t task;
    SemaphoreHandle_t completed;
    rns_hal_thread_fn function;
    void *context;
    void *result;
    int joined;
    int detached;
    portMUX_TYPE lock;
} esp_thread_t;

static const char *TAG = "reticulum";
enum {
    ENTROPY_DISABLED = 0,
    ENTROPY_ENABLING,
    ENTROPY_ENABLED,
    ENTROPY_DISABLING
};

static atomic_int entropy_state = ATOMIC_VAR_INIT(ENTROPY_DISABLED);
static atomic_bool wallclock_valid = ATOMIC_VAR_INIT(0);

static rns_status_t esp_monotonic_ms(void *context, uint64_t *milliseconds) {
    int64_t microseconds;
    (void)context;
    if (milliseconds == NULL) return RNS_ERROR_INVALID_ARGUMENT;
    microseconds = esp_timer_get_time();
    if (microseconds < 0) return RNS_ERROR_IO;
    *milliseconds = (uint64_t)microseconds / 1000U;
    return RNS_OK;
}

static rns_status_t esp_wallclock_ms(void *context, uint64_t *milliseconds) {
    struct timeval value;
    uint64_t seconds;
    (void)context;
    if (milliseconds == NULL) return RNS_ERROR_INVALID_ARGUMENT;
    if (!atomic_load_explicit(&wallclock_valid, memory_order_acquire))
        return RNS_ERROR_INVALID_STATE;
    if (gettimeofday(&value, NULL) != 0 || value.tv_sec < 0 || value.tv_usec < 0)
        return RNS_ERROR_IO;
    seconds = (uint64_t)value.tv_sec;
    if (seconds > UINT64_MAX / 1000U) return RNS_ERROR_OVERFLOW;
    *milliseconds = seconds * 1000U + (uint64_t)value.tv_usec / 1000U;
    if (*milliseconds < RNS_ESP_WALLCLOCK_MIN_MS) return RNS_ERROR_INVALID_STATE;
    return RNS_OK;
}

static rns_status_t esp_random_bytes(void *context, void *output, size_t length) {
    (void)context;
    if (output == NULL && length != 0U) return RNS_ERROR_INVALID_ARGUMENT;
    if (atomic_load_explicit(&entropy_state, memory_order_acquire) !=
        ENTROPY_ENABLED)
        return RNS_ERROR_INVALID_STATE;
    if (length != 0U) esp_fill_random(output, length);
    return RNS_OK;
}

static void esp_secure_zero(void *context, void *memory, size_t length) {
    volatile uint8_t *cursor = memory;
    (void)context;
    while (cursor != NULL && length != 0U) {
        *cursor++ = 0U;
        length--;
    }
}

static rns_status_t esp_sleep_ms(void *context, uint64_t milliseconds) {
    uint64_t remaining = milliseconds;
    uint64_t consumed;
    uint32_t ticks;
    uint32_t maximum_ticks = (uint32_t)portMAX_DELAY;
    rns_status_t status;
    (void)context;
    if (remaining == 0U) return RNS_OK;
    /* Keep portMAX_DELAY out of vTaskDelay(), since some FreeRTOS
     * configurations reserve it for an indefinite wait. */
    if (maximum_ticks > 1U) maximum_ticks--;
    while (remaining != 0U) {
        status = rns_esp_delay_chunk(remaining, (uint32_t)configTICK_RATE_HZ,
                                     maximum_ticks, &ticks, &consumed);
        if (status != RNS_OK) return status;
        vTaskDelay((TickType_t)ticks);
        remaining -= consumed;
    }
    return RNS_OK;
}

static void *esp_allocate(void *context, size_t size) {
    (void)context;
    return heap_caps_malloc(size, MALLOC_CAP_8BIT);
}

static void esp_deallocate(void *context, void *memory) {
    (void)context;
    heap_caps_free(memory);
}

static rns_status_t esp_mutex_create(void *context, void **mutex) {
    SemaphoreHandle_t created;
    (void)context;
    if (mutex == NULL) return RNS_ERROR_INVALID_ARGUMENT;
    created = xSemaphoreCreateMutex();
    if (created == NULL) return RNS_ERROR_NO_MEMORY;
    *mutex = created;
    return RNS_OK;
}

static void esp_mutex_destroy(void *context, void *mutex) {
    (void)context;
    if (mutex != NULL) vSemaphoreDelete((SemaphoreHandle_t)mutex);
}

static rns_status_t esp_mutex_lock(void *context, void *mutex) {
    (void)context;
    if (mutex == NULL) return RNS_ERROR_INVALID_ARGUMENT;
    return xSemaphoreTake((SemaphoreHandle_t)mutex, portMAX_DELAY) == pdTRUE
               ? RNS_OK : RNS_ERROR_IO;
}

static rns_status_t esp_mutex_unlock(void *context, void *mutex) {
    (void)context;
    if (mutex == NULL) return RNS_ERROR_INVALID_ARGUMENT;
    return xSemaphoreGive((SemaphoreHandle_t)mutex) == pdTRUE
               ? RNS_OK : RNS_ERROR_IO;
}

static void esp_thread_entry(void *argument) {
    esp_thread_t *thread = argument;
    int detached;
    thread->result = thread->function(thread->context);
    (void)xSemaphoreGive(thread->completed);
    portENTER_CRITICAL(&thread->lock);
    thread->task = NULL;
    detached = thread->detached;
    portEXIT_CRITICAL(&thread->lock);
    if (detached != 0) {
        vSemaphoreDelete(thread->completed);
        esp_secure_zero(NULL, thread, sizeof(*thread));
        heap_caps_free(thread);
    }
    vTaskDelete(NULL);
}

static rns_status_t esp_thread_create(void *context, void **thread,
                                      rns_hal_thread_fn function,
                                      void *function_context) {
    esp_thread_t *created;
    BaseType_t status;
    (void)context;
    if (thread == NULL || function == NULL) return RNS_ERROR_INVALID_ARGUMENT;
    created = heap_caps_calloc(1U, sizeof(*created), MALLOC_CAP_8BIT);
    if (created == NULL) return RNS_ERROR_NO_MEMORY;
    created->completed = xSemaphoreCreateBinary();
    if (created->completed == NULL) {
        heap_caps_free(created);
        return RNS_ERROR_NO_MEMORY;
    }
    created->function = function;
    created->context = function_context;
    created->lock = (portMUX_TYPE)portMUX_INITIALIZER_UNLOCKED;
    status = xTaskCreate(esp_thread_entry, "rns", RNS_ESP_TASK_STACK_BYTES,
                         created, RNS_ESP_TASK_PRIORITY, &created->task);
    if (status != pdPASS) {
        vSemaphoreDelete(created->completed);
        heap_caps_free(created);
        return RNS_ERROR_NO_MEMORY;
    }
    *thread = created;
    return RNS_OK;
}

static rns_status_t esp_thread_join(void *context, void *thread_value,
                                    void **result) {
    esp_thread_t *thread = thread_value;
    (void)context;
    if (thread == NULL || thread->joined != 0 ||
        thread->task == xTaskGetCurrentTaskHandle()) return RNS_ERROR_INVALID_ARGUMENT;
    if (xSemaphoreTake(thread->completed, portMAX_DELAY) != pdTRUE)
        return RNS_ERROR_IO;
    thread->joined = 1;
    if (result != NULL) *result = thread->result;
    return RNS_OK;
}

static void esp_thread_destroy(void *context, void *thread_value) {
    esp_thread_t *thread = thread_value;
    int running;
    (void)context;
    if (thread == NULL) return;
    portENTER_CRITICAL(&thread->lock);
    running = thread->task != NULL;
    if (running) thread->detached = 1;
    portEXIT_CRITICAL(&thread->lock);
    if (running) return;
    vSemaphoreDelete(thread->completed);
    esp_secure_zero(NULL, thread, sizeof(*thread));
    heap_caps_free(thread);
}

static void esp_log_message(void *context, rns_log_level_t level,
                            const char *message) {
    (void)context;
    switch (level) {
        case RNS_LOG_ERROR: ESP_LOGE(TAG, "%s", message); break;
        case RNS_LOG_WARNING: ESP_LOGW(TAG, "%s", message); break;
        case RNS_LOG_INFO: ESP_LOGI(TAG, "%s", message); break;
        case RNS_LOG_DEBUG: ESP_LOGD(TAG, "%s", message); break;
        default: break;
    }
}

rns_status_t rns_esp_platform_install(void) {
    static const rns_platform_ops_t ops = {
        NULL, esp_monotonic_ms, esp_wallclock_ms, esp_random_bytes,
        esp_secure_zero, esp_sleep_ms, esp_allocate, esp_deallocate,
        esp_mutex_create, esp_mutex_destroy, esp_mutex_lock, esp_mutex_unlock,
        esp_thread_create, esp_thread_join, esp_thread_destroy, esp_log_message
    };
    int entropy_was_enabled =
        atomic_load_explicit(&entropy_state, memory_order_acquire) ==
        ENTROPY_ENABLED;
    rns_status_t status = rns_esp_entropy_enable_radio_only();
    if (status != RNS_OK) return status;
    status = rns_platform_install(&ops);
    if (status != RNS_OK && !entropy_was_enabled) rns_esp_entropy_disable();
    return status;
}

rns_status_t rns_esp_entropy_enable_radio_only(void) {
    int expected = ENTROPY_DISABLED;
    if (atomic_load_explicit(&entropy_state, memory_order_acquire) ==
        ENTROPY_ENABLED)
        return RNS_OK;
    if (!atomic_compare_exchange_strong_explicit(
            &entropy_state, &expected, ENTROPY_ENABLING,
            memory_order_acq_rel, memory_order_acquire))
        return RNS_ERROR_INVALID_STATE;
    bootloader_random_enable();
    atomic_store_explicit(&entropy_state, ENTROPY_ENABLED, memory_order_release);
    return RNS_OK;
}

void rns_esp_entropy_disable(void) {
    int expected = ENTROPY_ENABLED;
    if (!atomic_compare_exchange_strong_explicit(
            &entropy_state, &expected, ENTROPY_DISABLING,
            memory_order_acq_rel, memory_order_acquire))
        return;
    bootloader_random_disable();
    atomic_store_explicit(&entropy_state, ENTROPY_DISABLED,
                          memory_order_release);
}

rns_status_t rns_esp_wallclock_set_ms(uint64_t milliseconds) {
    struct timeval value;
    if (milliseconds < RNS_ESP_WALLCLOCK_MIN_MS)
        return RNS_ERROR_INVALID_ARGUMENT;
    if (milliseconds / 1000U > (uint64_t)INT64_MAX)
        return RNS_ERROR_OVERFLOW;
    value.tv_sec = (time_t)(milliseconds / 1000U);
    value.tv_usec = (suseconds_t)((milliseconds % 1000U) * 1000U);
    if (settimeofday(&value, NULL) != 0) return RNS_ERROR_IO;
    atomic_store_explicit(&wallclock_valid, 1, memory_order_release);
    return RNS_OK;
}
