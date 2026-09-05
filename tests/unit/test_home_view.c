/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "home_view.h"
#include <assert.h>
#include <string.h>
int main(void) {
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
