/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "chat_journal.h"
#include "reticulum/storage_record.h"
#include "reticulum/hal.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdbool.h>
#define SECTOR 4096U
#define PAYLOAD (SECTOR-RNS_STORAGE_RECORD_HEADER_SIZE)
typedef struct {
    heltec_chat_flash_ops ops;
    uint8_t sectors[2][SECTOR], payload[PAYLOAD];
    bool quarantined[HELTEC_CHAT_JOURNAL_RECORDS];
} journal;
static bool key_slot(const char *key, size_t *slot) {
    if (!key) return false;
    if (!strncmp(key,"chat",4) && key[4] >= '0' && key[4] <= '7' && !key[5]) {
        *slot=(size_t)(key[4]-'0'); return true;
    }
    if (!strncmp(key,"msg",3) && key[3] >= '0' && key[3] <= '6' &&
        key[4] >= '0' && key[4] <= '9' && !key[5]) {
        size_t index=(size_t)(key[3]-'0')*10U+(size_t)(key[4]-'0');
        if(index<64U) { *slot=8U+index; return true; }
    }
    if (!strncmp(key,"out",3) && key[3]>='0' && key[3]<='3' && !key[4]) {
        *slot=72U+(size_t)(key[3]-'0'); return true;
    }
    if (!strcmp(key,"prefs")) { *slot=76U; return true; }
    if(!strncmp(key,"peer",4) && key[4]>='0' && key[4]<='3' &&
       key[5]>='0' && key[5]<='9' && !key[6]) {
        size_t index=(size_t)(key[4]-'0')*10U+(size_t)(key[5]-'0');
        if(index<32U) { *slot=77U+index; return true; }
    }
    return false;
}
static rns_status_t inspect(journal *j,const char *key,size_t slot,int valid[2],uint32_t gen[2]) {
    bool blank[2] = {true,true};
    for (size_t i=0;i<2;++i) {
        rns_status_t st=j->ops.read(j->ops.context,(slot*2U+i)*SECTOR,j->sectors[i],SECTOR);
        if(st!=RNS_OK) return st;
        for(size_t b=0;b<SECTOR;++b) if(j->sectors[i][b]!=0xffU) { blank[i]=false; break; }
        size_t n=0;
        valid[i]=rns_storage_record_decode(key,j->sectors[i],SECTOR,j->payload,PAYLOAD,&n,&gen[i])==RNS_OK && n==PAYLOAD;
        if(valid[i]) {
            size_t len=(size_t)j->payload[1]*256U+j->payload[2];
            if(j->payload[0]>1 || len>PAYLOAD-3U || (!j->payload[0] && len)) valid[i]=0;
        }
    }
    if(!valid[0] && !valid[1] && (!blank[0] || !blank[1])) return RNS_ERROR_PROTOCOL;
    return RNS_OK;
}
static rns_status_t read_record(void *ctx,const char *key,uint8_t *out,size_t cap,size_t *length) {
    journal *j=ctx; size_t slot; int valid[2]={0}; uint32_t gen[2]={0}; char selected;
    if(!key_slot(key,&slot)) return RNS_ERROR_INVALID_ARGUMENT;
    if(j->quarantined[slot]) return RNS_ERROR_QUARANTINED;
    rns_status_t st=inspect(j,key,slot,valid,gen); if(st!=RNS_OK) return st;
    st=rns_storage_record_select_slot(valid[0],gen[0],valid[1],gen[1],&selected); if(st!=RNS_OK) return st;
    size_t n=0; uint32_t generation=0;
    st=rns_storage_record_decode(key,j->sectors[selected=='a'?0:1],SECTOR,j->payload,PAYLOAD,&n,&generation);
    if(st!=RNS_OK) return st;
    if(!j->payload[0]) return RNS_ERROR_NOT_FOUND;
    *length=(size_t)j->payload[1]*256U+j->payload[2];
    if(cap<*length) return RNS_ERROR_OVERFLOW;
    if(*length) memcpy(out,j->payload+3,*length);
    return RNS_OK;
}
static rns_status_t replace(journal *j,const char *key,const uint8_t *data,size_t len,bool present) {
    size_t slot; int valid[2]={0}; uint32_t gen[2]={0},next; char selected;
    if(!key_slot(key,&slot)) return RNS_ERROR_INVALID_ARGUMENT;
    if(j->quarantined[slot]) return RNS_ERROR_QUARANTINED;
    if(len>PAYLOAD-3U) return RNS_ERROR_OVERFLOW;
    rns_status_t st=inspect(j,key,slot,valid,gen); if(st!=RNS_OK) return st;
    st=rns_storage_record_next_slot(valid[0],gen[0],valid[1],gen[1],&selected,&next); if(st!=RNS_OK) return st;
    size_t target=selected=='a'?0U:1U, offset=(slot*2U+target)*SECTOR, encoded=0;
    memset(j->payload,0,PAYLOAD); j->payload[0]=present?1U:0U;
    j->payload[1]=(uint8_t)(len>>8); j->payload[2]=(uint8_t)len;
    if(len) memcpy(j->payload+3,data,len);
    st=rns_storage_record_encode(key,next,j->payload,PAYLOAD,j->sectors[target],SECTOR,&encoded);
    if(st!=RNS_OK) return st;
    st=j->ops.erase(j->ops.context,offset,SECTOR); if(st!=RNS_OK) return st;
    st=j->ops.write(j->ops.context,offset,j->sectors[target],SECTOR); if(st!=RNS_OK) return st;
    st=j->ops.read(j->ops.context,offset,j->sectors[1U-target],SECTOR); if(st!=RNS_OK) return st;
    return memcmp(j->sectors[target],j->sectors[1U-target],SECTOR)?RNS_ERROR_IO:RNS_OK;
}
static rns_status_t write_record(void *ctx,const char *key,const uint8_t *data,size_t len) {
    return replace(ctx,key,data,len,true);
}
static rns_status_t remove_record(void *ctx,const char *key) { return replace(ctx,key,NULL,0,false); }
static void destroy(void *ctx) { journal *j=ctx; rns_hal_secure_zero(j,sizeof(*j)); free(j); }
rns_status_t heltec_chat_journal_open_report(const heltec_chat_flash_ops *ops,rns_storage_t **out,size_t *quarantined) {
    if(!ops || !ops->read || !ops->write || !ops->erase || !out) return RNS_ERROR_INVALID_ARGUMENT;
    if(quarantined) *quarantined=0;
    *out=NULL; journal *j=calloc(1,sizeof(*j)); if(!j) return RNS_ERROR_NO_MEMORY; j->ops=*ops;
    size_t damaged=0, healthy=0;
    for(unsigned i=0;i<HELTEC_CHAT_JOURNAL_RECORDS;++i) {
        char key[8]; int valid[2]={0}; uint32_t gen[2]={0};
        if(i<8U) (void)snprintf(key,sizeof(key),"chat%u",i);
        else if(i<72U) (void)snprintf(key,sizeof(key),"msg%02u",i-8U);
        else if(i<76U) (void)snprintf(key,sizeof(key),"out%u",i-72U);
        else if(i==76U) memcpy(key,"prefs",6);
        else (void)snprintf(key,sizeof(key),"peer%02u",i-77U);
        rns_status_t st=inspect(j,key,i,valid,gen);
        if(st==RNS_ERROR_PROTOCOL) { j->quarantined[i]=true; ++damaged; }
        else if(st!=RNS_OK) { destroy(j); return st; }
        else if(valid[0] || valid[1]) ++healthy;
    }
    if(damaged && !healthy) { destroy(j); return RNS_ERROR_PROTOCOL; }
    static const rns_storage_ops_t storage_ops={.read=read_record,.write_atomic=write_record,.remove=remove_record,.destroy=destroy};
    rns_status_t st=rns_storage_create(&storage_ops,j,out);
    if(st!=RNS_OK) destroy(j);
    else if(quarantined) *quarantined=damaged;
    return st;
}
rns_status_t heltec_chat_journal_open(const heltec_chat_flash_ops *ops,rns_storage_t **out) {
    return heltec_chat_journal_open_report(ops,out,NULL);
}
