/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef RETICULUM_BOARDS_HELTEC_WIFI_LORA_32_V3_1_H
#define RETICULUM_BOARDS_HELTEC_WIFI_LORA_32_V3_1_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    RNS_HELTEC_V3_1_GPIO_PRG = 0,
    RNS_HELTEC_V3_1_GPIO_RADIO_NSS = 8,
    RNS_HELTEC_V3_1_GPIO_RADIO_SCK = 9,
    RNS_HELTEC_V3_1_GPIO_RADIO_MOSI = 10,
    RNS_HELTEC_V3_1_GPIO_RADIO_MISO = 11,
    RNS_HELTEC_V3_1_GPIO_RADIO_RESET = 12,
    RNS_HELTEC_V3_1_GPIO_RADIO_BUSY = 13,
    RNS_HELTEC_V3_1_GPIO_RADIO_DIO1 = 14,
    RNS_HELTEC_V3_1_GPIO_OLED_SDA = 17,
    RNS_HELTEC_V3_1_GPIO_OLED_SCL = 18,
    RNS_HELTEC_V3_1_GPIO_OLED_RESET = 21,
    RNS_HELTEC_V3_1_GPIO_LED = 35,
    RNS_HELTEC_V3_1_GPIO_VEXT = 36,
    RNS_HELTEC_V3_1_GPIO_UART_TX = 43,
    RNS_HELTEC_V3_1_GPIO_UART_RX = 44,
    RNS_HELTEC_V3_1_OLED_ADDRESS = 0x3c,
    RNS_HELTEC_V3_1_OLED_WIDTH = 128,
    RNS_HELTEC_V3_1_OLED_HEIGHT = 64,
    RNS_HELTEC_V3_1_TCXO_MILLIVOLTS = 1800,
    RNS_HELTEC_V3_1_TCXO_STARTUP_US = 5000,
    RNS_HELTEC_V3_1_VEXT_SETTLE_US = 10000,
    RNS_HELTEC_V3_1_OLED_RESET_US = 20000
};

typedef enum {
    RNS_HELTEC_V3_1_BATTERY_ADC_NONE = 0,
    RNS_HELTEC_V3_1_BATTERY_ADC_UNVALIDATED = 1,
    RNS_HELTEC_V3_1_BATTERY_ADC_VALIDATED = 2
} rns_heltec_v3_1_battery_adc_state_t;

typedef struct {
    const char *name;
    int radio_nss;
    int radio_sck;
    int radio_mosi;
    int radio_miso;
    int radio_reset;
    int radio_busy;
    int radio_dio1;
    bool radio_dio2_rf_switch;
    bool radio_dio3_tcxo;
    uint16_t radio_tcxo_millivolts;
    uint32_t radio_tcxo_startup_us;
    int oled_sda;
    int oled_scl;
    int oled_reset;
    uint8_t oled_address;
    uint16_t oled_width;
    uint16_t oled_height;
    int led;
    int vext;
    bool vext_active_low;
    int console_uart;
    int uart_tx;
    int uart_rx;
    uint32_t console_baud;
    bool has_psram;
    rns_heltec_v3_1_battery_adc_state_t battery_adc_state;
    int battery_adc_gpio;
} rns_heltec_v3_1_board_t;

typedef struct {
    void *context;
    bool (*configure_output)(void *context, int gpio, bool initial_high);
    bool (*set_level)(void *context, int gpio, bool high);
    void (*delay_us)(void *context, uint32_t duration_us);
} rns_heltec_v3_1_gpio_ops_t;

const rns_heltec_v3_1_board_t *rns_heltec_v3_1_board(void);
bool rns_heltec_v3_1_board_valid(const rns_heltec_v3_1_board_t *board);

/* Power/reset only the display domain. Radio startup remains independent. */
bool rns_heltec_v3_1_prepare_oled(const rns_heltec_v3_1_gpio_ops_t *ops);

#ifdef __cplusplus
}
#endif
#endif
