/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "channel_view.h"
#include <assert.h>
#include <string.h>
int main(void) {
    heltec_channel_view v = {0};
    rns_interface_stats_t s = {.radio_telemetry_valid = 1};
    char lines[8][22];
    heltec_channel_sample(&v, 0, &s); assert(v.samples[59] == 8);
    s.radio_rx_frames = 1;
    heltec_channel_sample(&v, 1000, &s); assert(v.samples[59] == 1);
    s.radio_tx_frames = 1; s.radio_cad_busy = 1;
    heltec_channel_sample(&v, 2000, &s); assert(v.samples[59] == 6);
    heltec_channel_lines(&v, &s, lines);
    assert(lines[1][20] == '*' && lines[2][20] == '*' && lines[3][20] == '*');
    heltec_channel_sample(&v, 5000, &s); assert(v.samples[59] == 8);
    heltec_channel_sample(&v, 100000, &s);
    for (unsigned i = 0; i < 60; ++i) assert(v.samples[i] == 8);
    heltec_channel_sample(&v, 101000, &s); assert(v.samples[59] == 0);
    s.radio_rx_frames = 0;
    heltec_channel_sample(&v, 102000, &s); assert(v.samples[59] == 8);
    s.radio_telemetry_valid = 0;
    heltec_channel_sample(&v, 103000, &s);
    s.radio_telemetry_valid = 1;
    heltec_channel_sample(&v, 104000, &s); assert(v.samples[59] == 8);
    heltec_channel_sample(&v, 100, &s); assert(v.samples[59] == 8);
    heltec_channel_lines(&v, &s, lines);
    for (unsigned i = 0; i < 8; ++i) assert(strlen(lines[i]) <= 21);
    return 0;
}
