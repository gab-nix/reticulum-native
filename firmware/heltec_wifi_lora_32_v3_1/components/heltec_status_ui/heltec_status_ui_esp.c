/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "reticulum/boards/heltec_status_ui_esp.h"

#include <stdlib.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

enum { OLED_CHUNK_BYTES = 128, OLED_TIMEOUT_MS = 100 };

struct rns_heltec_oled_esp {
    rns_heltec_oled_t core;
    i2c_master_bus_handle_t bus;
    i2c_master_dev_handle_t device;
    bool vext_owned;
    bool reset_owned;
};

static bool configure_output(void *context, int pin, bool high) {
    rns_heltec_oled_esp_t *handle = context;
    if (pin != RNS_HELTEC_V3_1_GPIO_VEXT &&
        pin != RNS_HELTEC_V3_1_GPIO_OLED_RESET) return false;
    gpio_num_t gpio = (gpio_num_t)pin;
    /* Preload the output latch before enabling the output driver. */
    if (gpio_set_level(gpio, high ? 1U : 0U) != ESP_OK) return false;
    gpio_config_t config = {
        .pin_bit_mask = UINT64_C(1) << (unsigned)pin,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };
    if (gpio_config(&config) != ESP_OK) return false;
    if (pin == RNS_HELTEC_V3_1_GPIO_VEXT) handle->vext_owned = true;
    else handle->reset_owned = true;
    return true;
}

static bool set_level(void *context, int pin, bool high) {
    rns_heltec_oled_esp_t *handle = context;
    if ((pin == RNS_HELTEC_V3_1_GPIO_VEXT && handle->vext_owned) ||
        (pin == RNS_HELTEC_V3_1_GPIO_OLED_RESET && handle->reset_owned))
        return gpio_set_level((gpio_num_t)pin, high ? 1U : 0U) == ESP_OK;
    return false;
}

static void delay_us(void *context, uint32_t duration_us) {
    (void)context;
    /* These board delays are at least 10 ms. Round up and add one tick so
     * entering just before a tick cannot shorten the minimum settling time. */
    if (duration_us >= 1000U) {
        uint32_t milliseconds = duration_us / 1000U + (duration_us % 1000U != 0U);
        vTaskDelay(pdMS_TO_TICKS(milliseconds) + 1U);
    } else {
        esp_rom_delay_us(duration_us);
    }
}

static bool configure_bus(void *context, int sda, int scl, uint8_t address) {
    rns_heltec_oled_esp_t *handle = context;
    if (handle->bus != NULL || sda != RNS_HELTEC_V3_1_GPIO_OLED_SDA ||
        scl != RNS_HELTEC_V3_1_GPIO_OLED_SCL ||
        address != RNS_HELTEC_V3_1_OLED_ADDRESS) return false;
    i2c_master_bus_config_t bus = {
        .i2c_port = -1,
        .sda_io_num = (gpio_num_t)sda,
        .scl_io_num = (gpio_num_t)scl,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true
    };
    if (i2c_new_master_bus(&bus, &handle->bus) != ESP_OK) return false;
    i2c_device_config_t device = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = address,
        .scl_speed_hz = 100000
    };
    return i2c_master_bus_add_device(handle->bus, &device, &handle->device) == ESP_OK;
}

static bool write_bytes(void *context, uint8_t control,
                        const uint8_t *data, size_t length) {
    rns_heltec_oled_esp_t *handle = context;
    if (handle->device == NULL || data == NULL || length == 0U ||
        length > RNS_HELTEC_OLED_FRAME_BYTES) return false;
    uint8_t transfer[OLED_CHUNK_BYTES + 1U];
    transfer[0] = control;
    for (size_t offset = 0U; offset < length;) {
        size_t chunk = length - offset;
        if (chunk > OLED_CHUNK_BYTES) chunk = OLED_CHUNK_BYTES;
        memcpy(transfer + 1U, data + offset, chunk);
        if (i2c_master_transmit(handle->device, transfer, chunk + 1U,
                                OLED_TIMEOUT_MS) != ESP_OK) return false;
        offset += chunk;
    }
    return true;
}

static bool write_command(void *context, const uint8_t *data, size_t length) {
    return write_bytes(context, 0x00U, data, length);
}

static bool write_data(void *context, const uint8_t *data, size_t length) {
    return write_bytes(context, 0x40U, data, length);
}

bool rns_heltec_oled_esp_open(rns_heltec_oled_esp_t **output) {
    if (output == NULL) return false;
    *output = NULL;
    rns_heltec_oled_esp_t *handle = calloc(1U, sizeof(*handle));
    if (handle == NULL) return false;
    rns_heltec_oled_ops_t ops = {
        .context = handle,
        .gpio = { .context = handle, .configure_output = configure_output,
                  .set_level = set_level, .delay_us = delay_us },
        .configure_bus = configure_bus,
        .write_command = write_command,
        .write_data = write_data
    };
    rns_heltec_oled_settings_t settings;
    rns_heltec_oled_settings_default(&settings);
    if (!rns_heltec_oled_init(&handle->core, &ops, &settings)) {
        rns_heltec_oled_esp_close(handle);
        return false;
    }
    *output = handle;
    return true;
}

rns_heltec_oled_t *rns_heltec_oled_esp_core(rns_heltec_oled_esp_t *handle) {
    return handle == NULL ? NULL : &handle->core;
}

void rns_heltec_oled_esp_close(rns_heltec_oled_esp_t *handle) {
    if (handle == NULL) return;
    if (handle->device != NULL) (void)i2c_master_bus_rm_device(handle->device);
    if (handle->bus != NULL) (void)i2c_del_master_bus(handle->bus);
    if (handle->reset_owned)
        (void)gpio_set_level(RNS_HELTEC_V3_1_GPIO_OLED_RESET, 0U);
    /* Keep Vext actively off rather than floating when releasing the object. */
    if (handle->vext_owned) (void)gpio_set_level(RNS_HELTEC_V3_1_GPIO_VEXT, 1U);
    free(handle);
}
