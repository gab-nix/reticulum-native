#include "reticulum/heltec_sx1262.h"
#include "sx126x.h"
#include "sx126x_hal.h"
#include <stdio.h>
#include <string.h>
typedef struct {
  uint8_t cmd[64][16];
  uint16_t len[64];
  size_t count;
  unsigned resets;
  uint8_t status_byte;
  bool sync_readback_valid;
  sx126x_errors_mask_t initial_errors;
  unsigned device_error_reads;
} log_t;
sx126x_hal_status_t sx126x_hal_write(const void *c, const uint8_t *cmd,
                                     uint16_t clen, const uint8_t *d,
                                     uint16_t dlen) {
  log_t *l = (void *)c;
  size_t n = (size_t)clen + dlen;
  if (!l || !cmd || n > 16 || l->count >= 64 || (dlen && !d))
    return SX126X_HAL_STATUS_ERROR;
  memcpy(l->cmd[l->count], cmd, clen);
  if (dlen)
    memcpy(l->cmd[l->count] + clen, d, dlen);
  l->len[l->count++] = (uint16_t)n;
  return SX126X_HAL_STATUS_OK;
}
sx126x_hal_status_t sx126x_hal_read(const void *c, const uint8_t *cmd,
                                    uint16_t clen, uint8_t *d, uint16_t dlen) {
  log_t *l = (void *)c;
  if (sx126x_hal_write(c, cmd, clen, NULL, 0) != SX126X_HAL_STATUS_OK)
    return SX126X_HAL_STATUS_ERROR;
  if (d) {
    memset(d, 0, dlen);
    if (cmd[0] == 0xc0 && dlen == 1)
      d[0] = l->status_byte;
    if (cmd[0] == 0x17 && dlen == 2) {
      const sx126x_errors_mask_t errors =
          l->device_error_reads++ == 0U ? l->initial_errors : 0U;
      d[0] = (uint8_t)(errors >> 8U);
      d[1] = (uint8_t)errors;
    }
    if (cmd[0] == 0x1d && clen >= 3 && cmd[1] == 0x07U &&
        cmd[2] == 0x40U && dlen == 2 && l->sync_readback_valid) {
      d[0] = 0x14U;
      d[1] = 0x24U;
    }
  }
  return SX126X_HAL_STATUS_OK;
}
sx126x_hal_status_t sx126x_hal_reset(const void *c) {
  log_t *l = (void *)c;
  if (!l)
    return SX126X_HAL_STATUS_ERROR;
  l->resets++;
  return SX126X_HAL_STATUS_OK;
}
sx126x_hal_status_t sx126x_hal_wakeup(const void *c) {
  return c ? SX126X_HAL_STATUS_OK : SX126X_HAL_STATUS_ERROR;
}
static int has(const log_t *l, uint8_t op, const uint8_t *s, size_t n) {
  for (size_t i = 0; i < l->count; i++)
    if (l->len[i] == n + 1 && l->cmd[i][0] == op &&
        !memcmp(l->cmd[i] + 1, s, n))
      return 1;
  return 0;
}
static int ck(int ok, const char *m) {
  if (!ok)
    fprintf(stderr, "FAIL: %s\n", m);
  return ok ? 0 : 1;
}
int main(void) {
  log_t l = {.status_byte = 0x34U, .sync_readback_valid = true};
  rns_sx1262_config_t c;
  const rns_sx1262_chip_ops_t *o = rns_sx1262_semtech_chip_ops();
  int f = 0;
  rns_sx1262_default_config(&c);
  {
    sx126x_pkt_params_lora_t packet = {
        18U, SX126X_LORA_PKT_EXPLICIT, 255U, true, false};
    sx126x_mod_params_lora_t modem = {
        SX126X_LORA_SF5, SX126X_LORA_BW_125, SX126X_LORA_CR_4_5, 0U};
    uint32_t portable_ms = 0;
    c.spreading_factor = 5U;
    f += ck(rns_sx1262_lora_airtime_ms(&c, 255U, &portable_ms) == RNS_OK &&
                portable_ms ==
                    sx126x_get_lora_time_on_air_in_ms(&packet, &modem),
            "SF5 airtime matches pinned Semtech calculation");
    c.spreading_factor = 6U;
    modem.sf = SX126X_LORA_SF6;
    f += ck(rns_sx1262_lora_airtime_ms(&c, 255U, &portable_ms) == RNS_OK &&
                portable_ms ==
                    sx126x_get_lora_time_on_air_in_ms(&packet, &modem),
            "SF6 airtime matches pinned Semtech calculation");
    c.spreading_factor = 12U;
    c.crc_enabled = false;
    modem.sf = SX126X_LORA_SF12;
    modem.ldro = 1U;
    packet.pld_len_in_bytes = 1U;
    packet.crc_is_on = false;
    f += ck(rns_sx1262_lora_airtime_ms(&c, 1U, &portable_ms) == RNS_OK &&
                portable_ms ==
                    sx126x_get_lora_time_on_air_in_ms(&packet, &modem),
            "short SF12 airtime matches pinned signed calculation");
    rns_sx1262_default_config(&c);
  }
  f += ck(o->reset(&l) == RNS_OK && l.resets == 1, "reset");
  f += ck(o->configure(&l, &c) == RNS_OK, "configure");
  f += ck(has(&l, 0x97, (const uint8_t[]){2, 0, 1, 0x40}, 4), "DIO3 1.8V 5ms");
  f += ck(has(&l, 0x9d, (const uint8_t[]){1}, 1), "DIO2 RF switch");
  f += ck(has(&l, 0x8b, (const uint8_t[]){8, 4, 1, 0}, 4), "SF8 BW125 CR4/5");
  f += ck(has(&l, 0x8c, (const uint8_t[]){0, 18, 0, 255, 1, 0}, 6),
          "packet params");
  f += ck(has(&l, 0x98, (const uint8_t[]){0xd7, 0xdb}, 2),
          "EU868 image calibration");
  f += ck(has(&l, 0x0d, (const uint8_t[]){7, 0x40, 0x14, 0x24}, 4),
          "RNode sync word");
  f += ck(has(&l, 0x0d, (const uint8_t[]){8, 0xe7, 0x28}, 3), "100 mA OCP");
  f += ck(has(&l, 0x8e, (const uint8_t[]){14, 2}, 2), "power/ramp");
  f += ck(has(&l, 0x08,
              (const uint8_t[]){0x03, 0xe3, 0x03, 0xe3, 0, 0, 0, 0}, 8),
          "TX RX CAD and error IRQ routing");
  f += ck(has(&l, 0x0d, (const uint8_t[]){8, 0xac, 0x96}, 3),
          "boosted receive gain");
  f += ck(o->start_tx(&l, &c, (const uint8_t[]){0xaa, 0xbb}, 2, 5000) ==
                  RNS_OK &&
              has(&l, 0x0e, (const uint8_t[]){0, 0xaa, 0xbb}, 3),
          "TX buffer");
  l.status_byte = 0x54U;
  f += ck(o->start_rx(&l, &c) == RNS_OK, "continuous RX");
  l.status_byte = 0x34U;
  f += ck(o->start_cad(&l, &c) == RNS_OK &&
              has(&l, 0x88, (const uint8_t[]){2, 21, 10, 0, 0, 0, 0}, 7) &&
              has(&l, 0xc5, (const uint8_t[]){0}, 0),
          "asynchronous four-symbol CAD");
  l.count = 0;
  c.frequency_hz = 915000000U;
  c.bandwidth_hz = 62500U;
  c.spreading_factor = 12U;
  c.coding_rate_denominator = 8U;
  c.preamble_symbols = 23U;
  c.crc_enabled = false;
  c.invert_iq = true;
  f += ck(o->configure(&l, &c) == RNS_OK, "configurable LoRa profile");
  f += ck(has(&l, 0x8b, (const uint8_t[]){12, 3, 4, 1}, 4),
          "SF12 BW62.5 CR4/8 LDRO");
  f += ck(has(&l, 0x8c, (const uint8_t[]){0, 23, 0, 255, 0, 1}, 6),
          "configured preamble CRC and IQ");
  f += ck(has(&l, 0x98, (const uint8_t[]){0xe1, 0xe9}, 2),
          "frequency-specific image calibration");
  c.bandwidth_hz = 12345U;
  f += ck(o->configure(&l, &c) == RNS_ERROR_INVALID_ARGUMENT,
          "unsupported bandwidth rejected");
  {
    log_t initial_xosc = {.status_byte = 0x34U,
                          .sync_readback_valid = true,
                          .initial_errors = SX126X_ERRORS_XOSC_START};
    rns_sx1262_default_config(&c);
    f += ck(o->configure(&initial_xosc, &c) == RNS_OK &&
                has(&initial_xosc, 0x07, (const uint8_t[]){0, 0}, 2),
            "stale startup XOSC error cleared before calibration");
  }
  {
    log_t bad_health = {.status_byte = 0x34U,
                        .sync_readback_valid = true,
                        .initial_errors = SX126X_ERRORS_PLL_LOCK};
    rns_sx1262_default_config(&c);
    f += ck(o->configure(&bad_health, &c) == RNS_ERROR_PROTOCOL,
            "unexpected initial device error rejected");
  }
  {
    log_t disconnected = {.status_byte = 0U, .sync_readback_valid = false};
    rns_sx1262_default_config(&c);
    f += ck(o->configure(&disconnected, &c) == RNS_ERROR_PROTOCOL,
            "invalid chip status rejected");
  }
  for (unsigned mode = 0; mode < 8; ++mode) {
    for (unsigned cmd = 0; cmd < 8; ++cmd) {
      log_t sample = {.status_byte = (uint8_t)((mode << 4U) | (cmd << 1U)),
                      .sync_readback_valid = true};
      bool non_error = cmd == 1U || cmd == 2U || cmd == 6U;
      rns_sx1262_default_config(&c);
      f += ck((o->configure(&sample, &c) == RNS_OK) ==
              (mode == 3U && non_error), "standby mode/status matrix");
      sample.count = 0;
      f += ck((o->start_rx(&sample, &c) == RNS_OK) ==
              (mode == 5U && non_error), "RX mode/status matrix");
    }
  }
  {
    log_t broken_readback = {.status_byte = 0x32U, .sync_readback_valid = false};
    f += ck(o->configure(&broken_readback, &c) == RNS_ERROR_PROTOCOL,
            "RFU command status cannot bypass register readback");
  }
  return f ? 1 : 0;
}
