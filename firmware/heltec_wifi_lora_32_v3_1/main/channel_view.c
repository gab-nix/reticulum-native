/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "channel_view.h"
#include <stdio.h>
#include <string.h>
void heltec_channel_sample(heltec_channel_view *v, uint64_t now,
                          const rns_interface_stats_t *s) {
    uint64_t second = now/1000U;
    if (v->initialized && second == v->second) return;
    if (!v->initialized || second < v->second) {
        memset(v->samples, 8, sizeof(v->samples)); v->initialized = true;
    } else {
        uint64_t elapsed = second-v->second;
        size_t n = elapsed > 60U ? 60U : (size_t)elapsed;
        memmove(v->samples, v->samples+n, 60U-n);
        memset(v->samples+60U-n, 8, n);
        if (elapsed == 1U && v->previous_valid && s->radio_telemetry_valid &&
            s->radio_rx_frames >= v->rx && s->radio_tx_frames >= v->tx && s->radio_cad_busy >= v->busy)
            v->samples[59] = (uint8_t)((s->radio_rx_frames != v->rx ? 1U : 0U) |
                (s->radio_tx_frames != v->tx ? 2U : 0U) | (s->radio_cad_busy != v->busy ? 4U : 0U));
    }
    v->second = second; v->rx = s->radio_rx_frames;
    v->tx = s->radio_tx_frames; v->busy = s->radio_cad_busy;
    v->previous_valid = s->radio_telemetry_valid != 0;
}
void heltec_channel_lines(const heltec_channel_view *v,
                         const rns_interface_stats_t *s, char lines[8][22]) {
    memset(lines, 0, 8U*22U);
    memcpy(lines[0], "ACTIVITY HISTORY 60S", 21U);
    /* Three time-aligned rows, each cell summarises three one-second samples. */
    for (unsigned row = 0; row < 3; ++row) {
        const char labels[] = "RTB";
        lines[row+1U][0] = labels[row];
        for (unsigned col = 0; col < 20; ++col) {
            unsigned bits = 0;
            for (unsigned i = 0; i < 3; ++i) bits |= v->samples[col*3U+i];
            lines[row+1U][col+1U] = bits & (1U<<row) ? '*' : bits & 8U ? '?' : '.';
        }
    }
    if (s->radio_telemetry_valid) {
        /* Firmware profile is fixed to 1% of a rolling hour: 36000 ms. */
        (void)snprintf(lines[4], 22, "AIR %lu/36000MS", (unsigned long)(s->radio_airtime_us/1000U > 999999U ? 999999U : s->radio_airtime_us/1000U));
        (void)snprintf(lines[5], 22, "QUEUE %u DEF %u", (unsigned)(s->pending_tx > 999U ? 999U : s->pending_tx),
            (unsigned)(s->radio_duty_deferrals > 999U ? 999U : s->radio_duty_deferrals));
    } else memcpy(lines[4], "RADIO UNAVAILABLE", 18U);
    if (s->radio_telemetry_valid && s->radio_signal_valid)
        (void)snprintf(lines[6], 22, "R%d S%d LAST", (int)s->radio_last_rssi_dbm, (int)s->radio_last_snr_db);
    else memcpy(lines[6], "LAST SIGNAL UNKNOWN", 20U);
    memcpy(lines[7], "HOLD MENU", 10U);
}
