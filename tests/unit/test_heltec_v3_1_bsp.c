#include "reticulum/boards/heltec_wifi_lora_32_v3_1.h"

#include <assert.h>
#include <stddef.h>

typedef struct {
    int gpio[5];
    bool high[5];
    size_t gpio_count;
    uint32_t delays[3];
    size_t delay_count;
} fake_gpio_t;

static bool configure_output(void *context, int gpio, bool initial_high) {
    fake_gpio_t *fake = context;
    assert(fake->gpio_count < 5U);
    fake->gpio[fake->gpio_count] = gpio;
    fake->high[fake->gpio_count++] = initial_high;
    return true;
}

static bool set_level(void *context, int gpio, bool high) {
    fake_gpio_t *fake = context;
    assert(fake->gpio_count < 5U);
    fake->gpio[fake->gpio_count] = gpio;
    fake->high[fake->gpio_count++] = high;
    return true;
}

static void delay_us(void *context, uint32_t duration_us) {
    fake_gpio_t *fake = context;
    assert(fake->delay_count < 3U);
    fake->delays[fake->delay_count++] = duration_us;
}

int main(void) {
    const rns_heltec_v3_1_board_t *board = rns_heltec_v3_1_board();
    fake_gpio_t fake = {0};
    rns_heltec_v3_1_gpio_ops_t ops = {
        .context = &fake,
        .configure_output = configure_output,
        .set_level = set_level,
        .delay_us = delay_us
    };

    assert(board == rns_heltec_v3_1_board());
    assert(rns_heltec_v3_1_board_valid(board));
    assert(board->radio_nss == 8 && board->radio_sck == 9);
    assert(board->radio_mosi == 10 && board->radio_miso == 11);
    assert(board->radio_reset == 12 && board->radio_busy == 13);
    assert(board->radio_dio1 == 14);
    assert(board->radio_dio2_rf_switch && board->radio_dio3_tcxo);
    assert(board->radio_tcxo_millivolts == 1800U);
    assert(board->radio_tcxo_startup_us >= 5000U);
    assert(board->oled_sda == 17 && board->oled_scl == 18);
    assert(board->oled_reset == 21 && board->led == 35);
    assert(board->vext == 36 && board->vext_active_low);
    assert(board->console_uart == 0 && board->console_baud == 115200U);
    assert(board->uart_tx == 43 && board->uart_rx == 44);
    assert(!board->has_psram);
    assert(board->battery_adc_state == RNS_HELTEC_V3_1_BATTERY_ADC_UNVALIDATED);
    assert(board->battery_adc_gpio == -1);

    assert(rns_heltec_v3_1_prepare_oled(&ops));
    assert(fake.gpio_count == 5U);
    assert(fake.gpio[0] == 36 && fake.high[0]);
    assert(fake.gpio[1] == 36 && !fake.high[1]);
    assert(fake.gpio[2] == 21 && fake.high[2]);
    assert(fake.gpio[3] == 21 && !fake.high[3]);
    assert(fake.gpio[4] == 21 && fake.high[4]);
    assert(fake.delay_count == 3U);
    assert(fake.delays[0] == RNS_HELTEC_V3_1_VEXT_SETTLE_US);
    assert(fake.delays[1] == RNS_HELTEC_V3_1_OLED_RESET_US);
    assert(fake.delays[2] == RNS_HELTEC_V3_1_OLED_RESET_US);
    assert(!rns_heltec_v3_1_prepare_oled(NULL));
    return 0;
}
