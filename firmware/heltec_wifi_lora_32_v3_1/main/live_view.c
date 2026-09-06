/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "live_view.h"
#include <stdio.h>
#include <string.h>
void heltec_live_notify(heltec_live_view *v,const uint8_t sender[16],const uint8_t *text,size_t length,uint64_t now) {
    if(!v || !sender || (!text && length)) return;
    memcpy(v->notification_sender,sender,16); memset(v->notification_preview,0,22);
    size_t out=0;
    for(size_t i=0;i<length;++i) {
        uint8_t c=text[i]; if((c&0xc0U)==0x80U) continue;
        if(out==21U) { v->notification_preview[20]='~'; break; }
        v->notification_preview[out++]=c>=32U && c<127U?(char)c:c<32U?' ':'?';
    }
    v->notification_until=now>UINT64_MAX-30000U?UINT64_MAX:now+30000U;
}
bool heltec_live_notification(const heltec_live_view *v,uint64_t now,char sender[22],char preview[22]) {
    if(!v || !sender || !preview || !v->notification_until || now>=v->notification_until) return false;
    heltec_peer_label(v->peer_name,v->peer_context,v->notification_sender,sender);
    memcpy(preview,v->notification_preview,22); return true;
}
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
    if(v->peer_snapshot) {
        uint8_t addresses[32][16]; unsigned state[32]; size_t count=0,selected=0;
        memset(lines,0,8U*22U);
        for(size_t i=0;i<32;++i) if(v->peer_snapshot(v->peer_context,i,addresses[count],&state[count])) ++count;
        for(size_t i=0;i<count;++i) if(v->node_selected && !memcmp(v->node_key,addresses[i],16)) selected=i;
        if(next && count) selected=(selected+1U)%count;
        (void)snprintf(lines[0],22,"LXMF NODES %u",(unsigned)count);
        if(count) {
            memcpy(v->node_key,addresses[selected],16); v->node_selected=true;
            size_t first=selected/4U*4U;
            for(size_t i=first;i<count && i<first+4U;++i) {
                char label[22]; heltec_peer_label(v->peer_name,v->peer_context,addresses[i],label);
                (void)snprintf(lines[i-first+1U],22,"%c %.19s",i==selected?'*':' ',label);
            }
            const uint8_t *a=addresses[selected];
            static const char *states[]={"KNOWN","CONNECTING","LINKED","UNREACHABLE"};
            unsigned current=state[selected]<4?state[selected]:0;
            (void)snprintf(lines[5],22,"%02X%02X%02X%02X %s",a[12],a[13],a[14],a[15],states[current]);
            (void)snprintf(lines[6],22,"%s",current==2?"ENCRYPTED LINK":current==0?"REACHABILITY UNKNOWN":"RECONNECT ON SEND");
        } else { v->node_selected=false; (void)snprintf(lines[2],22,"NO REMEMBERED PEERS"); }
        memcpy(lines[7],"TAP NEXT  HOLD MENU",20U); return;
    }
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
