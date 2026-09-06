/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef HELTEC_PEER_LABEL_H
#define HELTEC_PEER_LABEL_H
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
typedef bool (*heltec_peer_name_fn)(void *context,const uint8_t address[16],char name[128]);
/* Names are presentation only; callers keep address-based selection. */
static inline void heltec_peer_label(heltec_peer_name_fn lookup,void *context,
                                    const uint8_t address[16],char label[22]) {
    char name[128]={0}; size_t n=0;
    if(lookup && lookup(context,address,name)) {
        for(size_t i=0;i<sizeof(name) && name[i];++i) {
            uint8_t c=(uint8_t)name[i];
            if((c&0xc0U)==0x80U) continue;
            if(n==21U) { label[20]='~'; break; }
            label[n++]=c>=32U && c<127U?(char)c:c<32U?' ':'?';
        }
        while(n && label[n-1U]==' ') --n;
    }
    label[n]=0;
    if(!n) (void)snprintf(label,22,"%02X%02X%02X%02X",address[12],address[13],address[14],address[15]);
}
#endif
