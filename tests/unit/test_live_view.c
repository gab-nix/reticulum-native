/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "live_view.h"
#include <assert.h>
#include <string.h>
int main(void) {
    heltec_live_view v = {0};
    heltec_radio_discovery s = {0};
    char lines[8][22];
    heltec_live_messages(&v, true, lines);
    assert(strstr(lines[2], "NO VERIFIED"));
    heltec_live_message(&v, (const uint8_t *)"hello", 5);
    heltec_live_message(&v, (const uint8_t *)"world", 5);
    heltec_live_messages(&v, false, lines);
    assert(!strcmp(lines[2], "world"));
    heltec_live_messages(&v, true, lines);
    assert(!strcmp(lines[2], "hello"));
    heltec_live_message(&v, (const uint8_t *)"new", 3);
    heltec_live_messages(&v, false, lines);
    assert(!strcmp(lines[2], "hello"));
    for (size_t i = 0; i < 20; ++i) heltec_live_message(&v, NULL, 0);
    assert(v.count == 8 && v.selected < 8);
    uint8_t long_text[200]; memset(long_text, 'a', sizeof(long_text));
    heltec_live_message(&v, long_text, sizeof(long_text));
    assert(strlen(v.messages[0]) == 95);
    heltec_live_nodes(&v, &s, 0, true, lines);
    assert(!v.node_selected);
    s.peers[0].used = s.peers[1].used = s.peers[2].used = true;
    s.peers[2].identity_hash[15] = 1;
    heltec_live_nodes(&v, &s, 1000, false, lines);
    assert(!strcmp(lines[0], "NODES 2"));
    heltec_live_nodes(&v, &s, 1000, true, lines);
    assert(v.node_key[15] == 1);
    s.peers[0].used = false;
    heltec_live_nodes(&v, &s, 2000, false, lines);
    assert(v.node_key[15] == 1);
    s.peers[2].used = false;
    heltec_live_nodes(&v, &s, 2000, false, lines);
    assert(v.node_key[15] == 0);
    return 0;
}
