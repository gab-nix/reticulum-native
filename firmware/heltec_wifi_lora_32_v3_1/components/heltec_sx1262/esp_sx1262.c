/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "reticulum/boards/heltec_wifi_lora_32_v3_1.h"
#include "reticulum/heltec_sx1262.h"
#include "sx1262_bus.h"
#include "sx126x_hal.h"
#include <stdlib.h>
enum {
  MAGIC = 0x52313236U,
  SPI_HZ = 8000000,
  MAX_TRANSFER = RNS_SX1262_MAX_PAYLOAD + 4,
  OWNER_STACK = 4096,
  OWNER_PRIORITY = 8,
  OWNER_POLL_MS = 100,
  OWNER_STOP_TIMEOUT_MS = 2000,
  OWNER_STOP_BIT = 1U << 0,
  OWNER_EXIT_BIT = 1U << 1,
  OWNER_RUN_BIT = 1U << 2
};
struct rns_heltec_sx1262 {
  uint32_t magic;
  spi_device_handle_t spi;
  rns_sx1262_bus_t bus;
  rns_sx1262_radio_t *radio;
  SemaphoreHandle_t mutex;
  EventGroupHandle_t events;
  TaskHandle_t owner;
  bool bus_owned;
  bool isr_registered;
  rns_status_t owner_status;
};
static bool valid(const struct rns_heltec_sx1262 *d) {
  return d && d->magic == MAGIC && d->spi;
}
static bool busy(void *c) {
  (void)c;
  return gpio_get_level(RNS_HELTEC_V3_1_GPIO_RADIO_BUSY) != 0;
}
static uint64_t now_us(void *c) {
  (void)c;
  return (uint64_t)esp_timer_get_time();
}
static void delay_us(void *c, uint32_t n) {
  (void)c;
  esp_rom_delay_us(n);
}
static rns_status_t spi_transfer(void *c, const uint8_t *tx, uint8_t *rx,
                                 size_t n) {
  struct rns_heltec_sx1262 *d = c;
  spi_transaction_t t = {0};
  if (!valid(d) || !tx || !rx || !n || n > MAX_TRANSFER)
    return RNS_ERROR_INVALID_ARGUMENT;
  t.length = n * 8U;
  t.tx_buffer = tx;
  t.rx_buffer = rx;
  return spi_device_polling_transmit(d->spi, &t) == ESP_OK ? RNS_OK
                                                           : RNS_ERROR_IO;
}
static rns_status_t set_reset(void *c, bool high) {
  (void)c;
  return gpio_set_level(RNS_HELTEC_V3_1_GPIO_RADIO_RESET, high ? 1 : 0) ==
                 ESP_OK
             ? RNS_OK
             : RNS_ERROR_IO;
}
sx126x_hal_status_t sx126x_hal_write(const void *c, const uint8_t *cmd,
                                     uint16_t clen, const uint8_t *d,
                                     uint16_t dlen) {
  struct rns_heltec_sx1262 *dev = (void *)c;
  return valid(dev) && rns_sx1262_bus_transfer(&dev->bus, cmd, clen, d, NULL,
                                               dlen) == RNS_OK
             ? SX126X_HAL_STATUS_OK
             : SX126X_HAL_STATUS_ERROR;
}
sx126x_hal_status_t sx126x_hal_read(const void *c, const uint8_t *cmd,
                                    uint16_t clen, uint8_t *d, uint16_t dlen) {
  struct rns_heltec_sx1262 *dev = (void *)c;
  return valid(dev) && rns_sx1262_bus_transfer(&dev->bus, cmd, clen, NULL, d,
                                               dlen) == RNS_OK
             ? SX126X_HAL_STATUS_OK
             : SX126X_HAL_STATUS_ERROR;
}
sx126x_hal_status_t sx126x_hal_reset(const void *c) {
  struct rns_heltec_sx1262 *dev = (void *)c;
  return valid(dev) && rns_sx1262_bus_reset(&dev->bus) == RNS_OK
             ? SX126X_HAL_STATUS_OK
             : SX126X_HAL_STATUS_ERROR;
}
sx126x_hal_status_t sx126x_hal_wakeup(const void *c) {
  static const uint8_t cmd[] = {0xc0, 0};
  struct rns_heltec_sx1262 *dev = (void *)c;
  return valid(dev) && rns_sx1262_bus_transfer(&dev->bus, cmd, sizeof(cmd),
                                               NULL, NULL, 0) == RNS_OK
             ? SX126X_HAL_STATUS_OK
             : SX126X_HAL_STATUS_ERROR;
}
static void dio1_isr(void *c) {
  struct rns_heltec_sx1262 *d = c;
  BaseType_t w = pdFALSE;
  if (d && d->owner) {
    vTaskNotifyGiveFromISR(d->owner, &w);
    if (w == pdTRUE)
      portYIELD_FROM_ISR();
  }
}
static void owner_task(void *c) {
  struct rns_heltec_sx1262 *d = c;
  for (;;) {
    (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(OWNER_POLL_MS));
    if ((xEventGroupGetBits(d->events) & OWNER_STOP_BIT) != 0U)
      break;
    if ((xEventGroupGetBits(d->events) & OWNER_RUN_BIT) == 0U)
      continue;
    if (xSemaphoreTake(d->mutex, portMAX_DELAY) == pdTRUE) {
      d->owner_status =
          rns_sx1262_radio_poll(d->radio, RNS_SX1262_RX_QUEUE_CAPACITY);
      xSemaphoreGive(d->mutex);
    }
  }
  xEventGroupSetBits(d->events, OWNER_EXIT_BIT);
  vTaskDelete(NULL);
}
static rns_status_t gpio_setup(void) {
  gpio_config_t out = {.pin_bit_mask = 1ULL << RNS_HELTEC_V3_1_GPIO_RADIO_RESET,
                       .mode = GPIO_MODE_OUTPUT,
                       .intr_type = GPIO_INTR_DISABLE};
  gpio_config_t busy_input = {.pin_bit_mask =
                                  1ULL << RNS_HELTEC_V3_1_GPIO_RADIO_BUSY,
                              .mode = GPIO_MODE_INPUT,
                              .intr_type = GPIO_INTR_DISABLE};
  gpio_config_t dio1_input = {.pin_bit_mask =
                                  1ULL << RNS_HELTEC_V3_1_GPIO_RADIO_DIO1,
                              .mode = GPIO_MODE_INPUT,
                              .intr_type = GPIO_INTR_POSEDGE};
  return gpio_config(&out) == ESP_OK &&
                 gpio_set_level(RNS_HELTEC_V3_1_GPIO_RADIO_RESET, 1) ==
                     ESP_OK &&
                 gpio_config(&busy_input) == ESP_OK &&
                 gpio_config(&dio1_input) == ESP_OK
             ? RNS_OK
             : RNS_ERROR_IO;
}
static rns_status_t spi_setup(struct rns_heltec_sx1262 *d) {
  spi_bus_config_t b = {.mosi_io_num = RNS_HELTEC_V3_1_GPIO_RADIO_MOSI,
                        .miso_io_num = RNS_HELTEC_V3_1_GPIO_RADIO_MISO,
                        .sclk_io_num = RNS_HELTEC_V3_1_GPIO_RADIO_SCK,
                        .quadwp_io_num = -1,
                        .quadhd_io_num = -1,
                        .max_transfer_sz = MAX_TRANSFER};
  spi_device_interface_config_t v = {.clock_speed_hz = SPI_HZ,
                                     .mode = 0,
                                     .spics_io_num =
                                         RNS_HELTEC_V3_1_GPIO_RADIO_NSS,
                                     .queue_size = 1};
  if (spi_bus_initialize(SPI2_HOST, &b, SPI_DMA_CH_AUTO) != ESP_OK)
    return RNS_ERROR_IO;
  d->bus_owned = true;
  return spi_bus_add_device(SPI2_HOST, &v, &d->spi) == ESP_OK ? RNS_OK
                                                              : RNS_ERROR_IO;
}
rns_status_t rns_heltec_sx1262_open_with_config(
    const rns_sx1262_config_t *cfg, rns_heltec_sx1262_t **out) {
  struct rns_heltec_sx1262 *d;
  rns_status_t s;
  esp_err_t e;
  if (!cfg || !out)
    return RNS_ERROR_INVALID_ARGUMENT;
  *out = NULL;
  d = calloc(1, sizeof(*d));
  if (!d)
    return RNS_ERROR_NO_MEMORY;
  d->magic = MAGIC;
  d->mutex = xSemaphoreCreateMutex();
  d->events = xEventGroupCreate();
  if (!d->mutex || !d->events) {
    rns_heltec_sx1262_close(d);
    return RNS_ERROR_NO_MEMORY;
  }
  d->bus = (rns_sx1262_bus_t){
      {d, busy, now_us, delay_us, spi_transfer, set_reset}, 100000U};
  s = gpio_setup();
  if (s == RNS_OK)
    s = spi_setup(d);
  if (s == RNS_OK)
    s = rns_sx1262_radio_create(rns_sx1262_semtech_chip_ops(), d, &d->radio);
  /* Do not advertise an IRAM-safe service: this ISR calls the FreeRTOS
     notification API and performs no SPI, allocation or radio callbacks. */
  e = s == RNS_OK ? gpio_install_isr_service(0) : ESP_FAIL;
  if (e == ESP_ERR_INVALID_STATE)
    e = ESP_OK;
  if (s == RNS_OK && e != ESP_OK)
    s = RNS_ERROR_IO;
  if (s == RNS_OK && gpio_isr_handler_add(RNS_HELTEC_V3_1_GPIO_RADIO_DIO1,
                                          dio1_isr, d) != ESP_OK)
    s = RNS_ERROR_IO;
  else if (s == RNS_OK)
    d->isr_registered = true;
  if (s == RNS_OK && xTaskCreate(owner_task, "sx1262", OWNER_STACK, d,
                                 OWNER_PRIORITY, &d->owner) != pdPASS)
    s = RNS_ERROR_NO_MEMORY;
  if (s == RNS_OK) {
    d->bus.busy_timeout_us = cfg->busy_timeout_us;
    s = rns_sx1262_radio_start(d->radio, cfg);
  }
  if (s == RNS_OK) {
    xEventGroupSetBits(d->events, OWNER_RUN_BIT);
    xTaskNotifyGive(d->owner);
  }
  if (s != RNS_OK) {
    rns_heltec_sx1262_close(d);
    return s;
  }
  *out = d;
  return RNS_OK;
}
rns_status_t rns_heltec_sx1262_open(rns_heltec_sx1262_t **out) {
  rns_sx1262_config_t cfg;
  rns_sx1262_default_config(&cfg);
  return rns_heltec_sx1262_open_with_config(&cfg, out);
}
rns_status_t rns_heltec_sx1262_send(rns_heltec_sx1262_t *d, const uint8_t *p,
                                    size_t n) {
  rns_status_t s;
  if (!valid(d))
    return RNS_ERROR_INVALID_ARGUMENT;
  if (xSemaphoreTake(d->mutex, portMAX_DELAY) != pdTRUE)
    return RNS_ERROR_IO;
  s = rns_sx1262_radio_send(d->radio, p, n);
  xSemaphoreGive(d->mutex);
  if (s == RNS_OK)
    xTaskNotifyGive(d->owner);
  return s;
}
rns_status_t rns_heltec_sx1262_receive(rns_heltec_sx1262_t *d,
                                       rns_sx1262_packet_t *p) {
  rns_status_t s;
  if (!valid(d))
    return RNS_ERROR_INVALID_ARGUMENT;
  if (xSemaphoreTake(d->mutex, portMAX_DELAY) != pdTRUE)
    return RNS_ERROR_IO;
  s = rns_sx1262_radio_receive(d->radio, p);
  xSemaphoreGive(d->mutex);
  return s;
}
rns_status_t rns_heltec_sx1262_get_stats(rns_heltec_sx1262_t *d,
                                         rns_sx1262_stats_t *s) {
  rns_status_t v;
  if (!valid(d))
    return RNS_ERROR_INVALID_ARGUMENT;
  if (xSemaphoreTake(d->mutex, portMAX_DELAY) != pdTRUE)
    return RNS_ERROR_IO;
  v = rns_sx1262_radio_get_stats(d->radio, s);
  if (v == RNS_OK && d->owner_status != RNS_OK)
    s->last_error = d->owner_status;
  xSemaphoreGive(d->mutex);
  return v;
}
rns_status_t rns_heltec_sx1262_close(rns_heltec_sx1262_t *d) {
  rns_status_t status = RNS_OK;
  if (!d || d->magic != MAGIC)
    return RNS_ERROR_INVALID_ARGUMENT;
  if (d->isr_registered) {
    if (gpio_isr_handler_remove(RNS_HELTEC_V3_1_GPIO_RADIO_DIO1) != ESP_OK)
      status = RNS_ERROR_IO;
    d->isr_registered = false;
  }
  if (d->owner && d->events) {
    xEventGroupSetBits(d->events, OWNER_STOP_BIT);
    xTaskNotifyGive(d->owner);
    if ((xEventGroupWaitBits(d->events, OWNER_EXIT_BIT, pdFALSE, pdTRUE,
                             pdMS_TO_TICKS(OWNER_STOP_TIMEOUT_MS)) &
         OWNER_EXIT_BIT) == 0U)
      return RNS_ERROR_TIMEOUT;
    d->owner = NULL;
  }
  if (d->radio) {
    const rns_status_t stop_status = rns_sx1262_radio_destroy(d->radio);
    if (stop_status != RNS_OK) {
      if (set_reset(d, false) != RNS_OK && status == RNS_OK)
        status = RNS_ERROR_IO;
      if (status == RNS_OK)
        status = stop_status;
    }
    d->radio = NULL;
  }
  if (d->spi) {
    if (spi_bus_remove_device(d->spi) != ESP_OK)
      status = RNS_ERROR_IO;
    d->spi = NULL;
  }
  if (d->bus_owned) {
    if (spi_bus_free(SPI2_HOST) != ESP_OK)
      status = RNS_ERROR_IO;
    d->bus_owned = false;
  }
  if (d->events)
    vEventGroupDelete(d->events);
  if (d->mutex)
    vSemaphoreDelete(d->mutex);
  d->magic = 0;
  free(d);
  return status;
}
