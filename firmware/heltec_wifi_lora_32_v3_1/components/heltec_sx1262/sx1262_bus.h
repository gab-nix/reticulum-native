/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef RETICULUM_SX1262_BUS_H
#define RETICULUM_SX1262_BUS_H
#include "reticulum/status.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
typedef struct {
  void *context;
  bool (*busy)(void *);
  uint64_t (*now_us)(void *);
  void (*delay_us)(void *, uint32_t);
  rns_status_t (*spi_transfer)(void *, const uint8_t *, uint8_t *, size_t);
  rns_status_t (*set_reset)(void *, bool);
} rns_sx1262_bus_ops_t;
typedef struct {
  rns_sx1262_bus_ops_t ops;
  uint32_t busy_timeout_us;
} rns_sx1262_bus_t;
rns_status_t rns_sx1262_bus_transfer(rns_sx1262_bus_t *, const uint8_t *,
                                     size_t, const uint8_t *, uint8_t *,
                                     size_t);
rns_status_t rns_sx1262_bus_reset(rns_sx1262_bus_t *);
#endif
