/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "live_view.h"
#include <stdio.h>
#include <string.h>
void heltec_live_message(heltec_live_view *v, const uint8_t *text, size_t length) {
    if (!v || (!text && length)) return;
    /* Keep selection on the same retained message as new arrivals prepend. */
    if (v->count && v->selected) {
        if (v->selected < 7U) ++v->selected;
    }
    memmove(v->messages[1], v->messages[0], 7U * sizeof(v->messages[0]));
    memset(v->messages[0], 0, sizeof(v->messages[0]));
    size_t out = 0;
    for (size_t i = 0; i < length && out < 95U; ++i) {
        uint8_t c = text[i];
        if ((c & 0xc0U) == 0x80U) continue;
        v->messages[0][out++] = c >= 32U && c < 127U ? (char)c : c < 32U ? ' ' : '?';
    }
    if (!out) memcpy(v->messages[0], "EMPTY MESSAGE", 14U);
    if (v->count < 8U) ++v->count;
}
void heltec_live_messages(heltec_live_view *v, bool next, char lines[8][22]) {
    memset(lines, 0, 8U*22U);
    if (next && v->count) v->selected = (v->selected+1U)%v->count;
    (void)snprintf(lines[0], 22, "MESSAGES %u OF %u", (unsigned)(v->count ? v->selected+1U : 0U), (unsigned)v->count);
    if (!v->count) memcpy(lines[2], "NO VERIFIED MESSAGES", 21U);
    else {
        const char *text = v->messages[v->selected];
        size_t n = strlen(text);
        for (size_t i = 0; i < n; ++i) lines[2U+i/21U][i%21U] = text[i];
    }
    memcpy(lines[7], "TAP NEXT  HOLD MENU", 20U);
}
void heltec_live_nodes(heltec_live_view *v, const heltec_radio_discovery *s,
                      uint64_t now, bool next, char lines[8][22]) {
    size_t indices[HELTEC_DISCOVERY_PEERS], count = 0, selected = 0;
    memset(lines, 0, 8U*22U);
    /* One row per identity, even when it advertises several services. */
    for (size_t i = 0; i < HELTEC_DISCOVERY_PEERS; ++i) {
        if (!s->peers[i].used) continue;
        bool duplicate = false;
        for (size_t j = 0; j < count; ++j)
            if (!memcmp(s->peers[indices[j]].identity_hash, s->peers[i].identity_hash, 16)) duplicate = true;
        if (!duplicate) indices[count++] = i;
    }
    for (size_t i = 0; i < count; ++i)
        if (v->node_selected && !memcmp(v->node_key, s->peers[indices[i]].identity_hash, 16)) selected = i;
    if (next && count) selected = (selected+1U)%count;
    (void)snprintf(lines[0], 22, "NODES %u", (unsigned)count);
    if (!count) { memcpy(lines[2], "WAITING FOR ANNOUNCE", 21U); v->node_selected = false; }
    else {
        memcpy(v->node_key, s->peers[indices[selected]].identity_hash, 16); v->node_selected = true;
        size_t start = selected/5U*5U;
        for (size_t i = start; i < count && i < start+5U; ++i) {
            const heltec_discovered_peer *p = &s->peers[indices[i]];
            uint64_t age = now >= p->last_seen_ms ? (now-p->last_seen_ms)/1000U : 0U;
            (void)snprintf(lines[1U+i-start], 22, "%c %02X%02X%02X%02X %us", i == selected ? '*' : ' ',
                p->identity_hash[12], p->identity_hash[13], p->identity_hash[14], p->identity_hash[15],
                (unsigned)(age > 99999U ? 99999U : age));
        }
    }
    memcpy(lines[7], "TAP NEXT  HOLD MENU", 20U);
}
