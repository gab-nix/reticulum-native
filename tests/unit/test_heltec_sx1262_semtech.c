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
  log_t l = {.status_byte = 0x24U, .sync_readback_valid = true};
  rns_sx1262_config_t c;
  const rns_sx1262_chip_ops_t *o = rns_sx1262_semtech_chip_ops();
  int f = 0;
  rns_sx1262_default_config(&c);
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
  f += ck(has(&l, 0x0d, (const uint8_t[]){8, 0xac, 0x96}, 3),
          "boosted receive gain");
  f += ck(o->start_tx(&l, &c, (const uint8_t[]){0xaa, 0xbb}, 2, 5000) ==
                  RNS_OK &&
              has(&l, 0x0e, (const uint8_t[]){0, 0xaa, 0xbb}, 3),
          "TX buffer");
  f += ck(o->start_rx(&l, &c) == RNS_OK, "continuous RX");
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
    log_t initial_xosc = {.status_byte = 0x24U,
                          .sync_readback_valid = true,
                          .initial_errors = SX126X_ERRORS_XOSC_START};
    rns_sx1262_default_config(&c);
    f += ck(o->configure(&initial_xosc, &c) == RNS_OK &&
                has(&initial_xosc, 0x07, (const uint8_t[]){0, 0}, 2),
            "stale startup XOSC error cleared before calibration");
  }
  {
    log_t bad_health = {.status_byte = 0x24U,
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
  return f ? 1 : 0;
}
