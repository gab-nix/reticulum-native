/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "reticulum/heltec_sx1262.h"
#include "sx126x.h"
#ifdef ESP_PLATFORM
#include "esp_log.h"
#endif
static rns_status_t protocol_error(const char *stage, unsigned value) {
#ifdef ESP_PLATFORM
  ESP_LOGE("sx1262", "Initialization check %s failed: 0x%x", stage, value);
#else
  (void)stage;
  (void)value;
#endif
  return RNS_ERROR_PROTOCOL;
}
static rns_status_t mapped(sx126x_status_t s) {
  switch (s) {
  case SX126X_STATUS_OK:
    return RNS_OK;
  case SX126X_STATUS_UNSUPPORTED_FEATURE:
    return RNS_ERROR_UNSUPPORTED;
  case SX126X_STATUS_UNKNOWN_VALUE:
    return RNS_ERROR_INVALID_ARGUMENT;
  case SX126X_STATUS_ERROR:
  default:
    return RNS_ERROR_IO;
  }
}
static rns_status_t reset(void *c) { return mapped(sx126x_reset(c)); }
static bool lora_bandwidth(uint32_t hz, sx126x_lora_bw_t *out) {
  static const struct {
    uint32_t hz;
    sx126x_lora_bw_t value;
  } bands[] = {{7800U, SX126X_LORA_BW_007},
               {10400U, SX126X_LORA_BW_010},
               {15600U, SX126X_LORA_BW_015},
               {20800U, SX126X_LORA_BW_020},
               {31250U, SX126X_LORA_BW_031},
               {41700U, SX126X_LORA_BW_041},
               {62500U, SX126X_LORA_BW_062},
               {125000U, SX126X_LORA_BW_125},
               {250000U, SX126X_LORA_BW_250},
               {500000U, SX126X_LORA_BW_500}};
  size_t i;
  if (!out)
    return false;
  for (i = 0; i < sizeof(bands) / sizeof(bands[0]); ++i) {
    if (bands[i].hz == hz) {
      *out = bands[i].value;
      return true;
    }
  }
  return false;
}
static void image_calibration_band(uint32_t hz, uint8_t *low, uint8_t *high) {
  if (hz > 900000000U) {
    *low = 0xe1U;
    *high = 0xe9U;
  } else if (hz > 850000000U) {
    *low = 0xd7U;
    *high = 0xdbU;
  } else if (hz > 770000000U) {
    *low = 0xc1U;
    *high = 0xc5U;
  } else if (hz > 460000000U) {
    *low = 0x75U;
    *high = 0x81U;
  } else {
    *low = 0x6bU;
    *high = 0x6fU;
  }
}
static bool command_status_valid(const sx126x_chip_status_t *status,
                                 sx126x_chip_modes_t expected_mode) {
  /* V3.1 hardware reports command=1 after SetStandby. That field is RFU,
     not a documented error; never use it alone as evidence of health.
     Require the commanded mode plus the configuration error/readback checks. */
  return status && status->chip_mode == expected_mode &&
         (status->cmd_status == SX126X_CMD_STATUS_RFU ||
          status->cmd_status == SX126X_CMD_STATUS_DATA_AVAILABLE ||
          status->cmd_status == SX126X_CMD_STATUS_CMD_TX_DONE);
}
static sx126x_pkt_params_lora_t packet_params(const rns_sx1262_config_t *cfg,
                                              uint8_t payload_size) {
  return (sx126x_pkt_params_lora_t){cfg->preamble_symbols,
                                    SX126X_LORA_PKT_EXPLICIT, payload_size,
                                    cfg->crc_enabled, cfg->invert_iq};
}
static rns_status_t configure(void *c, const rns_sx1262_config_t *cfg) {
  sx126x_lora_bw_t bandwidth;
  sx126x_mod_params_lora_t mod;
  sx126x_pkt_params_lora_t pkt;
  sx126x_pa_cfg_params_t pa = {0x04U, 0x07U, 0x00U, 0x01U};
  sx126x_errors_mask_t errors = 0;
  sx126x_chip_status_t status = {0};
  uint8_t cal_low = 0, cal_high = 0;
  uint8_t sync_word[2] = {0};
  sx126x_status_t s;
  if (!c || !cfg || cfg->frequency_hz <= 425000000U ||
      cfg->frequency_hz > 960000000U || cfg->spreading_factor < 5U ||
      cfg->spreading_factor > 12U || cfg->coding_rate_denominator < 5U ||
      cfg->coding_rate_denominator > 8U || cfg->preamble_symbols == 0U ||
      !lora_bandwidth(cfg->bandwidth_hz, &bandwidth))
    return RNS_ERROR_INVALID_ARGUMENT;
  mod = (sx126x_mod_params_lora_t){
      (sx126x_lora_sf_t)cfg->spreading_factor, bandwidth,
      (sx126x_lora_cr_t)(cfg->coding_rate_denominator - 4U),
      ((uint64_t)1U << cfg->spreading_factor) * 1000000ULL >=
              (uint64_t)cfg->bandwidth_hz * 16000ULL
          ? 1U
          : 0U};
  pkt = packet_params(cfg, RNS_SX1262_MAX_PAYLOAD);
  image_calibration_band(cfg->frequency_hz, &cal_low, &cal_high);
  s = sx126x_set_standby(c, SX126X_STANDBY_CFG_RC);
  if (s == SX126X_STATUS_OK)
    s = sx126x_get_device_errors(c, &errors);
  if (s == SX126X_STATUS_OK &&
      (errors & (sx126x_errors_mask_t)~SX126X_ERRORS_XOSC_START) != 0U)
    return protocol_error("reset-errors", errors);
  if (s == SX126X_STATUS_OK && errors != 0U)
    s = sx126x_clear_device_errors(c);
  if (s == SX126X_STATUS_OK)
    s = sx126x_set_dio3_as_tcxo_ctrl(c, SX126X_TCXO_CTRL_1_8V, 320U);
  if (s == SX126X_STATUS_OK)
    s = sx126x_get_device_errors(c, &errors);
  if (s == SX126X_STATUS_OK && errors != 0U)
    return protocol_error("tcxo-errors", errors);
  if (s == SX126X_STATUS_OK)
    s = sx126x_set_dio2_as_rf_sw_ctrl(c, true);
  if (s == SX126X_STATUS_OK)
    s = sx126x_set_reg_mode(c, SX126X_REG_MODE_DCDC);
  if (s == SX126X_STATUS_OK)
    s = sx126x_cal(c, SX126X_CAL_ALL);
  if (s == SX126X_STATUS_OK)
    s = sx126x_cal_img(c, cal_low, cal_high);
  if (s == SX126X_STATUS_OK)
    s = sx126x_get_device_errors(c, &errors);
  if (s == SX126X_STATUS_OK && errors != 0U)
    return protocol_error("calibration-errors", errors);
  if (s == SX126X_STATUS_OK)
    s = sx126x_set_standby(c, SX126X_STANDBY_CFG_XOSC);
  if (s == SX126X_STATUS_OK)
    s = sx126x_get_status(c, &status);
#ifdef ESP_PLATFORM
  if (s == SX126X_STATUS_OK)
    ESP_LOGI("sx1262", "Standby status: mode=%u command=%u",
             (unsigned)status.chip_mode, (unsigned)status.cmd_status);
#endif
  if (s == SX126X_STATUS_OK &&
      !command_status_valid(&status, SX126X_CHIP_MODE_STBY_XOSC))
    return protocol_error("command-status", (unsigned)status.cmd_status);
  if (s == SX126X_STATUS_OK)
    s = sx126x_set_pkt_type(c, SX126X_PKT_TYPE_LORA);
  if (s == SX126X_STATUS_OK)
    s = sx126x_set_rf_freq(c, cfg->frequency_hz);
  if (s == SX126X_STATUS_OK)
    s = sx126x_set_lora_mod_params(c, &mod);
  if (s == SX126X_STATUS_OK)
    s = sx126x_set_lora_pkt_params(c, &pkt);
  if (s == SX126X_STATUS_OK) {
    const uint8_t sw[] = {0x14U, 0x24U};
    s = sx126x_write_register(c, 0x0740U, sw, sizeof(sw));
  }
  if (s == SX126X_STATUS_OK)
    s = sx126x_read_register(c, 0x0740U, sync_word, sizeof(sync_word));
  if (s == SX126X_STATUS_OK &&
      (sync_word[0] != 0x14U || sync_word[1] != 0x24U))
    return protocol_error("sync-readback", ((unsigned)sync_word[0] << 8U) | sync_word[1]);
  if (s == SX126X_STATUS_OK)
    s = sx126x_set_pa_cfg(c, &pa);
  if (s == SX126X_STATUS_OK)
    s = sx126x_cfg_tx_clamp(c);
  if (s == SX126X_STATUS_OK)
    s = sx126x_set_ocp_value(c, 0x28U);
  if (s == SX126X_STATUS_OK)
    s = sx126x_cfg_rx_boosted(c, true);
  if (s == SX126X_STATUS_OK)
    s = sx126x_set_tx_params(c, cfg->tx_power_dbm, SX126X_RAMP_40_US);
  if (s == SX126X_STATUS_OK)
    s = sx126x_set_buffer_base_address(c, 0U, 0U);
  if (s == SX126X_STATUS_OK) {
    const uint16_t irq = SX126X_IRQ_TX_DONE | SX126X_IRQ_RX_DONE |
                         SX126X_IRQ_HEADER_ERROR | SX126X_IRQ_CRC_ERROR |
                         SX126X_IRQ_CAD_DONE | SX126X_IRQ_CAD_DETECTED |
                         SX126X_IRQ_TIMEOUT;
    s = sx126x_set_dio_irq_params(c, irq, irq, SX126X_IRQ_NONE,
                                  SX126X_IRQ_NONE);
  }
  if (s == SX126X_STATUS_OK)
    s = sx126x_clear_irq_status(c, SX126X_IRQ_ALL);
  return mapped(s);
}
static rns_status_t start_cad(void *c, const rns_sx1262_config_t *cfg) {
  sx126x_cad_params_t params;
  sx126x_status_t s;
  if (!c || !cfg || cfg->spreading_factor < 5U ||
      cfg->spreading_factor > 12U)
    return RNS_ERROR_INVALID_ARGUMENT;
  params = (sx126x_cad_params_t){.cad_symb_nb = SX126X_CAD_04_SYMB,
                                 .cad_detect_peak =
                                     (uint8_t)(cfg->spreading_factor + 13U),
                                 .cad_detect_min = 10U,
                                 .cad_exit_mode = SX126X_CAD_ONLY,
                                 .cad_timeout = 0U};
  s = sx126x_set_standby(c, SX126X_STANDBY_CFG_XOSC);
  if (s == SX126X_STATUS_OK)
    s = sx126x_set_cad_params(c, &params);
  if (s == SX126X_STATUS_OK)
    s = sx126x_set_cad(c);
  return mapped(s);
}
static rns_status_t start_rx(void *c, const rns_sx1262_config_t *cfg) {
  sx126x_pkt_params_lora_t p;
  if (!c || !cfg)
    return RNS_ERROR_INVALID_ARGUMENT;
  p = packet_params(cfg, RNS_SX1262_MAX_PAYLOAD);
  sx126x_status_t s = sx126x_set_lora_pkt_params(c, &p);
  if (s == SX126X_STATUS_OK)
    s = sx126x_set_rx_with_timeout_in_rtc_step(c, SX126X_RX_CONTINUOUS);
  sx126x_chip_status_t status = {0};
  if (s == SX126X_STATUS_OK) s = sx126x_get_status(c, &status);
  if (s == SX126X_STATUS_OK &&
      !command_status_valid(&status, SX126X_CHIP_MODE_RX))
    return protocol_error("rx-mode", ((unsigned)status.chip_mode << 4U) |
                          ((unsigned)status.cmd_status << 1U));
  return mapped(s);
}
static rns_status_t start_tx(void *c, const rns_sx1262_config_t *cfg,
                             const uint8_t *d, size_t n, uint32_t timeout) {
  sx126x_pkt_params_lora_t p;
  sx126x_status_t s;
  if (!c || !cfg || !d || !n || n > RNS_SX1262_MAX_PAYLOAD)
    return RNS_ERROR_INVALID_ARGUMENT;
  p = packet_params(cfg, (uint8_t)n);
  s = sx126x_set_standby(c, SX126X_STANDBY_CFG_XOSC);
  if (s == SX126X_STATUS_OK)
    s = sx126x_set_lora_pkt_params(c, &p);
  if (s == SX126X_STATUS_OK)
    s = sx126x_write_buffer(c, 0U, d, (uint8_t)n);
  if (s == SX126X_STATUS_OK)
    s = sx126x_set_tx(c, timeout);
  return mapped(s);
}
static rns_status_t irq(void *c, uint16_t *out) {
  sx126x_irq_mask_t m = 0;
  sx126x_status_t s;
  if (!out)
    return RNS_ERROR_INVALID_ARGUMENT;
  s = sx126x_get_and_clear_irq_status(c, &m);
  *out = m;
  return mapped(s);
}
static rns_status_t read_packet(void *c, rns_sx1262_packet_t *p) {
  sx126x_rx_buffer_status_t b = {0};
  sx126x_pkt_status_lora_t q = {0};
  sx126x_status_t s;
  if (!p)
    return RNS_ERROR_INVALID_ARGUMENT;
  s = sx126x_get_rx_buffer_status(c, &b);
  if (s == SX126X_STATUS_OK && b.pld_len_in_bytes)
    s = sx126x_read_buffer(c, b.buffer_start_pointer, p->data,
                           b.pld_len_in_bytes);
  if (s == SX126X_STATUS_OK)
    s = sx126x_get_lora_pkt_status(c, &q);
  if (s != SX126X_STATUS_OK || !b.pld_len_in_bytes)
    return RNS_ERROR_IO;
  p->length = b.pld_len_in_bytes;
  p->rssi_dbm = q.rssi_pkt_in_dbm;
  p->snr_db = q.snr_pkt_in_db;
  return RNS_OK;
}
static rns_status_t standby(void *c) {
  return mapped(sx126x_set_standby(c, SX126X_STANDBY_CFG_RC));
}
const rns_sx1262_chip_ops_t *rns_sx1262_semtech_chip_ops(void) {
  static const rns_sx1262_chip_ops_t ops = {
      .reset = reset,
      .configure = configure,
      .start_rx = start_rx,
      .start_cad = start_cad,
      .start_tx = start_tx,
      .get_and_clear_irq = irq,
      .read_packet = read_packet,
      .standby = standby};
  return &ops;
}
