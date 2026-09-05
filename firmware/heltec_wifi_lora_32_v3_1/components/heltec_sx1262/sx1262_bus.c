/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "sx1262_bus.h"
#include "reticulum/heltec_sx1262.h"
#include <string.h>
enum {
  BUS_MAX = RNS_SX1262_MAX_PAYLOAD + 4,
  BUSY_POLL_US = 10,
  RESET_PULSE_US = 200,
  RESET_SETTLE_US = 5000
};
static bool valid(const rns_sx1262_bus_t *b) {
  return b && b->busy_timeout_us && b->ops.busy && b->ops.now_us &&
         b->ops.delay_us && b->ops.spi_transfer && b->ops.set_reset;
}
static rns_status_t ready(rns_sx1262_bus_t *b) {
  uint64_t start = b->ops.now_us(b->ops.context);
  while (b->ops.busy(b->ops.context)) {
    uint64_t now = b->ops.now_us(b->ops.context);
    if (now - start >= b->busy_timeout_us)
      return RNS_ERROR_TIMEOUT;
    b->ops.delay_us(b->ops.context, BUSY_POLL_US);
  }
  return RNS_OK;
}
rns_status_t rns_sx1262_bus_transfer(rns_sx1262_bus_t *b, const uint8_t *cmd,
                                     size_t clen, const uint8_t *w, uint8_t *r,
                                     size_t dlen) {
  uint8_t tx[BUS_MAX] = {0}, rx[BUS_MAX] = {0};
  size_t total = clen + dlen;
  rns_status_t s;
  if (!valid(b) || !cmd || !clen || total < clen || total > sizeof(tx) ||
      (dlen && !w && !r))
    return RNS_ERROR_INVALID_ARGUMENT;
  s = ready(b);
  if (s != RNS_OK)
    return s;
  memcpy(tx, cmd, clen);
  if (w && dlen)
    memcpy(tx + clen, w, dlen);
  s = b->ops.spi_transfer(b->ops.context, tx, rx, total);
  {
    const rns_status_t post = ready(b);
    if (s != RNS_OK)
      return s;
    if (post != RNS_OK)
      return post;
  }
  if (r && dlen)
    memcpy(r, rx + clen, dlen);
  return RNS_OK;
}
rns_status_t rns_sx1262_bus_reset(rns_sx1262_bus_t *b) {
  rns_status_t s;
  if (!valid(b))
    return RNS_ERROR_INVALID_ARGUMENT;
  s = b->ops.set_reset(b->ops.context, false);
  if (s != RNS_OK)
    return s;
  b->ops.delay_us(b->ops.context, RESET_PULSE_US);
  s = b->ops.set_reset(b->ops.context, true);
  if (s != RNS_OK)
    return s;
  b->ops.delay_us(b->ops.context, RESET_SETTLE_US);
  return ready(b);
}
