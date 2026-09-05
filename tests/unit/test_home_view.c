/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "home_view.h"
#include "activity_led.h"
#include <assert.h>
#include <string.h>
int main(void) {
    heltec_activity_led led = {0};
    rns_interface_stats_t stats = {.radio_telemetry_valid = 1};
    assert(!heltec_activity_led_sample(&led, 0, &stats));
    stats.pending_tx = 1; /* Enqueue is not RF activity. */
    assert(!heltec_activity_led_sample(&led, 1, &stats));
    stats.radio_rx_frames++;
    assert(heltec_activity_led_sample(&led, 10, &stats));
    assert(heltec_activity_led_sample(&led, 129, &stats));
    assert(!heltec_activity_led_sample(&led, 130, &stats));
    stats.radio_tx_frames++;
    assert(heltec_activity_led_sample(&led, 200, &stats));
    stats.radio_rx_frames++;
    assert(heltec_activity_led_sample(&led, 210, &stats));
    assert(heltec_activity_led_sample(&led, 439, &stats));
    assert(!heltec_activity_led_sample(&led, 440, &stats));
    assert(!heltec_activity_led_sample(&led, 0, &stats));
    stats.radio_rx_frames = 0;
    assert(!heltec_activity_led_sample(&led, 1, &stats));
    stats.radio_telemetry_valid = 0;
    assert(!heltec_activity_led_sample(&led, 2, &stats));
    heltec_home_snapshot s = {0}; char lines[8][22];
    heltec_home_lines(&s, lines);
    assert(strstr(lines[3], "UNAVAIL"));
    assert(!strcmp(lines[6], "CPU UNAVAILABLE"));
    s.radio_valid = true; s.radio.bytes_sent = 42;
    s.rx_packets = UINT64_MAX; s.heap_free = 32768; s.heap_minimum = 16384;
    heltec_home_lines(&s, lines);
    assert(strstr(lines[1], "999999999+"));
    assert(!strcmp(lines[4], "TX BYTES 42"));
    assert(!strcmp(lines[5], "RAM 32K MIN 16K"));
    for (size_t i = 0; i < 8; ++i) assert(strlen(lines[i]) <= 21);
    return 0;
}
