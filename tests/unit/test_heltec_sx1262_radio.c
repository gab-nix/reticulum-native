#include "reticulum/heltec_sx1262.h"
#include "reticulum/sx1262_interface.h"
#include "sx1262_bus.h"
#include <stdio.h>
#include <string.h>
typedef struct {
  unsigned resets, configs, rx, tx, reads, standby;
  unsigned cad;
  uint32_t last_tx_timeout_ms;
  uint16_t irqs[40];
  size_t count, index;
  rns_sx1262_packet_t packet;
  bool fail_tx;
  bool fail_cad;
  bool fail_irq;
  bool fail_reset;
  bool fail_standby;
} fake_chip_t;
static rns_status_t reset(void *c) {
  fake_chip_t *f = c;
  f->resets++;
  if (f->fail_reset) {
    f->fail_reset = false;
    return RNS_ERROR_IO;
  }
  return RNS_OK;
}
static rns_status_t config(void *c, const rns_sx1262_config_t *v) {
  ((fake_chip_t *)c)->configs++;
  return v->frequency_hz == 868200000U ? RNS_OK : RNS_ERROR_PROTOCOL;
}
static rns_status_t rx(void *c, const rns_sx1262_config_t *v) {
  ((fake_chip_t *)c)->rx++;
  return v && v->preamble_symbols ? RNS_OK : RNS_ERROR_PROTOCOL;
}
static rns_status_t cad(void *c, const rns_sx1262_config_t *v) {
  fake_chip_t *f = c;
  f->cad++;
  if (f->fail_cad) {
    f->fail_cad = false;
    return RNS_ERROR_IO;
  }
  return v && v->spreading_factor >= 5U ? RNS_OK : RNS_ERROR_PROTOCOL;
}
static rns_status_t tx(void *c, const rns_sx1262_config_t *v, const uint8_t *d,
                       size_t n, uint32_t t) {
  fake_chip_t *f = c;
  f->tx++;
  f->last_tx_timeout_ms = t;
  if (f->fail_tx) {
    f->fail_tx = false;
    return RNS_ERROR_IO;
  }
  return v && v->crc_enabled && d && n && t ? RNS_OK : RNS_ERROR_PROTOCOL;
}
static rns_status_t irq(void *c, uint16_t *out) {
  fake_chip_t *f = c;
  if (f->fail_irq) {
    f->fail_irq = false;
    return RNS_ERROR_IO;
  }
  *out = f->index < f->count ? f->irqs[f->index++] : 0U;
  return RNS_OK;
}
static rns_status_t read_packet(void *c, rns_sx1262_packet_t *out) {
  fake_chip_t *f = c;
  f->reads++;
  *out = f->packet;
  return RNS_OK;
}
static rns_status_t standby(void *c) {
  fake_chip_t *f = c;
  f->standby++;
  return f->fail_standby ? RNS_ERROR_IO : RNS_OK;
}
static const rns_sx1262_chip_ops_t OPS = {.reset = reset,
                                         .configure = config,
                                         .start_rx = rx,
                                         .start_cad = cad,
                                         .start_tx = tx,
                                         .get_and_clear_irq = irq,
                                         .read_packet = read_packet,
                                         .standby = standby};
typedef struct {
  uint64_t now;
  unsigned busy_reads, busy_until, transfers, edges;
  bool stuck_after_transfer;
  bool fail_transfer;
  bool fail_reset;
  uint8_t tx[260];
  size_t len;
} fake_bus_t;
static bool busy(void *c) {
  fake_bus_t *f = c;
  f->busy_reads++;
  if (f->stuck_after_transfer && f->transfers)
    return true;
  return f->busy_reads <= f->busy_until;
}
static uint64_t now(void *c) { return ((fake_bus_t *)c)->now; }
static void delay(void *c, uint32_t d) { ((fake_bus_t *)c)->now += d; }
static rns_status_t transfer(void *c, const uint8_t *txd, uint8_t *r,
                             size_t n) {
  fake_bus_t *f = c;
  f->transfers++;
  f->len = n;
  memcpy(f->tx, txd, n);
  for (size_t i = 0; i < n; ++i)
    r[i] = (uint8_t)(0x80U + i);
  return f->fail_transfer ? RNS_ERROR_IO : RNS_OK;
}
static rns_status_t set_reset(void *c, bool high) {
  fake_bus_t *f = c;
  f->edges = f->edges * 2U + (high ? 1U : 0U);
  return f->fail_reset ? RNS_ERROR_IO : RNS_OK;
}
static int ck(bool ok, const char *m) {
  if (!ok)
    fprintf(stderr, "FAIL: %s\n", m);
  return ok ? 0 : 1;
}
int main(void) {
  fake_chip_t f = {0};
  rns_sx1262_radio_t *r = NULL;
  rns_sx1262_config_t c;
  rns_sx1262_stats_t s;
  rns_sx1262_packet_t p;
  rns_sx1262_tx_result_t result;
  uint32_t tx_id = 0;
  uint32_t airtime_ms = 0;
  const uint8_t d[] = {1, 2, 3};
  int fail = 0;
  rns_sx1262_default_config(&c);
  {
    static const uint32_t bandwidths[] = {7800U,  10400U, 15600U, 20800U,
                                          31250U, 41700U, 62500U, 125000U,
                                          250000U, 500000U};
    static const size_t lengths[] = {1U, 128U, 255U};
    rns_sx1262_scheduler_config_t scheduler;
    size_t bandwidth_index, length_index;
    unsigned sf, cr;
    rns_sx1262_scheduler_default_config(&scheduler);
    for (bandwidth_index = 0U;
         bandwidth_index < sizeof(bandwidths) / sizeof(bandwidths[0]);
         ++bandwidth_index) {
      for (sf = 5U; sf <= 12U; ++sf) {
        for (cr = 5U; cr <= 8U; ++cr) {
          for (length_index = 0U;
               length_index < sizeof(lengths) / sizeof(lengths[0]);
               ++length_index) {
            uint64_t scheduler_us = 0U;
            uint32_t radio_ms = 0U;
            c.bandwidth_hz = bandwidths[bandwidth_index];
            c.spreading_factor = (uint8_t)sf;
            c.coding_rate_denominator = (uint8_t)cr;
            scheduler.bandwidth_hz = bandwidths[bandwidth_index];
            scheduler.spreading_factor = (uint8_t)sf;
            scheduler.coding_rate_denominator = (uint8_t)cr;
            if (rns_sx1262_lora_airtime_ms(&c, lengths[length_index],
                                           &radio_ms) != RNS_OK ||
                rns_sx1262_airtime_us(&scheduler, lengths[length_index],
                                      &scheduler_us) != RNS_OK ||
                radio_ms != (scheduler_us + 999U) / 1000U) {
              fprintf(stderr,
                      "airtime mismatch bw=%u sf=%u cr=%u len=%zu: %u vs %llu us\n",
                      bandwidths[bandwidth_index], sf, cr,
                      lengths[length_index], radio_ms,
                      (unsigned long long)scheduler_us);
              fail++;
            }
          }
        }
      }
    }
    rns_sx1262_default_config(&c);
  }
  fail += ck(c.frequency_hz == 868200000U && c.bandwidth_hz == 125000U &&
                 c.spreading_factor == 8U && c.coding_rate_denominator == 5U &&
                 c.preamble_symbols == 18U && c.crc_enabled && !c.invert_iq &&
                 c.tx_power_dbm == 14,
             "EU868 defaults");
  fail += ck(rns_sx1262_lora_airtime_ms(&c, sizeof(d), &airtime_ms) == RNS_OK &&
                 airtime_ms == 83U && c.tx_timeout_margin_ms == 250U &&
                 c.recovery_backoff_polls == 5U,
             "default frame airtime and recovery policy");
  fail += ck(rns_sx1262_radio_create(&OPS, &f, &r) == RNS_OK, "create");
  fail += ck(rns_sx1262_radio_send(r, d, sizeof(d)) == RNS_ERROR_INVALID_STATE,
             "pre-start send rejected");
  fail += ck(rns_sx1262_radio_start(r, &c) == RNS_OK && f.resets == 1 &&
                 f.configs == 1 && f.rx == 1,
             "start order");
  fail += ck(rns_sx1262_radio_start(r, &c) == RNS_ERROR_INVALID_STATE,
             "double start");
  {
    rns_sx1262_radio_t *invalid_radio = NULL;
    rns_sx1262_config_t invalid = c;
    invalid.spreading_factor = 4U;
    fail += ck(rns_sx1262_radio_create(&OPS, &f, &invalid_radio) == RNS_OK &&
                   rns_sx1262_radio_start(invalid_radio, &invalid) ==
                       RNS_ERROR_INVALID_ARGUMENT,
               "invalid spreading factor rejected");
    rns_sx1262_radio_destroy(invalid_radio);
    invalid_radio = NULL;
    invalid = c;
    invalid.frequency_hz = 425000000U;
    fail += ck(rns_sx1262_radio_create(&OPS, &f, &invalid_radio) == RNS_OK &&
                   rns_sx1262_radio_start(invalid_radio, &invalid) ==
                       RNS_ERROR_INVALID_ARGUMENT,
               "frequency outside image calibration coverage rejected");
    rns_sx1262_radio_destroy(invalid_radio);
    invalid_radio = NULL;
    invalid = c;
    invalid.tx_timeout_margin_ms = 262144U;
    fail += ck(rns_sx1262_radio_create(&OPS, &f, &invalid_radio) == RNS_OK &&
                   rns_sx1262_radio_start(invalid_radio, &invalid) ==
                       RNS_ERROR_INVALID_ARGUMENT,
               "timeout beyond SX1262 RTC range rejected");
    rns_sx1262_radio_destroy(invalid_radio);
  }
  fail += ck(rns_sx1262_radio_send_with_id(r, d, sizeof(d), &tx_id) == RNS_OK &&
                 tx_id != 0U &&
                 rns_sx1262_radio_poll(r, 4) == RNS_OK && f.tx == 1 &&
                 f.last_tx_timeout_ms == 333U,
             "queued TX starts without IRQ notification");
  f.irqs[f.count++] = RNS_SX1262_IRQ_TX_DONE;
  fail += ck(rns_sx1262_radio_poll(r, 4) == RNS_OK, "TX done");
  rns_sx1262_radio_get_stats(r, &s);
  fail += ck(s.tx_packets == 1 && s.tx_bytes == 3 && s.pending_tx == 0 &&
                 s.pending_tx_results == 1U &&
                 s.state == RNS_SX1262_RECEIVING,
             "TX stats");
  fail += ck(rns_sx1262_radio_receive_tx_result(r, &result) == RNS_OK &&
                 result.id == tx_id && result.outcome == RNS_SX1262_TX_SENT &&
                 result.status == RNS_OK && result.length == sizeof(d) &&
                 rns_sx1262_radio_receive_tx_result(r, &result) ==
                     RNS_ERROR_NOT_FOUND,
             "successful TX has one terminal result");
  f.packet.length = 2;
  f.packet.data[0] = 0xaa;
  f.packet.data[1] = 0xbb;
  f.packet.rssi_dbm = -91;
  f.packet.snr_db = 7;
  f.irqs[f.count++] = RNS_SX1262_IRQ_RX_DONE;
  f.irqs[f.count++] = RNS_SX1262_IRQ_RX_DONE | RNS_SX1262_IRQ_CRC_ERROR;
  fail +=
      ck(rns_sx1262_radio_poll(r, 4) == RNS_OK && f.reads == 1, "RX and CRC");
  fail += ck(rns_sx1262_radio_receive(r, &p) == RNS_OK && p.data[1] == 0xbb &&
                 p.rssi_dbm == -91 && p.snr_db == 7,
             "RX metrics");
  f.irqs[f.count++] = RNS_SX1262_IRQ_TIMEOUT;
  fail += ck(rns_sx1262_radio_poll(r, 4) == RNS_OK && f.resets == 2 &&
                 f.configs == 2,
             "timeout recovery");
  rns_sx1262_radio_get_stats(r, &s);
  fail += ck(s.timeouts == 1 && s.recoveries == 1 && s.crc_errors == 1,
             "error stats");
  f.fail_tx = true;
  fail += ck(rns_sx1262_radio_send_with_id(r, d, sizeof(d), &tx_id) == RNS_OK,
             "TX accepted during recovery backoff");
  for (size_t i = 0; i < c.recovery_backoff_polls; ++i)
    fail += ck(rns_sx1262_radio_poll(r, 1U) == RNS_OK && f.tx == 1U,
               "recovery backoff defers next TX attempt");
  fail += ck(rns_sx1262_radio_poll(r, 1) == RNS_OK && f.resets == 3 &&
                 rns_sx1262_radio_receive_tx_result(r, &result) == RNS_OK &&
                 result.id == tx_id &&
                 result.outcome == RNS_SX1262_TX_START_FAILED &&
                 result.status == RNS_ERROR_IO,
             "TX start failure is terminal and recovers RX");
  fail += ck(rns_sx1262_radio_poll(r, 1) == RNS_OK && f.tx == 2 &&
                 rns_sx1262_radio_receive_tx_result(r, &result) ==
                     RNS_ERROR_NOT_FOUND,
             "failed TX is never retried or reported twice");
  fail += ck(rns_sx1262_radio_stop(r) == RNS_OK && f.standby == 1, "stop");
  fail += ck(rns_sx1262_radio_receive(r, &p) == RNS_ERROR_INVALID_STATE,
             "stopped receive");
  rns_sx1262_radio_destroy(r);
  {
    fake_chip_t restart = {0};
    rns_sx1262_radio_t *restart_radio = NULL;
    uint32_t restart_id = 0U;
    rns_sx1262_default_config(&c);
    restart.packet.length = 1U;
    restart.packet.data[0] = 0x7eU;
    fail += ck(rns_sx1262_radio_create(&OPS, &restart, &restart_radio) ==
                       RNS_OK &&
                   rns_sx1262_radio_start(restart_radio, &c) == RNS_OK &&
                   rns_sx1262_radio_send_with_id(restart_radio, d, sizeof(d),
                                                 &restart_id) == RNS_OK &&
                   rns_sx1262_radio_poll(restart_radio, 1U) == RNS_OK,
               "abort/restart fixture begins TX");
    restart.irqs[restart.count++] = RNS_SX1262_IRQ_TX_DONE;
    restart.irqs[restart.count++] = RNS_SX1262_IRQ_RX_DONE;
    fail += ck(rns_sx1262_radio_poll(restart_radio, 2U) == RNS_OK,
               "abort/restart fixture retains bounded results");
    fail += ck(rns_sx1262_radio_abort_and_restart(restart_radio, &c) ==
                       RNS_OK &&
                   rns_sx1262_radio_receive_tx_result(restart_radio,
                                                      &result) ==
                       RNS_ERROR_NOT_FOUND &&
                   rns_sx1262_radio_receive(restart_radio, &p) ==
                       RNS_ERROR_NOT_FOUND,
               "abort/restart clears TX/RX/results and returns to RX");
    rns_sx1262_radio_get_stats(restart_radio, &s);
    fail += ck(s.state == RNS_SX1262_RECEIVING && s.pending_tx == 0U &&
                   s.pending_rx == 0U && s.pending_tx_results == 0U,
               "abort/restart exposes clean receiving state");
    restart.fail_reset = true;
    fail += ck(rns_sx1262_radio_abort_and_restart(restart_radio, &c) ==
                       RNS_ERROR_IO,
               "abort/restart reports reset failure");
    rns_sx1262_radio_get_stats(restart_radio, &s);
    fail += ck(s.state == RNS_SX1262_FAULT && s.pending_tx == 0U &&
                   s.pending_rx == 0U && s.pending_tx_results == 0U,
               "failed restart remains clean and faulted");
    rns_sx1262_radio_destroy(restart_radio);
  }
  {
    fake_chip_t q = {0};
    rns_sx1262_radio_t *queue_radio = NULL;
    rns_sx1262_default_config(&c);
    q.packet.length = 1U;
    q.packet.data[0] = 0x5aU;
    fail += ck(rns_sx1262_radio_create(&OPS, &q, &queue_radio) == RNS_OK &&
                   rns_sx1262_radio_start(queue_radio, &c) == RNS_OK,
               "queue test radio starts");
    for (size_t i = 0; i < RNS_SX1262_TX_QUEUE_CAPACITY; ++i)
      fail += ck(rns_sx1262_radio_send(queue_radio, d, sizeof(d)) == RNS_OK,
                 "TX queue accepts bounded entry");
    fail += ck(rns_sx1262_radio_send(queue_radio, d, sizeof(d)) ==
                       RNS_ERROR_OVERFLOW,
               "TX queue rejects overflow");
    rns_sx1262_radio_get_stats(queue_radio, &s);
    fail += ck(s.pending_tx == RNS_SX1262_TX_QUEUE_CAPACITY &&
                   s.tx_overflows == 1U,
               "TX overflow keeps queued frames");
    for (size_t i = 0; i < RNS_SX1262_RX_QUEUE_CAPACITY + 1U; ++i)
      q.irqs[q.count++] = RNS_SX1262_IRQ_RX_DONE;
    fail += ck(rns_sx1262_radio_poll(queue_radio,
                                     RNS_SX1262_RX_QUEUE_CAPACITY + 1U) ==
                       RNS_OK,
               "RX burst handled");
    rns_sx1262_radio_get_stats(queue_radio, &s);
    fail += ck(s.pending_rx == RNS_SX1262_RX_QUEUE_CAPACITY &&
                   s.rx_overflows == 1U &&
                   s.rx_packets == RNS_SX1262_RX_QUEUE_CAPACITY,
               "RX queue overflow counted without overwrite");
    for (size_t i = 0; i < RNS_SX1262_RX_QUEUE_CAPACITY; ++i)
      fail += ck(rns_sx1262_radio_receive(queue_radio, &p) == RNS_OK &&
                     p.data[0] == 0x5aU,
                 "RX queue contents preserved");
    q.irqs[q.count++] = RNS_SX1262_IRQ_HEADER_ERROR |
                        RNS_SX1262_IRQ_RX_DONE;
    fail += ck(rns_sx1262_radio_poll(queue_radio, 1U) == RNS_OK,
               "header error does not enqueue packet");
    q.fail_irq = true;
    fail += ck(rns_sx1262_radio_poll(queue_radio, 1U) == RNS_OK &&
                   q.resets == 2U,
               "IRQ command failure recovers");
    rns_sx1262_radio_get_stats(queue_radio, &s);
    fail += ck(s.header_errors == 1U && s.pending_rx == 0U &&
                   s.last_error == RNS_ERROR_IO,
               "header and terminal error stats observable");
    rns_sx1262_radio_destroy(queue_radio);
  }
  {
    fake_chip_t scan = {0};
    rns_sx1262_radio_t *scan_radio = NULL;
    rns_sx1262_cad_result_t cad_result;
    rns_sx1262_default_config(&c);
    fail += ck(rns_sx1262_radio_create(&OPS, &scan, &scan_radio) == RNS_OK &&
                   rns_sx1262_radio_start(scan_radio, &c) == RNS_OK,
               "CAD test radio starts");
    fail += ck(rns_sx1262_radio_start_cad(scan_radio) == RNS_OK &&
                   rns_sx1262_radio_send(scan_radio, d, sizeof(d)) ==
                       RNS_ERROR_INVALID_STATE &&
                   rns_sx1262_radio_poll(scan_radio, 1U) == RNS_OK &&
                   scan.cad == 1U,
               "CAD is requested before TX queuing and starts from poll");
    scan.irqs[scan.count++] = RNS_SX1262_IRQ_CAD_DONE;
    fail += ck(rns_sx1262_radio_poll(scan_radio, 1U) == RNS_OK &&
                   rns_sx1262_radio_receive_cad_result(scan_radio,
                                                       &cad_result) == RNS_OK &&
                   cad_result.outcome == RNS_SX1262_CAD_CLEAR &&
                   cad_result.status == RNS_OK,
               "CAD clear terminal result");
    scan.irqs[scan.count++] = RNS_SX1262_IRQ_CAD_DONE |
                              RNS_SX1262_IRQ_CAD_DETECTED;
    fail += ck(rns_sx1262_radio_poll(scan_radio, 1U) == RNS_OK &&
                   rns_sx1262_radio_receive_cad_result(scan_radio,
                                                       &cad_result) ==
                       RNS_ERROR_NOT_FOUND,
               "stale CAD IRQ is ignored");
    fail += ck(rns_sx1262_radio_start_cad(scan_radio) == RNS_OK &&
                   rns_sx1262_radio_poll(scan_radio, 1U) == RNS_OK,
               "second CAD starts");
    fail += ck(rns_sx1262_radio_send(scan_radio, d, sizeof(d)) ==
                       RNS_ERROR_INVALID_STATE,
               "active CAD excludes frame queuing");
    scan.irqs[scan.count++] = RNS_SX1262_IRQ_CAD_DONE |
                              RNS_SX1262_IRQ_CAD_DETECTED;
    fail += ck(rns_sx1262_radio_poll(scan_radio, 1U) == RNS_OK &&
                   rns_sx1262_radio_start_cad(scan_radio) ==
                       RNS_ERROR_INVALID_STATE &&
                   rns_sx1262_radio_receive_cad_result(scan_radio,
                                                       &cad_result) == RNS_OK &&
                   cad_result.outcome == RNS_SX1262_CAD_BUSY,
               "CAD busy result applies consumption backpressure");
    scan.fail_cad = true;
    fail += ck(rns_sx1262_radio_start_cad(scan_radio) == RNS_OK &&
                   rns_sx1262_radio_poll(scan_radio, 1U) == RNS_OK &&
                   rns_sx1262_radio_receive_cad_result(scan_radio,
                                                       &cad_result) == RNS_OK &&
                   cad_result.outcome == RNS_SX1262_CAD_FAILED &&
                   cad_result.status == RNS_ERROR_IO,
               "CAD start failure is terminal and recovers");
    for (size_t i = 0; i < c.recovery_backoff_polls; ++i)
      fail += ck(rns_sx1262_radio_poll(scan_radio, 1U) == RNS_OK,
                 "CAD recovery observes bounded backoff");
    fail += ck(rns_sx1262_radio_start_cad(scan_radio) == RNS_OK &&
                   rns_sx1262_radio_poll(scan_radio, 1U) == RNS_OK,
               "CAD restarts after recovery backoff");
    scan.irqs[scan.count++] = RNS_SX1262_IRQ_TIMEOUT;
    fail += ck(rns_sx1262_radio_poll(scan_radio, 1U) == RNS_OK &&
                   rns_sx1262_radio_receive_cad_result(scan_radio,
                                                       &cad_result) == RNS_OK &&
                   cad_result.outcome == RNS_SX1262_CAD_FAILED &&
                   cad_result.status == RNS_ERROR_TIMEOUT,
               "CAD timeout has one terminal failure result");
    rns_sx1262_radio_destroy(scan_radio);
  }
  {
    fake_chip_t terminal = {0};
    rns_sx1262_radio_t *terminal_radio = NULL;
    uint32_t timeout_id = 0, next_id = 0;
    rns_sx1262_default_config(&c);
    fail += ck(rns_sx1262_radio_create(&OPS, &terminal, &terminal_radio) ==
                       RNS_OK &&
                   rns_sx1262_radio_start(terminal_radio, &c) == RNS_OK,
               "terminal test radio starts");
    fail += ck(rns_sx1262_radio_send_with_id(terminal_radio, d, sizeof(d),
                                             &timeout_id) == RNS_OK &&
                   rns_sx1262_radio_poll(terminal_radio, 1U) == RNS_OK,
               "timeout frame starts");
    terminal.irqs[terminal.count++] =
        RNS_SX1262_IRQ_TIMEOUT | RNS_SX1262_IRQ_TX_DONE;
    fail += ck(rns_sx1262_radio_poll(terminal_radio, 1U) == RNS_OK &&
                   terminal.tx == 1U &&
                   rns_sx1262_radio_receive_tx_result(terminal_radio,
                                                      &result) == RNS_OK &&
                   result.id == timeout_id &&
                   result.outcome == RNS_SX1262_TX_TIMED_OUT &&
                   result.status == RNS_ERROR_TIMEOUT,
               "TX timeout is terminal");
    fail += ck(rns_sx1262_radio_poll(terminal_radio, 1U) == RNS_OK &&
                   terminal.tx == 1U,
               "timed-out frame is not retried");
    terminal.irqs[terminal.count++] = RNS_SX1262_IRQ_TX_DONE;
    fail += ck(rns_sx1262_radio_poll(terminal_radio, 1U) == RNS_OK &&
                   rns_sx1262_radio_receive_tx_result(terminal_radio,
                                                      &result) ==
                       RNS_ERROR_NOT_FOUND,
               "combined timeout wins and stale TX done is ignored");
    fail += ck(rns_sx1262_radio_send_with_id(terminal_radio, d, sizeof(d),
                                             &next_id) == RNS_OK &&
                   next_id != timeout_id,
               "radio queues next frame during timeout backoff");
    for (size_t i = 2U; i < c.recovery_backoff_polls; ++i)
      fail += ck(rns_sx1262_radio_poll(terminal_radio, 1U) == RNS_OK &&
                     terminal.tx == 1U,
                 "timeout recovery backoff prevents immediate retransmit");
    fail += ck(rns_sx1262_radio_poll(terminal_radio, 1U) == RNS_OK &&
                   terminal.tx == 2U,
               "next frame starts after bounded recovery backoff");
    terminal.irqs[terminal.count++] = RNS_SX1262_IRQ_TX_DONE;
    fail += ck(rns_sx1262_radio_poll(terminal_radio, 1U) == RNS_OK &&
                   rns_sx1262_radio_receive_tx_result(terminal_radio,
                                                      &result) == RNS_OK &&
                   result.id == next_id &&
                   result.outcome == RNS_SX1262_TX_SENT,
               "post-timeout frame completes normally");
    rns_sx1262_radio_get_stats(terminal_radio, &s);
    fail += ck(s.tx_packets == 1U && s.tx_failures == 1U && s.timeouts == 1U &&
                   s.pending_tx == 0U && s.pending_tx_results == 0U,
               "terminal TX statistics distinguish success and failure");
    fail += ck(rns_sx1262_radio_send_with_id(terminal_radio, d, sizeof(d),
                                             &next_id) == RNS_OK &&
                   rns_sx1262_radio_poll(terminal_radio, 1U) == RNS_OK,
               "IRQ-failure frame starts");
    terminal.fail_irq = true;
    fail += ck(rns_sx1262_radio_poll(terminal_radio, 1U) == RNS_OK &&
                   terminal.tx == 3U &&
                   rns_sx1262_radio_receive_tx_result(terminal_radio,
                                                      &result) == RNS_OK &&
                   result.id == next_id &&
                   result.outcome == RNS_SX1262_TX_RADIO_FAILED &&
                   result.status == RNS_ERROR_IO,
               "IRQ read failure terminates active TX once");
    fail += ck(rns_sx1262_radio_poll(terminal_radio, 1U) == RNS_OK &&
                   terminal.tx == 3U &&
                   rns_sx1262_radio_receive_tx_result(terminal_radio,
                                                      &result) ==
                       RNS_ERROR_NOT_FOUND,
               "IRQ-failed TX is not retried or duplicated");
    rns_sx1262_radio_destroy(terminal_radio);
  }
  {
    fake_chip_t stale = {0};
    rns_sx1262_radio_t *stale_radio = NULL;
    rns_sx1262_cad_result_t cad_result;
    uint32_t stale_id = 0;
    rns_sx1262_default_config(&c);
    fail += ck(rns_sx1262_radio_create(&OPS, &stale, &stale_radio) == RNS_OK &&
                   rns_sx1262_radio_start(stale_radio, &c) == RNS_OK &&
                   rns_sx1262_radio_send_with_id(stale_radio, d, sizeof(d),
                                                 &stale_id) == RNS_OK &&
                   rns_sx1262_radio_poll(stale_radio, 1U) == RNS_OK,
               "stale IRQ TX starts");
    stale.irqs[stale.count++] = RNS_SX1262_IRQ_RX_DONE;
    fail += ck(rns_sx1262_radio_poll(stale_radio, 1U) == RNS_OK &&
                   stale.reads == 0U && stale.tx == 1U &&
                   rns_sx1262_radio_receive_tx_result(stale_radio, &result) ==
                       RNS_ERROR_NOT_FOUND,
               "stale RX done cannot recover or retry active TX");
    stale.irqs[stale.count++] =
        RNS_SX1262_IRQ_TX_DONE | RNS_SX1262_IRQ_RX_DONE;
    fail += ck(rns_sx1262_radio_poll(stale_radio, 1U) == RNS_OK &&
                   stale.reads == 0U && stale.tx == 1U &&
                   rns_sx1262_radio_receive_tx_result(stale_radio, &result) ==
                       RNS_OK &&
                   result.id == stale_id &&
                   result.outcome == RNS_SX1262_TX_SENT,
               "combined TX and stale RX flags terminate TX only once");
    fail += ck(rns_sx1262_radio_start_cad(stale_radio) == RNS_OK &&
                   rns_sx1262_radio_poll(stale_radio, 1U) == RNS_OK,
               "stale IRQ CAD starts");
    stale.irqs[stale.count++] = RNS_SX1262_IRQ_RX_DONE;
    fail += ck(rns_sx1262_radio_poll(stale_radio, 1U) == RNS_OK &&
                   stale.reads == 0U && stale.cad == 1U &&
                   rns_sx1262_radio_receive_cad_result(stale_radio,
                                                       &cad_result) ==
                       RNS_ERROR_NOT_FOUND,
               "stale RX done cannot recover or restart active CAD");
    stale.irqs[stale.count++] =
        RNS_SX1262_IRQ_CAD_DONE | RNS_SX1262_IRQ_RX_DONE;
    fail += ck(rns_sx1262_radio_poll(stale_radio, 1U) == RNS_OK &&
                   stale.reads == 0U && stale.cad == 1U &&
                   rns_sx1262_radio_receive_cad_result(stale_radio,
                                                       &cad_result) == RNS_OK &&
                   cad_result.outcome == RNS_SX1262_CAD_CLEAR,
               "combined CAD and stale RX flags terminate CAD only once");
    rns_sx1262_radio_destroy(stale_radio);
  }
  {
    fake_chip_t backpressure = {0};
    rns_sx1262_radio_t *backpressure_radio = NULL;
    uint32_t ids[RNS_SX1262_TX_RESULT_CAPACITY] = {0};
    uint32_t extra_id = 0;
    rns_sx1262_default_config(&c);
    fail += ck(rns_sx1262_radio_create(&OPS, &backpressure,
                                       &backpressure_radio) == RNS_OK &&
                   rns_sx1262_radio_start(backpressure_radio, &c) == RNS_OK,
               "result backpressure radio starts");
    for (size_t i = 0; i < RNS_SX1262_TX_RESULT_CAPACITY; ++i) {
      fail += ck(rns_sx1262_radio_send_with_id(backpressure_radio, d,
                                               sizeof(d), &ids[i]) == RNS_OK &&
                     rns_sx1262_radio_poll(backpressure_radio, 1U) == RNS_OK,
                 "backpressure frame starts");
      backpressure.irqs[backpressure.count++] = RNS_SX1262_IRQ_TX_DONE;
      fail += ck(rns_sx1262_radio_poll(backpressure_radio, 1U) == RNS_OK,
                 "backpressure frame completes");
    }
    fail += ck(rns_sx1262_radio_send_with_id(backpressure_radio, d, sizeof(d),
                                             &extra_id) == RNS_ERROR_OVERFLOW,
               "unconsumed terminal results apply bounded backpressure");
    rns_sx1262_radio_get_stats(backpressure_radio, &s);
    fail += ck(s.pending_tx == 0U &&
                   s.pending_tx_results == RNS_SX1262_TX_RESULT_CAPACITY &&
                   s.tx_result_backpressure == 1U,
               "result backpressure is observable");
    for (size_t i = 0; i < RNS_SX1262_TX_RESULT_CAPACITY; ++i)
      fail += ck(rns_sx1262_radio_receive_tx_result(backpressure_radio,
                                                    &result) == RNS_OK &&
                     result.id == ids[i] &&
                     result.outcome == RNS_SX1262_TX_SENT,
                 "terminal results preserve FIFO order");
    fail += ck(rns_sx1262_radio_receive_tx_result(backpressure_radio,
                                                  &result) ==
                       RNS_ERROR_NOT_FOUND &&
                   rns_sx1262_radio_send_with_id(backpressure_radio, d,
                                                 sizeof(d), &extra_id) ==
                       RNS_OK,
               "draining results releases producer backpressure");
    fail += ck(rns_sx1262_radio_stop(backpressure_radio) == RNS_OK &&
                   rns_sx1262_radio_receive_tx_result(backpressure_radio,
                                                      &result) == RNS_OK &&
                   result.id == extra_id &&
                   result.outcome == RNS_SX1262_TX_STOPPED &&
                   result.status == RNS_ERROR_INVALID_STATE,
               "stop reports queued frame exactly once");
    rns_sx1262_radio_destroy(backpressure_radio);
  }
  {
    fake_chip_t stop_failure = {.fail_standby = true};
    rns_sx1262_radio_t *stop_radio = NULL;
    uint32_t stop_ids[2] = {0};
    rns_sx1262_default_config(&c);
    fail += ck(rns_sx1262_radio_create(&OPS, &stop_failure, &stop_radio) ==
                       RNS_OK &&
                   rns_sx1262_radio_start(stop_radio, &c) == RNS_OK &&
                   rns_sx1262_radio_send_with_id(stop_radio, d, sizeof(d),
                                                 &stop_ids[0]) == RNS_OK &&
                   rns_sx1262_radio_send_with_id(stop_radio, d, sizeof(d),
                                                 &stop_ids[1]) == RNS_OK &&
                   rns_sx1262_radio_poll(stop_radio, 1U) == RNS_OK &&
                   rns_sx1262_radio_stop(stop_radio) == RNS_ERROR_IO,
               "failed standby terminates active and queued TX");
    for (size_t i = 0; i < 2U; ++i)
      fail += ck(rns_sx1262_radio_receive_tx_result(stop_radio, &result) ==
                         RNS_OK &&
                     result.id == stop_ids[i] &&
                     result.outcome == RNS_SX1262_TX_RADIO_FAILED &&
                     result.status == RNS_ERROR_IO,
                 "failed stop preserves FIFO terminal TX failures");
    fail += ck(rns_sx1262_radio_receive_tx_result(stop_radio, &result) ==
                       RNS_ERROR_NOT_FOUND,
               "failed stop emits no duplicate TX outcomes");
    stop_failure.fail_standby = false;
    fail += ck(rns_sx1262_radio_destroy(stop_radio) == RNS_OK,
               "radio can be destroyed after failed-stop outcomes drain");
  }
  {
    fake_chip_t stop_cad = {0};
    rns_sx1262_radio_t *stop_radio = NULL;
    rns_sx1262_cad_result_t cad_result;
    rns_sx1262_default_config(&c);
    fail += ck(rns_sx1262_radio_create(&OPS, &stop_cad, &stop_radio) ==
                       RNS_OK &&
                   rns_sx1262_radio_start(stop_radio, &c) == RNS_OK &&
                   rns_sx1262_radio_start_cad(stop_radio) == RNS_OK &&
                   rns_sx1262_radio_poll(stop_radio, 1U) == RNS_OK,
               "failed-stop CAD starts");
    stop_cad.fail_standby = true;
    fail += ck(rns_sx1262_radio_stop(stop_radio) == RNS_ERROR_IO &&
                   rns_sx1262_radio_receive_cad_result(stop_radio,
                                                       &cad_result) == RNS_OK &&
                   cad_result.outcome == RNS_SX1262_CAD_FAILED &&
                   cad_result.status == RNS_ERROR_IO &&
                   rns_sx1262_radio_receive_cad_result(stop_radio,
                                                       &cad_result) ==
                       RNS_ERROR_NOT_FOUND,
               "failed standby terminalizes CAD exactly once");
    stop_cad.fail_standby = false;
    fail += ck(rns_sx1262_radio_destroy(stop_radio) == RNS_OK,
               "failed-stop CAD radio destroys after result drain");
  }
  {
    fake_bus_t b = {.busy_until = 2};
    rns_sx1262_bus_t bus = {{&b, busy, now, delay, transfer, set_reset}, 100};
    const uint8_t cmd[] = {0x8a, 1}, w[] = {0x55};
    const uint8_t read_cmd[] = {0x12, 0x00};
    const uint8_t status_cmd = 0xc0;
    uint8_t read_data[2] = {0};
    uint8_t status_data = 0;
    fail += ck(rns_sx1262_bus_transfer(&bus, cmd, 2, w, NULL, 1) == RNS_OK &&
                   b.transfers == 1 && b.tx[2] == 0x55 && b.busy_reads >= 4,
               "BUSY before/after");
    fail += ck(rns_sx1262_bus_transfer(&bus, read_cmd, sizeof(read_cmd), NULL,
                                       read_data, sizeof(read_data)) == RNS_OK &&
                   read_data[0] == 0x82U && read_data[1] == 0x83U,
               "read data follows command and caller NOP");
    fail += ck(rns_sx1262_bus_transfer(&bus, &status_cmd, 1U, NULL,
                                       &status_data, 1U) == RNS_OK &&
                   status_data == 0x81U,
               "GET_STATUS byte follows opcode");
    fail += ck(rns_sx1262_bus_reset(&bus) == RNS_OK && b.edges == 1 &&
                   b.now >= 5200,
               "reset timing");
  }
  {
    fake_bus_t b = {.stuck_after_transfer = true};
    rns_sx1262_bus_t bus = {{&b, busy, now, delay, transfer, set_reset}, 30};
    const uint8_t cmd = 0xc0;
    uint8_t status = 0;
    fail += ck(rns_sx1262_bus_transfer(&bus, &cmd, 1, NULL, &status, 1) ==
                       RNS_ERROR_TIMEOUT &&
                   b.transfers == 1,
               "BUSY timeout after SPI transaction");
  }
  {
    fake_bus_t b = {.fail_transfer = true};
    rns_sx1262_bus_t bus = {{&b, busy, now, delay, transfer, set_reset}, 30};
    const uint8_t cmd = 0x80;
    fail += ck(rns_sx1262_bus_transfer(&bus, &cmd, 1U, NULL, NULL, 0U) ==
                       RNS_ERROR_IO &&
                   b.busy_reads >= 2U,
               "SPI failure still checks trailing BUSY");
    b = (fake_bus_t){.fail_reset = true};
    fail += ck(rns_sx1262_bus_reset(&bus) == RNS_ERROR_IO,
               "reset GPIO failure propagated");
  }
  {
    fake_bus_t b = {.busy_until = 100};
    rns_sx1262_bus_t bus = {{&b, busy, now, delay, transfer, set_reset}, 30};
    const uint8_t cmd = 0x80;
    fail += ck(rns_sx1262_bus_transfer(&bus, &cmd, 1, NULL, NULL, 0) ==
                       RNS_ERROR_TIMEOUT &&
                   b.transfers == 0,
               "BUSY timeout");
  }
  return fail ? 1 : 0;
}
