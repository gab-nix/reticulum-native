/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "live_view.h"
#include <assert.h>
#include <string.h>
static bool snapshot(void *context,size_t slot,uint8_t address[16],bool *observed) {
    (void)context; if(slot>=2) return false;
    memset(address,0,16); address[15]=(uint8_t)(slot+1U); *observed=slot==1; return true;
}
static bool name(void *context,const uint8_t address[16],char out[128]) {
    (void)context; (void)address; memcpy(out,"same\nname",10); return true;
}
static bool oversized_name(void *context,const uint8_t address[16],char out[128]) {
    (void)context; (void)address; memset(out,'a',128); return true;
}
static bool blank_name(void *context,const uint8_t address[16],char out[128]) {
    (void)context; (void)address; memcpy(out,"\r\n\t",4); return true;
}
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
    v.peer_snapshot=snapshot; v.peer_name=name;
    heltec_live_nodes(&v,&s,2000,false,lines);
    assert(!strcmp(lines[0],"LXMF NODES 2") && strstr(lines[1],"same name"));
    assert(strstr(lines[5],"KNOWN"));
    heltec_live_nodes(&v,&s,2000,true,lines);
    assert(v.node_key[15]==2 && strstr(lines[5],"OBSERVED"));
    assert(strstr(lines[6],"UNKNOWN"));
    uint8_t address[16]={0}; address[15]=3;
    char label[22]; heltec_peer_label(NULL,NULL,address,label);
    assert(!strcmp(label,"00000003"));
    heltec_peer_label(oversized_name,NULL,address,label);
    assert(strlen(label)==21 && label[20]=='~');
    heltec_peer_label(blank_name,NULL,address,label);
    assert(!strcmp(label,"00000003"));
    char sender[22],preview[22]; size_t previous=v.selected;
    heltec_live_notify(&v,address,(const uint8_t *)"a new message to truncate safely",31,2000);
    assert(v.selected==previous && v.node_key[15]==2);
    assert(heltec_live_notification(&v,2001,sender,preview));
    assert(!strcmp(sender,"same name") && preview[20]=='~' && !preview[21]);
    assert(!heltec_live_notification(&v,32000,sender,preview));
    return 0;
}
