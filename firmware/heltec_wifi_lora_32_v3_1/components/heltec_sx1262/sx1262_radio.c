/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "reticulum/heltec_sx1262.h"
#include <stdlib.h>
#include <string.h>
typedef struct {
  uint8_t data[RNS_SX1262_MAX_PAYLOAD];
  uint8_t length;
  uint32_t id;
} tx_entry_t;
enum { SX1262_MAX_TIMEOUT_MS = 262143U };
struct rns_sx1262_radio {
  const rns_sx1262_chip_ops_t *ops;
  void *ctx;
  rns_sx1262_config_t config;
  rns_sx1262_stats_t stats;
  rns_sx1262_packet_t rx[RNS_SX1262_RX_QUEUE_CAPACITY];
  tx_entry_t tx[RNS_SX1262_TX_QUEUE_CAPACITY];
  rns_sx1262_tx_result_t tx_results[RNS_SX1262_TX_RESULT_CAPACITY];
  size_t rx_head, rx_count, tx_head, tx_count, tx_result_head,
      tx_result_count;
  uint32_t next_tx_id;
  rns_sx1262_cad_result_t cad_result;
  bool cad_requested, cad_result_pending;
  uint8_t recovery_polls_remaining;
};
static bool valid_ops(const rns_sx1262_chip_ops_t *o) {
  return o && o->reset && o->configure && o->start_rx && o->start_cad &&
         o->start_tx &&
         o->get_and_clear_irq && o->read_packet && o->standby;
}
static bool valid_cfg(const rns_sx1262_config_t *c) {
  const bool valid_bandwidth =
      c && (c->bandwidth_hz == 7800U || c->bandwidth_hz == 10400U ||
            c->bandwidth_hz == 15600U || c->bandwidth_hz == 20800U ||
            c->bandwidth_hz == 31250U || c->bandwidth_hz == 41700U ||
            c->bandwidth_hz == 62500U || c->bandwidth_hz == 125000U ||
            c->bandwidth_hz == 250000U || c->bandwidth_hz == 500000U);
  /* The Semtech reference image-calibration table begins above 425 MHz. */
  return c && c->frequency_hz > 425000000U && c->frequency_hz <= 960000000U &&
         valid_bandwidth && c->spreading_factor >= 5U &&
         c->spreading_factor <= 12U && c->coding_rate_denominator >= 5U &&
         c->coding_rate_denominator <= 8U && c->preamble_symbols > 0U &&
         c->tx_power_dbm >= -9 && c->tx_power_dbm <= 22 && c->busy_timeout_us &&
         c->tx_timeout_margin_ms <= SX1262_MAX_TIMEOUT_MS &&
         c->recovery_backoff_polls > 0U;
}
void rns_sx1262_default_config(rns_sx1262_config_t *c) {
  if (c)
    *c = (rns_sx1262_config_t){.frequency_hz = 868200000U,
                               .bandwidth_hz = 125000U,
                               .spreading_factor = 8U,
                               .coding_rate_denominator = 5U,
                               .preamble_symbols = 18U,
                               .crc_enabled = true,
                               .invert_iq = false,
                               .tx_power_dbm = 14,
                               .busy_timeout_us = 100000U,
                               .tx_timeout_margin_ms = 250U,
                               .recovery_backoff_polls = 5U};
}
rns_status_t rns_sx1262_lora_airtime_ms(const rns_sx1262_config_t *c,
                                        size_t length, uint32_t *out_ms) {
  int64_t signed_ceil_numerator;
  uint64_t ceil_numerator, ceil_denominator, symbols, numerator;
  bool ldro;
  if (!valid_cfg(c) || !length || length > RNS_SX1262_MAX_PAYLOAD || !out_ms)
    return RNS_ERROR_INVALID_ARGUMENT;
  ldro = ((uint64_t)1U << c->spreading_factor) * 1000000ULL >=
         (uint64_t)c->bandwidth_hz * 16000ULL;
  signed_ceil_numerator = (int64_t)(length * 8U) +
                          (c->crc_enabled ? 16 : 0) -
                          (int64_t)(4U * c->spreading_factor) + 20;
  if (c->spreading_factor > 6U)
    signed_ceil_numerator += 8;
  ceil_numerator = signed_ceil_numerator > 0
                       ? (uint64_t)signed_ceil_numerator
                       : 0U;
  ceil_denominator = 4U *
                     (c->spreading_factor > 6U && ldro
                          ? c->spreading_factor - 2U
                          : c->spreading_factor);
  symbols = ((ceil_numerator + ceil_denominator - 1U) / ceil_denominator) *
                c->coding_rate_denominator +
            c->preamble_symbols + 12U;
  if (c->spreading_factor <= 6U)
    symbols += 2U;
  numerator = (4U * symbols + 1U) *
              ((uint64_t)1U << (c->spreading_factor - 2U)) * 1000U;
  *out_ms = (uint32_t)((numerator + c->bandwidth_hz - 1U) /
                       c->bandwidth_hz);
  return RNS_OK;
}
rns_status_t rns_sx1262_radio_create(const rns_sx1262_chip_ops_t *o, void *ctx,
                                     rns_sx1262_radio_t **out) {
  rns_sx1262_radio_t *r;
  if (!valid_ops(o) || !ctx || !out)
    return RNS_ERROR_INVALID_ARGUMENT;
  *out = NULL;
  r = calloc(1U, sizeof(*r));
  if (!r)
    return RNS_ERROR_NO_MEMORY;
  r->ops = o;
  r->ctx = ctx;
  r->stats.state = RNS_SX1262_STOPPED;
  *out = r;
  return RNS_OK;
}
rns_status_t rns_sx1262_radio_destroy(rns_sx1262_radio_t *r) {
  rns_status_t s;
  if (!r)
    return RNS_ERROR_INVALID_ARGUMENT;
  s = rns_sx1262_radio_stop(r);
  memset(r, 0, sizeof(*r));
  free(r);
  return s;
}
static rns_status_t enter_rx(rns_sx1262_radio_t *r) {
  rns_status_t s = r->ops->start_rx(r->ctx, &r->config);
  if (s != RNS_OK) {
    r->stats.command_errors++;
    r->stats.last_error = s;
    r->stats.state = RNS_SX1262_FAULT;
    return s;
  }
  r->stats.state = RNS_SX1262_RECEIVING;
  return RNS_OK;
}
static rns_status_t recover(rns_sx1262_radio_t *r) {
  rns_status_t s = r->ops->reset(r->ctx);
  if (s == RNS_OK)
    s = r->ops->configure(r->ctx, &r->config);
  if (s == RNS_OK)
    s = enter_rx(r);
  if (s == RNS_OK) {
    r->stats.recoveries++;
    r->recovery_polls_remaining = r->config.recovery_backoff_polls;
    return RNS_OK;
  }
  r->stats.command_errors++;
  r->stats.last_error = s;
  r->stats.state = RNS_SX1262_FAULT;
  r->recovery_polls_remaining = r->config.recovery_backoff_polls;
  return s;
}
rns_status_t rns_sx1262_radio_start(rns_sx1262_radio_t *r,
                                    const rns_sx1262_config_t *c) {
  rns_status_t s;
  if (!r || !valid_cfg(c))
    return RNS_ERROR_INVALID_ARGUMENT;
  if (r->stats.state != RNS_SX1262_STOPPED)
    return RNS_ERROR_INVALID_STATE;
  r->config = *c;
  s = r->ops->reset(r->ctx);
  if (s == RNS_OK)
    s = r->ops->configure(r->ctx, c);
  if (s == RNS_OK)
    s = enter_rx(r);
  if (s != RNS_OK) {
    r->stats.command_errors++;
    r->stats.last_error = s;
    r->stats.state = RNS_SX1262_FAULT;
  }
  return s;
}
rns_status_t rns_sx1262_radio_send_with_id(rns_sx1262_radio_t *r,
                                           const uint8_t *d, size_t n,
                                           uint32_t *out_id) {
  size_t tail;
  if (!r || !d || !n || n > RNS_SX1262_MAX_PAYLOAD || !out_id)
    return RNS_ERROR_INVALID_ARGUMENT;
  if (r->stats.state == RNS_SX1262_STOPPED ||
      r->stats.state == RNS_SX1262_FAULT)
    return RNS_ERROR_INVALID_STATE;
  if (r->cad_requested || r->cad_result_pending ||
      r->stats.state == RNS_SX1262_SCANNING)
    return RNS_ERROR_INVALID_STATE;
  if (r->tx_count == RNS_SX1262_TX_QUEUE_CAPACITY) {
    r->stats.tx_overflows++;
    return RNS_ERROR_OVERFLOW;
  }
  /* Every accepted frame reserves one terminal-result slot. This bounds
     memory and prevents a fast producer from making completion data lossy. */
  if (r->tx_count + r->tx_result_count >= RNS_SX1262_TX_RESULT_CAPACITY) {
    r->stats.tx_result_backpressure++;
    return RNS_ERROR_OVERFLOW;
  }
  tail = (r->tx_head + r->tx_count) % RNS_SX1262_TX_QUEUE_CAPACITY;
  memcpy(r->tx[tail].data, d, n);
  r->tx[tail].length = (uint8_t)n;
  r->next_tx_id++;
  if (!r->next_tx_id)
    r->next_tx_id++;
  r->tx[tail].id = r->next_tx_id;
  *out_id = r->next_tx_id;
  r->tx_count++;
  return RNS_OK;
}
static void finish_tx(rns_sx1262_radio_t *r,
                      rns_sx1262_tx_outcome_t outcome, rns_status_t status) {
  const tx_entry_t *e = &r->tx[r->tx_head];
  const size_t tail =
      (r->tx_result_head + r->tx_result_count) % RNS_SX1262_TX_RESULT_CAPACITY;
  /* send_with_id() reserves this slot before accepting the frame. */
  r->tx_results[tail] = (rns_sx1262_tx_result_t){
      .id = e->id, .outcome = outcome, .status = status, .length = e->length};
  r->tx_result_count++;
  if (outcome == RNS_SX1262_TX_SENT) {
    r->stats.tx_packets++;
    r->stats.tx_bytes += e->length;
  } else {
    r->stats.tx_failures++;
  }
  memset(&r->tx[r->tx_head], 0, sizeof(r->tx[r->tx_head]));
  r->tx_head = (r->tx_head + 1U) % RNS_SX1262_TX_QUEUE_CAPACITY;
  r->tx_count--;
}
rns_status_t rns_sx1262_radio_send(rns_sx1262_radio_t *r, const uint8_t *d,
                                   size_t n) {
  uint32_t ignored_id;
  return rns_sx1262_radio_send_with_id(r, d, n, &ignored_id);
}
static rns_status_t begin_tx(rns_sx1262_radio_t *r) {
  tx_entry_t *e;
  uint32_t airtime_ms, timeout_ms;
  rns_status_t s;
  if (r->stats.state != RNS_SX1262_RECEIVING || !r->tx_count ||
      r->cad_requested || r->cad_result_pending || r->recovery_polls_remaining)
    return RNS_OK;
  e = &r->tx[r->tx_head];
  s = rns_sx1262_lora_airtime_ms(&r->config, e->length, &airtime_ms);
  if (s != RNS_OK || airtime_ms > SX1262_MAX_TIMEOUT_MS ||
      r->config.tx_timeout_margin_ms > SX1262_MAX_TIMEOUT_MS - airtime_ms) {
    s = RNS_ERROR_INVALID_ARGUMENT;
    r->stats.command_errors++;
    r->stats.last_error = s;
    finish_tx(r, RNS_SX1262_TX_START_FAILED, s);
    return s;
  }
  timeout_ms = airtime_ms + r->config.tx_timeout_margin_ms;
  s = r->ops->start_tx(r->ctx, &r->config, e->data, e->length,
                       timeout_ms);
  if (s != RNS_OK) {
    r->stats.command_errors++;
    r->stats.last_error = s;
    finish_tx(r, RNS_SX1262_TX_START_FAILED, s);
    return recover(r);
  }
  r->stats.state = RNS_SX1262_TRANSMITTING;
  return RNS_OK;
}
static void finish_cad(rns_sx1262_radio_t *r,
                       rns_sx1262_cad_outcome_t outcome,
                       rns_status_t status) {
  r->cad_result =
      (rns_sx1262_cad_result_t){.outcome = outcome, .status = status};
  r->cad_result_pending = true;
  r->cad_requested = false;
}
static rns_status_t begin_cad(rns_sx1262_radio_t *r) {
  rns_status_t s;
  if (!r->cad_requested || r->stats.state != RNS_SX1262_RECEIVING ||
      r->recovery_polls_remaining)
    return RNS_OK;
  s = r->ops->start_cad(r->ctx, &r->config);
  if (s != RNS_OK) {
    r->stats.command_errors++;
    r->stats.last_error = s;
    finish_cad(r, RNS_SX1262_CAD_FAILED, s);
    return recover(r);
  }
  r->stats.state = RNS_SX1262_SCANNING;
  return RNS_OK;
}
static void enqueue_rx(rns_sx1262_radio_t *r, const rns_sx1262_packet_t *p) {
  size_t tail;
  if (r->rx_count == RNS_SX1262_RX_QUEUE_CAPACITY) {
    r->stats.rx_overflows++;
    return;
  }
  tail = (r->rx_head + r->rx_count) % RNS_SX1262_RX_QUEUE_CAPACITY;
  r->rx[tail] = *p;
  r->rx_count++;
  r->stats.rx_packets++;
  r->stats.rx_bytes += p->length;
  r->stats.last_rssi_dbm = p->rssi_dbm;
  r->stats.last_snr_db = p->snr_db;
}
rns_status_t rns_sx1262_radio_poll(rns_sx1262_radio_t *r, size_t budget) {
  size_t handled = 0;
  rns_status_t s;
  const bool backoff_at_entry = r && r->recovery_polls_remaining > 0U;
  if (!r || !budget)
    return RNS_ERROR_INVALID_ARGUMENT;
  if (r->stats.state == RNS_SX1262_STOPPED)
    return RNS_ERROR_INVALID_STATE;
  if (r->stats.state == RNS_SX1262_FAULT) {
    if (backoff_at_entry) {
      r->recovery_polls_remaining--;
      return r->stats.last_error;
    }
    s = recover(r);
    if (s != RNS_OK)
      return s;
  }
  while (handled < budget) {
    uint16_t irq = 0;
    rns_sx1262_state_t state_at_irq;
    s = r->ops->get_and_clear_irq(r->ctx, &irq);
    if (s != RNS_OK) {
      r->stats.command_errors++;
      r->stats.last_error = s;
      if (r->stats.state == RNS_SX1262_TRANSMITTING && r->tx_count)
        finish_tx(r, RNS_SX1262_TX_RADIO_FAILED, s);
      if (r->stats.state == RNS_SX1262_SCANNING && r->cad_requested)
        finish_cad(r, RNS_SX1262_CAD_FAILED, s);
      return recover(r);
    }
    if (!irq)
      break;
    /* A combined or stale flag belongs only to the state that owned this IRQ
       read. State transitions below must not make another flag actionable. */
    state_at_irq = r->stats.state;
    handled++;
    if (irq & RNS_SX1262_IRQ_HEADER_ERROR)
      r->stats.header_errors++;
    if (irq & RNS_SX1262_IRQ_CRC_ERROR)
      r->stats.crc_errors++;
    if (irq & RNS_SX1262_IRQ_TIMEOUT) {
      r->stats.timeouts++;
      if (state_at_irq == RNS_SX1262_TRANSMITTING && r->tx_count)
        finish_tx(r, RNS_SX1262_TX_TIMED_OUT, RNS_ERROR_TIMEOUT);
      if (state_at_irq == RNS_SX1262_SCANNING && r->cad_requested)
        finish_cad(r, RNS_SX1262_CAD_FAILED, RNS_ERROR_TIMEOUT);
      s = recover(r);
      if (s != RNS_OK)
        return s;
      continue;
    }
    if ((irq & RNS_SX1262_IRQ_CAD_DONE) &&
        state_at_irq == RNS_SX1262_SCANNING && r->cad_requested) {
      finish_cad(r,
                 (irq & RNS_SX1262_IRQ_CAD_DETECTED)
                     ? RNS_SX1262_CAD_BUSY
                     : RNS_SX1262_CAD_CLEAR,
                 RNS_OK);
      s = enter_rx(r);
      if (s != RNS_OK)
        return s;
    }
    if ((irq & RNS_SX1262_IRQ_RX_DONE) && state_at_irq == RNS_SX1262_RECEIVING &&
        !(irq & (RNS_SX1262_IRQ_HEADER_ERROR | RNS_SX1262_IRQ_CRC_ERROR))) {
      rns_sx1262_packet_t p = {0};
      s = r->ops->read_packet(r->ctx, &p);
      if (s != RNS_OK || !p.length || p.length > RNS_SX1262_MAX_PAYLOAD) {
        r->stats.command_errors++;
        r->stats.last_error = s != RNS_OK ? s : RNS_ERROR_PROTOCOL;
        s = recover(r);
        if (s != RNS_OK)
          return s;
        continue;
      }
      enqueue_rx(r, &p);
    }
    if ((irq & RNS_SX1262_IRQ_TX_DONE) &&
        state_at_irq == RNS_SX1262_TRANSMITTING && r->tx_count) {
      finish_tx(r, RNS_SX1262_TX_SENT, RNS_OK);
      s = enter_rx(r);
      if (s != RNS_OK)
        return s;
    }
  }
  if (backoff_at_entry) {
    r->recovery_polls_remaining--;
    return RNS_OK;
  }
  s = begin_cad(r);
  return s == RNS_OK ? begin_tx(r) : s;
}
rns_status_t rns_sx1262_radio_start_cad(rns_sx1262_radio_t *r) {
  if (!r)
    return RNS_ERROR_INVALID_ARGUMENT;
  if (r->stats.state != RNS_SX1262_RECEIVING || r->cad_requested ||
      r->cad_result_pending || r->tx_count)
    return RNS_ERROR_INVALID_STATE;
  r->cad_requested = true;
  return RNS_OK;
}
rns_status_t rns_sx1262_radio_receive_cad_result(
    rns_sx1262_radio_t *r, rns_sx1262_cad_result_t *result) {
  if (!r || !result)
    return RNS_ERROR_INVALID_ARGUMENT;
  if (!r->cad_result_pending)
    return RNS_ERROR_NOT_FOUND;
  *result = r->cad_result;
  memset(&r->cad_result, 0, sizeof(r->cad_result));
  r->cad_result_pending = false;
  return RNS_OK;
}
rns_status_t rns_sx1262_radio_receive_tx_result(
    rns_sx1262_radio_t *r, rns_sx1262_tx_result_t *result) {
  if (!r || !result)
    return RNS_ERROR_INVALID_ARGUMENT;
  if (!r->tx_result_count)
    return RNS_ERROR_NOT_FOUND;
  *result = r->tx_results[r->tx_result_head];
  memset(&r->tx_results[r->tx_result_head], 0,
         sizeof(r->tx_results[r->tx_result_head]));
  r->tx_result_head =
      (r->tx_result_head + 1U) % RNS_SX1262_TX_RESULT_CAPACITY;
  r->tx_result_count--;
  return RNS_OK;
}
rns_status_t rns_sx1262_radio_receive(rns_sx1262_radio_t *r,
                                      rns_sx1262_packet_t *p) {
  if (!r || !p)
    return RNS_ERROR_INVALID_ARGUMENT;
  if (r->stats.state == RNS_SX1262_STOPPED)
    return RNS_ERROR_INVALID_STATE;
  if (!r->rx_count)
    return RNS_ERROR_NOT_FOUND;
  *p = r->rx[r->rx_head];
  r->rx_head = (r->rx_head + 1U) % RNS_SX1262_RX_QUEUE_CAPACITY;
  r->rx_count--;
  return RNS_OK;
}
rns_status_t rns_sx1262_radio_get_stats(const rns_sx1262_radio_t *r,
                                        rns_sx1262_stats_t *s) {
  if (!r || !s)
    return RNS_ERROR_INVALID_ARGUMENT;
  *s = r->stats;
  s->pending_rx = r->rx_count;
  s->pending_tx = r->tx_count;
  s->pending_tx_results = r->tx_result_count;
  return RNS_OK;
}
rns_status_t rns_sx1262_radio_stop(rns_sx1262_radio_t *r) {
  rns_status_t s;
  if (!r)
    return RNS_ERROR_INVALID_ARGUMENT;
  if (r->stats.state == RNS_SX1262_STOPPED)
    return RNS_OK;
  s = r->ops->standby(r->ctx);
  r->rx_count = 0;
  if (r->cad_requested)
    finish_cad(r, RNS_SX1262_CAD_FAILED,
               s == RNS_OK ? RNS_ERROR_INVALID_STATE : s);
  while (r->tx_count)
    finish_tx(r, s == RNS_OK ? RNS_SX1262_TX_STOPPED
                             : RNS_SX1262_TX_RADIO_FAILED,
              s == RNS_OK ? RNS_ERROR_INVALID_STATE : s);
  if (s == RNS_OK) {
    r->stats.state = RNS_SX1262_STOPPED;
  } else {
    r->stats.command_errors++;
    r->stats.last_error = s;
    r->stats.state = RNS_SX1262_FAULT;
  }
  return s;
}
