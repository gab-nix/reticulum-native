#include "reticulum/boards/heltec_status_ui.h"

#include <assert.h>
#include <string.h>

typedef struct {
    int gpio[5];
    bool level[5];
    size_t gpio_count;
    uint32_t delay[3];
    size_t delay_count;
    size_t command_bytes;
    size_t data_bytes;
    int bus_sda;
    int bus_scl;
    uint8_t bus_address;
    bool fail_gpio;
    bool fail_write;
    unsigned network_state;
} fake_display_t;

static bool configure_output(void *context, int gpio, bool initial_high) {
    fake_display_t *fake = context;
    if (fake->fail_gpio) return false;
    assert(fake->gpio_count < 5U);
    fake->gpio[fake->gpio_count] = gpio;
    fake->level[fake->gpio_count++] = initial_high;
    return true;
}

static bool set_level(void *context, int gpio, bool high) {
    fake_display_t *fake = context;
    if (fake->fail_gpio) return false;
    assert(fake->gpio_count < 5U);
    fake->gpio[fake->gpio_count] = gpio;
    fake->level[fake->gpio_count++] = high;
    return true;
}

static void delay_us(void *context, uint32_t delay) {
    fake_display_t *fake = context;
    assert(fake->delay_count < 3U);
    fake->delay[fake->delay_count++] = delay;
}

static bool write_command(void *context, const uint8_t *data, size_t length) {
    fake_display_t *fake = context;
    assert(data != NULL && length > 0U);
    if (fake->fail_write) return false;
    fake->command_bytes += length;
    return true;
}

static bool configure_bus(void *context, int sda_gpio, int scl_gpio,
                          uint8_t address) {
    fake_display_t *fake = context;
    if (fake->fail_write) return false;
    fake->bus_sda = sda_gpio;
    fake->bus_scl = scl_gpio;
    fake->bus_address = address;
    return true;
}

static bool write_data(void *context, const uint8_t *data, size_t length) {
    fake_display_t *fake = context;
    assert(data != NULL && length == RNS_HELTEC_OLED_FRAME_BYTES);
    if (fake->fail_write) return false;
    fake->data_bytes += length;
    return true;
}

static rns_heltec_oled_ops_t make_ops(fake_display_t *fake) {
    rns_heltec_oled_ops_t ops = {
        .context = fake,
        .gpio = {
            .context = fake,
            .configure_output = configure_output,
            .set_level = set_level,
            .delay_us = delay_us
        },
        .configure_bus = configure_bus,
        .write_command = write_command,
        .write_data = write_data
    };
    return ops;
}

int main(void) {
    fake_display_t fake = {.network_state = 0x55aaU};
    rns_heltec_oled_ops_t ops = make_ops(&fake);
    rns_heltec_oled_settings_t settings;
    rns_heltec_oled_t oled;

    rns_heltec_oled_settings_default(&settings);
    assert(settings.enabled && settings.brightness == 0x7fU);
    assert(settings.preview_timeout_ms == 10000U);
    assert(rns_heltec_oled_init(&oled, &ops, &settings));
    assert(rns_heltec_oled_is_ready(&oled));
    assert(fake.gpio_count == 5U && fake.gpio[0] == 36 && fake.gpio[2] == 21);
    assert(fake.bus_sda == 17 && fake.bus_scl == 18 && fake.bus_address == 0x3cU);
    assert(fake.delay_count == 3U &&
           fake.delay[0] == RNS_HELTEC_V3_1_VEXT_SETTLE_US);

    rns_heltec_oled_set_status(&oled, "RX READY", "a1b2c3d4", 4U, 7U, 2U);
    assert(rns_heltec_oled_render(&oled));
    assert(fake.data_bytes == RNS_HELTEC_OLED_FRAME_BYTES);
    assert(!oled.dirty);

    const uint8_t valid[] = {'h', 'i', ' ', 0xe2U, 0x98U, 0x83U};
    assert(rns_heltec_oled_show_preview(&oled, valid, sizeof(valid), 100U));
    assert(strcmp(oled.model.preview, "hi \xe2\x98\x83") == 0);
    rns_heltec_oled_poll(&oled, 10099U);
    assert(oled.model.preview[0] != '\0');
    rns_heltec_oled_poll(&oled, 10100U);
    assert(oled.model.preview[0] == '\0');

    const uint8_t invalid[] = {0xe2U, '(', 0xa1U};
    assert(!rns_heltec_oled_show_preview(&oled, invalid, sizeof(invalid), 200U));
    assert(strcmp(oled.model.preview, "?(?") == 0);
    uint8_t boundary[RNS_HELTEC_OLED_PREVIEW_MAX + 2];
    memset(boundary, 'x', sizeof(boundary));
    boundary[RNS_HELTEC_OLED_PREVIEW_MAX - 1U] = 0xe2U;
    boundary[RNS_HELTEC_OLED_PREVIEW_MAX] = 0x98U;
    boundary[RNS_HELTEC_OLED_PREVIEW_MAX + 1U] = 0x83U;
    assert(rns_heltec_oled_show_preview(&oled, boundary, sizeof(boundary), 300U));
    assert(strlen(oled.model.preview) == RNS_HELTEC_OLED_PREVIEW_MAX - 1U);

    size_t writes_before_disable = fake.data_bytes;
    settings.enabled = false;
    settings.brightness = 0x22U;
    settings.screen = RNS_HELTEC_OLED_SCREEN_MESSAGE;
    rns_heltec_oled_set_settings(&oled, &settings);
    assert(rns_heltec_oled_render(&oled));
    assert(fake.data_bytes == writes_before_disable);

    settings.enabled = true;
    rns_heltec_oled_set_settings(&oled, &settings);
    fake.fail_write = true;
    assert(!rns_heltec_oled_render(&oled));
    assert(rns_heltec_oled_has_failed(&oled));
    assert(fake.network_state == 0x55aaU);

    fake_display_t gpio_failure = {.fail_gpio = true, .network_state = 0x1234U};
    ops = make_ops(&gpio_failure);
    assert(!rns_heltec_oled_init(&oled, &ops, &settings));
    assert(rns_heltec_oled_has_failed(&oled));
    assert(gpio_failure.network_state == 0x1234U);
    return 0;
}
