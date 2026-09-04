/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "reticulum/boards/heltec_wifi_lora_32_v3_1.h"

#include <stddef.h>

static const rns_heltec_v3_1_board_t BOARD = {
    .name = "Heltec WiFi LoRa 32 V3.1",
    .radio_nss = RNS_HELTEC_V3_1_GPIO_RADIO_NSS,
    .radio_sck = RNS_HELTEC_V3_1_GPIO_RADIO_SCK,
    .radio_mosi = RNS_HELTEC_V3_1_GPIO_RADIO_MOSI,
    .radio_miso = RNS_HELTEC_V3_1_GPIO_RADIO_MISO,
    .radio_reset = RNS_HELTEC_V3_1_GPIO_RADIO_RESET,
    .radio_busy = RNS_HELTEC_V3_1_GPIO_RADIO_BUSY,
    .radio_dio1 = RNS_HELTEC_V3_1_GPIO_RADIO_DIO1,
    .radio_dio2_rf_switch = true,
    .radio_dio3_tcxo = true,
    .radio_tcxo_millivolts = RNS_HELTEC_V3_1_TCXO_MILLIVOLTS,
    .radio_tcxo_startup_us = RNS_HELTEC_V3_1_TCXO_STARTUP_US,
    .oled_sda = RNS_HELTEC_V3_1_GPIO_OLED_SDA,
    .oled_scl = RNS_HELTEC_V3_1_GPIO_OLED_SCL,
    .oled_reset = RNS_HELTEC_V3_1_GPIO_OLED_RESET,
    .oled_address = RNS_HELTEC_V3_1_OLED_ADDRESS,
    .oled_width = RNS_HELTEC_V3_1_OLED_WIDTH,
    .oled_height = RNS_HELTEC_V3_1_OLED_HEIGHT,
    .led = RNS_HELTEC_V3_1_GPIO_LED,
    .vext = RNS_HELTEC_V3_1_GPIO_VEXT,
    .vext_active_low = true,
    .console_uart = 0,
    .uart_tx = RNS_HELTEC_V3_1_GPIO_UART_TX,
    .uart_rx = RNS_HELTEC_V3_1_GPIO_UART_RX,
    .console_baud = 115200U,
    .has_psram = false,
    .battery_adc_state = RNS_HELTEC_V3_1_BATTERY_ADC_UNVALIDATED,
    .battery_adc_gpio = -1
};

_Static_assert(RNS_HELTEC_V3_1_GPIO_RADIO_NSS != RNS_HELTEC_V3_1_GPIO_OLED_SDA,
               "radio and OLED pins must not overlap");
_Static_assert(RNS_HELTEC_V3_1_GPIO_VEXT != RNS_HELTEC_V3_1_GPIO_LED,
               "Vext and LED pins must not overlap");
_Static_assert(RNS_HELTEC_V3_1_TCXO_STARTUP_US >= 5000,
               "SX1262 TCXO startup must be at least 5 ms");

const rns_heltec_v3_1_board_t *rns_heltec_v3_1_board(void) { return &BOARD; }

bool rns_heltec_v3_1_board_valid(const rns_heltec_v3_1_board_t *board) {
    if (board == NULL || board->name == NULL) return false;
    return board->radio_nss == RNS_HELTEC_V3_1_GPIO_RADIO_NSS &&
           board->radio_sck == RNS_HELTEC_V3_1_GPIO_RADIO_SCK &&
           board->radio_mosi == RNS_HELTEC_V3_1_GPIO_RADIO_MOSI &&
           board->radio_miso == RNS_HELTEC_V3_1_GPIO_RADIO_MISO &&
           board->radio_reset == RNS_HELTEC_V3_1_GPIO_RADIO_RESET &&
           board->radio_busy == RNS_HELTEC_V3_1_GPIO_RADIO_BUSY &&
           board->radio_dio1 == RNS_HELTEC_V3_1_GPIO_RADIO_DIO1 &&
           board->radio_dio2_rf_switch && board->radio_dio3_tcxo &&
           board->radio_tcxo_millivolts == RNS_HELTEC_V3_1_TCXO_MILLIVOLTS &&
           board->radio_tcxo_startup_us >= RNS_HELTEC_V3_1_TCXO_STARTUP_US &&
           board->oled_sda == RNS_HELTEC_V3_1_GPIO_OLED_SDA &&
           board->oled_scl == RNS_HELTEC_V3_1_GPIO_OLED_SCL &&
           board->oled_reset == RNS_HELTEC_V3_1_GPIO_OLED_RESET &&
           board->oled_address == RNS_HELTEC_V3_1_OLED_ADDRESS &&
           board->oled_width == RNS_HELTEC_V3_1_OLED_WIDTH &&
           board->oled_height == RNS_HELTEC_V3_1_OLED_HEIGHT &&
           board->vext == RNS_HELTEC_V3_1_GPIO_VEXT && board->vext_active_low &&
           board->console_uart == 0 && board->uart_tx == RNS_HELTEC_V3_1_GPIO_UART_TX &&
           board->uart_rx == RNS_HELTEC_V3_1_GPIO_UART_RX &&
           board->console_baud == 115200U && !board->has_psram &&
           board->battery_adc_state == RNS_HELTEC_V3_1_BATTERY_ADC_UNVALIDATED &&
           board->battery_adc_gpio == -1;
}

bool rns_heltec_v3_1_prepare_oled(const rns_heltec_v3_1_gpio_ops_t *ops) {
    if (ops == NULL || ops->configure_output == NULL ||
        ops->set_level == NULL || ops->delay_us == NULL) return false;
    if (!ops->configure_output(ops->context, RNS_HELTEC_V3_1_GPIO_VEXT, true) ||
        !ops->set_level(ops->context, RNS_HELTEC_V3_1_GPIO_VEXT, false)) return false;
    ops->delay_us(ops->context, RNS_HELTEC_V3_1_VEXT_SETTLE_US);
    if (!ops->configure_output(ops->context, RNS_HELTEC_V3_1_GPIO_OLED_RESET, true) ||
        !ops->set_level(ops->context, RNS_HELTEC_V3_1_GPIO_OLED_RESET, false)) return false;
    ops->delay_us(ops->context, RNS_HELTEC_V3_1_OLED_RESET_US);
    if (!ops->set_level(ops->context, RNS_HELTEC_V3_1_GPIO_OLED_RESET, true)) return false;
    ops->delay_us(ops->context, RNS_HELTEC_V3_1_OLED_RESET_US);
    return true;
}
