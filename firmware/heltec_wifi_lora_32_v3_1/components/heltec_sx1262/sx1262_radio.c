/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "reticulum/heltec_sx1262.h"
#include <stdlib.h>
#include <string.h>
typedef struct {
  uint8_t data[RNS_SX1262_MAX_PAYLOAD];
  uint8_t length;
} tx_entry_t;
enum { SX1262_MAX_TIMEOUT_MS = 262143U };
struct rns_sx1262_radio {
  const rns_sx1262_chip_ops_t *ops;
  void *ctx;
  rns_sx1262_config_t config;
  rns_sx1262_stats_t stats;
  rns_sx1262_packet_t rx[RNS_SX1262_RX_QUEUE_CAPACITY];
  tx_entry_t tx[RNS_SX1262_TX_QUEUE_CAPACITY];
  size_t rx_head, rx_count, tx_head, tx_count;
};
static bool valid_ops(const rns_sx1262_chip_ops_t *o) {
  return o && o->reset && o->configure && o->start_rx && o->start_tx &&
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
         c->tx_timeout_ms && c->tx_timeout_ms <= SX1262_MAX_TIMEOUT_MS;
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
                               .tx_timeout_ms = 5000U};
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
    return RNS_OK;
  }
  r->stats.command_errors++;
  r->stats.last_error = s;
  r->stats.state = RNS_SX1262_FAULT;
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
rns_status_t rns_sx1262_radio_send(rns_sx1262_radio_t *r, const uint8_t *d,
                                   size_t n) {
  size_t tail;
  if (!r || !d || !n || n > RNS_SX1262_MAX_PAYLOAD)
    return RNS_ERROR_INVALID_ARGUMENT;
  if (r->stats.state == RNS_SX1262_STOPPED ||
      r->stats.state == RNS_SX1262_FAULT)
    return RNS_ERROR_INVALID_STATE;
  if (r->tx_count == RNS_SX1262_TX_QUEUE_CAPACITY) {
    r->stats.tx_overflows++;
    return RNS_ERROR_OVERFLOW;
  }
  tail = (r->tx_head + r->tx_count) % RNS_SX1262_TX_QUEUE_CAPACITY;
  memcpy(r->tx[tail].data, d, n);
  r->tx[tail].length = (uint8_t)n;
  r->tx_count++;
  return RNS_OK;
}
static rns_status_t begin_tx(rns_sx1262_radio_t *r) {
  tx_entry_t *e;
  rns_status_t s;
  if (r->stats.state != RNS_SX1262_RECEIVING || !r->tx_count)
    return RNS_OK;
  e = &r->tx[r->tx_head];
  s = r->ops->start_tx(r->ctx, &r->config, e->data, e->length,
                       r->config.tx_timeout_ms);
  if (s != RNS_OK) {
    r->stats.command_errors++;
    r->stats.last_error = s;
    return recover(r);
  }
  r->stats.state = RNS_SX1262_TRANSMITTING;
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
  if (!r || !budget)
    return RNS_ERROR_INVALID_ARGUMENT;
  if (r->stats.state == RNS_SX1262_STOPPED)
    return RNS_ERROR_INVALID_STATE;
  if (r->stats.state == RNS_SX1262_FAULT) {
    s = recover(r);
    if (s != RNS_OK)
      return s;
  }
  while (handled < budget) {
    uint16_t irq = 0;
    s = r->ops->get_and_clear_irq(r->ctx, &irq);
    if (s != RNS_OK) {
      r->stats.command_errors++;
      r->stats.last_error = s;
      return recover(r);
    }
    if (!irq)
      break;
    handled++;
    if (irq & RNS_SX1262_IRQ_HEADER_ERROR)
      r->stats.header_errors++;
    if (irq & RNS_SX1262_IRQ_CRC_ERROR)
      r->stats.crc_errors++;
    if (irq & RNS_SX1262_IRQ_TIMEOUT) {
      r->stats.timeouts++;
      s = recover(r);
      if (s != RNS_OK)
        return s;
      continue;
    }
    if ((irq & RNS_SX1262_IRQ_RX_DONE) &&
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
        r->stats.state == RNS_SX1262_TRANSMITTING && r->tx_count) {
      tx_entry_t *e = &r->tx[r->tx_head];
      r->stats.tx_packets++;
      r->stats.tx_bytes += e->length;
      r->tx_head = (r->tx_head + 1U) % RNS_SX1262_TX_QUEUE_CAPACITY;
      r->tx_count--;
      s = enter_rx(r);
      if (s != RNS_OK)
        return s;
    }
  }
  return begin_tx(r);
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
  return RNS_OK;
}
rns_status_t rns_sx1262_radio_stop(rns_sx1262_radio_t *r) {
  rns_status_t s;
  if (!r)
    return RNS_ERROR_INVALID_ARGUMENT;
  if (r->stats.state == RNS_SX1262_STOPPED)
    return RNS_OK;
  s = r->ops->standby(r->ctx);
  if (s == RNS_OK) {
    r->stats.state = RNS_SX1262_STOPPED;
    r->rx_count = 0;
    r->tx_count = 0;
  } else {
    r->stats.command_errors++;
    r->stats.last_error = s;
    r->stats.state = RNS_SX1262_FAULT;
  }
  return s;
}
