/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "home_view.h"
#include <stdio.h>
#include <string.h>
/* Counts remain exact in the underlying statistics; display saturation is
 * explicit so a long-running node never appears to wrap back to zero. */
static void counter(char line[22], const char *label, uint64_t value) {
    if (value > 999999999U) (void)snprintf(line, 22, "%s 999999999+", label);
    else (void)snprintf(line, 22, "%s %lu", label, (unsigned long)value);
}
void heltec_home_lines(const heltec_home_snapshot *s, char lines[8][22]) {
    memset(lines, 0, 8U*22U);
    memcpy(lines[0], "HOME TRANSPORT OFF", 19U);
    counter(lines[1], "RX PACKETS", s->rx_packets);
    counter(lines[2], "TX PACKETS", s->tx_packets);
    if (s->radio_valid) {
        counter(lines[3], "RX BYTES", s->radio.bytes_received);
        counter(lines[4], "TX BYTES", s->radio.bytes_sent);
        counter(lines[7], "QUEUED", s->radio.pending_tx);
    } else {
        memcpy(lines[3], "RADIO STATS UNAVAIL", 20U);
    }
    (void)snprintf(lines[5], 22, "RAM %uK MIN %uK",
        (unsigned)(s->heap_free/1024U > 9999U ? 9999U : s->heap_free/1024U),
        (unsigned)(s->heap_minimum/1024U > 9999U ? 9999U : s->heap_minimum/1024U));
    if (s->cpu_valid && s->cpu_percent <= 100U)
        (void)snprintf(lines[6], 22, "CPU %u PCT 5S", s->cpu_percent);
    else memcpy(lines[6], "CPU UNAVAILABLE", 16U);
}
