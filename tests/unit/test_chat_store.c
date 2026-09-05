/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "chat_store.h"
#include <assert.h>
#include <string.h>
static uint8_t records[8][4096]; static size_t lengths[8]; static bool fail;
static unsigned slot(const char *key) {
    assert(!strncmp(key,"chat",4) && key[4] >= '0' && key[4] <= '7' && !key[5]);
    return (unsigned)(key[4]-'0');
}
static rns_status_t read_record(void *c, const char *k, uint8_t *out, size_t cap, size_t *len) {
    (void)c; unsigned i=slot(k); if (!lengths[i]) return RNS_ERROR_NOT_FOUND;
    if (cap < lengths[i]) return RNS_ERROR_OVERFLOW;
    memcpy(out,records[i],lengths[i]); *len=lengths[i]; return RNS_OK;
}
static rns_status_t write_record(void *c,const char *k,const uint8_t *data,size_t len) {
    (void)c; if(fail) return RNS_ERROR_IO; unsigned i=slot(k); assert(len <= 4096);
    memcpy(records[i],data,len); lengths[i]=len; return RNS_OK;
}
static rns_status_t remove_record(void *c,const char *k) {
    (void)c; if(fail) return RNS_ERROR_IO; lengths[slot(k)]=0; return RNS_OK;
}
int main(void) {
    rns_storage_ops_t ops={.read=read_record,.write_atomic=write_record,.remove=remove_record};
    rns_storage_t *storage=NULL; assert(rns_storage_create(&ops,NULL,&storage)==RNS_OK);
    heltec_chat_store *s=NULL; assert(heltec_chat_store_open(storage,&s)==RNS_OK);
    uint8_t sender[16]={1}; heltec_chat_message m={.length=5,.timestamp=42};
    memcpy(m.text,"hello",5); m.id[0]=1;
    assert(heltec_chat_store_add(s,sender,&m)==RNS_OK);
    assert(heltec_chat_store_add(s,sender,&m)==RNS_OK);
    assert(heltec_chat_store_get(s,0)->count==1);
    fail=true; m.id[0]=2;
    assert(heltec_chat_store_add(s,sender,&m)==RNS_ERROR_IO);
    assert(heltec_chat_store_get(s,0)->count==1); fail=false;
    heltec_chat_store_close(s); assert(heltec_chat_store_open(storage,&s)==RNS_OK);
    assert(heltec_chat_store_get(s,0)->messages[0].timestamp==42);
    assert(!memcmp(heltec_chat_store_get(s,0)->messages[0].text,"hello",5));
    for(unsigned i=2;i<12;++i) { m.id[0]=(uint8_t)i; assert(heltec_chat_store_add(s,sender,&m)==RNS_OK); }
    assert(heltec_chat_store_get(s,0)->count==8);
    assert(heltec_chat_store_delete(s,0)==RNS_OK);
    m.state=1;
    for(unsigned i=0;i<8;++i) { m.id[0]=(uint8_t)i; assert(heltec_chat_store_add(s,sender,&m)==RNS_OK); }
    m.id[0]=99; assert(heltec_chat_store_add(s,sender,&m)==RNS_ERROR_OVERFLOW);
    assert(heltec_chat_store_delete(s,0)==RNS_ERROR_INVALID_STATE);
    for (unsigned i=2;i<=8;++i) { sender[0]=(uint8_t)i; assert(heltec_chat_store_add(s,sender,&m)==RNS_OK); }
    sender[0]=9; assert(heltec_chat_store_add(s,sender,&m)==RNS_ERROR_OVERFLOW);
    assert(heltec_chat_store_get(s,0)->sender[0]==1);
    sender[0]=2;
    fail=true;
    assert(heltec_chat_store_set_state(s,sender,m.id,3)==RNS_ERROR_IO);
    assert(heltec_chat_store_get(s,1)->messages[0].state==1);
    fail=false;
    assert(heltec_chat_store_set_state(s,sender,m.id,3)==RNS_OK);
    assert(heltec_chat_store_set_state(s,sender,m.id,1)==RNS_ERROR_INVALID_STATE);
    assert(heltec_chat_store_get(s,1)->messages[0].state==3);
    heltec_chat_store_close(s);
    records[0][0]=99; assert(heltec_chat_store_open(storage,&s)==RNS_ERROR_PROTOCOL && !s);
    rns_storage_destroy(storage); return 0;
}
