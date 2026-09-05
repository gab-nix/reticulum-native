#include "reticulum/heltec_sx1262.h"
#include "sx1262_bus.h"
#include <stdio.h>
#include <string.h>
typedef struct {
  unsigned resets, configs, rx, tx, reads, standby;
  uint16_t irqs[40];
  size_t count, index;
  rns_sx1262_packet_t packet;
  bool fail_tx;
  bool fail_irq;
  bool fail_standby;
} fake_chip_t;
static rns_status_t reset(void *c) {
  ((fake_chip_t *)c)->resets++;
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
static rns_status_t tx(void *c, const rns_sx1262_config_t *v, const uint8_t *d,
                       size_t n, uint32_t t) {
  fake_chip_t *f = c;
  f->tx++;
  if (f->fail_tx) {
    f->fail_tx = false;
    return RNS_ERROR_IO;
  }
  return v && v->crc_enabled && d && n && t == 5000U ? RNS_OK
                                                      : RNS_ERROR_PROTOCOL;
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
static const rns_sx1262_chip_ops_t OPS = {reset, config,      rx,     tx,
                                          irq,   read_packet, standby};
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
  const uint8_t d[] = {1, 2, 3};
  int fail = 0;
  rns_sx1262_default_config(&c);
  fail += ck(c.frequency_hz == 868200000U && c.bandwidth_hz == 125000U &&
                 c.spreading_factor == 8U && c.coding_rate_denominator == 5U &&
                 c.preamble_symbols == 18U && c.crc_enabled && !c.invert_iq &&
                 c.tx_power_dbm == 14,
             "EU868 defaults");
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
    invalid.tx_timeout_ms = 262144U;
    fail += ck(rns_sx1262_radio_create(&OPS, &f, &invalid_radio) == RNS_OK &&
                   rns_sx1262_radio_start(invalid_radio, &invalid) ==
                       RNS_ERROR_INVALID_ARGUMENT,
               "timeout beyond SX1262 RTC range rejected");
    rns_sx1262_radio_destroy(invalid_radio);
  }
  fail += ck(rns_sx1262_radio_send(r, d, sizeof(d)) == RNS_OK &&
                 rns_sx1262_radio_poll(r, 4) == RNS_OK && f.tx == 1,
             "queued TX starts without IRQ notification");
  f.irqs[f.count++] = RNS_SX1262_IRQ_TX_DONE;
  fail += ck(rns_sx1262_radio_poll(r, 4) == RNS_OK, "TX done");
  rns_sx1262_radio_get_stats(r, &s);
  fail += ck(s.tx_packets == 1 && s.tx_bytes == 3 && s.pending_tx == 0 &&
                 s.state == RNS_SX1262_RECEIVING,
             "TX stats");
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
  fail += ck(rns_sx1262_radio_send(r, d, sizeof(d)) == RNS_OK &&
                 rns_sx1262_radio_poll(r, 1) == RNS_OK && f.resets == 3,
             "TX failure recovery");
  fail += ck(rns_sx1262_radio_poll(r, 1) == RNS_OK && f.tx == 3,
             "TX retained retry");
  fail += ck(rns_sx1262_radio_stop(r) == RNS_OK && f.standby == 1, "stop");
  fail += ck(rns_sx1262_radio_receive(r, &p) == RNS_ERROR_INVALID_STATE,
             "stopped receive");
  rns_sx1262_radio_destroy(r);
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
    fake_chip_t stop_failure = {.fail_standby = true};
    rns_sx1262_radio_t *stop_radio = NULL;
    rns_sx1262_default_config(&c);
    fail += ck(rns_sx1262_radio_create(&OPS, &stop_failure, &stop_radio) ==
                       RNS_OK &&
                   rns_sx1262_radio_start(stop_radio, &c) == RNS_OK &&
                   rns_sx1262_radio_destroy(stop_radio) == RNS_ERROR_IO,
               "destroy reports failed standby");
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
