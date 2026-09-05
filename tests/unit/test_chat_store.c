/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "chat_store.h"
#include "chat_view.h"
#include <assert.h>
#include <string.h>
static uint8_t records[8][4096]; static size_t lengths[8]; static bool fail;
static unsigned reply_calls;
static unsigned cancel_calls;
static rns_status_t reply_cancel(void *context,const uint8_t id[32]) {
    (void)context; assert(id[0]==7); ++cancel_calls; return RNS_OK;
}
static rns_status_t reply_send(void *context,const uint8_t sender[16],const char *text) {
    (void)context; assert(sender[0]==1 && !strcmp(text,"I'm okay")); ++reply_calls; return RNS_OK;
}
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
    heltec_chat_view view={0}; char lines[8][22];
    assert(!heltec_chat_view_poll(&view,s,false,false,lines));
    assert(!strcmp(lines[0],"CHATS 1"));
    assert(!heltec_chat_view_poll(&view,s,false,true,lines) && view.screen==1);
    assert(!strcmp(lines[2],"hello"));
    assert(!heltec_chat_view_poll(&view,s,false,true,lines) && view.screen==2);
    view.action=3;
    assert(!heltec_chat_view_poll(&view,s,false,true,lines) && view.screen==3 && view.action==0);
    assert(!heltec_chat_view_poll(&view,s,false,true,lines) && view.screen==2);
    assert(heltec_chat_store_get(s,0)); /* Default confirmation cancels. */
    view.send_reply=reply_send; view.action=2;
    assert(!heltec_chat_view_poll(&view,s,false,true,lines) && view.screen==4);
    assert(!heltec_chat_view_poll(&view,s,false,true,lines) && view.screen==5 && !reply_calls);
    assert(!heltec_chat_view_poll(&view,s,false,true,lines) && view.screen==4 && !reply_calls);
    assert(!heltec_chat_view_poll(&view,s,false,true,lines) && view.screen==5);
    assert(!heltec_chat_view_poll(&view,s,true,false,lines) && !reply_calls);
    assert(!heltec_chat_view_poll(&view,s,false,true,lines) && reply_calls==1 && view.screen==1);
    view.error=RNS_ERROR_UNSUPPORTED;
    memcpy(view.reply_error,"NEED RATCHET ANNOUNCE",sizeof("NEED RATCHET ANNOUNCE"));
    assert(!heltec_chat_view_poll(&view,s,false,false,lines));
    assert(!strcmp(lines[6],"NEED RATCHET ANNOUNCE"));
    memcpy(view.reply_error,"STAMP COST 8 UNSUPP",sizeof("STAMP COST 8 UNSUPP"));
    assert(!heltec_chat_view_poll(&view,s,false,false,lines));
    assert(!strcmp(lines[6],"STAMP COST 8 UNSUPP"));
    view.error=RNS_OK;
    uint8_t second[16]={2};
    assert(heltec_chat_store_add(s,second,&m)==RNS_OK);
    view.screen=0; memcpy(view.sender,second,16); view.selected=true;
    assert(!heltec_chat_view_poll(&view,s,false,true,lines) && view.screen==1);
    assert(!heltec_chat_view_poll(&view,s,false,true,lines) && view.screen==2);
    view.action=3;
    assert(!heltec_chat_view_poll(&view,s,false,true,lines) && view.screen==3);
    assert(!heltec_chat_view_poll(&view,s,true,false,lines) && view.action==1);
    assert(!heltec_chat_view_poll(&view,s,false,true,lines));
    assert(!heltec_chat_store_get(s,1) && heltec_chat_store_get(s,0));
    for(unsigned i=2;i<12;++i) { m.id[0]=(uint8_t)i; assert(heltec_chat_store_add(s,sender,&m)==RNS_OK); }
    assert(heltec_chat_store_get(s,0)->count==8);
    assert(heltec_chat_can_rotate(heltec_chat_store_get(s,0),8,true));
    assert(!heltec_chat_can_rotate(heltec_chat_store_get(s,0),8,false));
    assert(!heltec_chat_can_rotate(heltec_chat_store_get(s,0),9,true));
    heltec_chat full_pending=*heltec_chat_store_get(s,0);
    for(size_t i=0;i<8;++i) full_pending.messages[i].state=1;
    assert(!heltec_chat_can_rotate(&full_pending,8,true));
    m.id[0]=99;
    assert(heltec_chat_store_add(s,sender,&m)==RNS_OK);
    assert(heltec_chat_store_get(s,0)->count==8 && heltec_chat_store_get(s,0)->messages[0].id[0]==99);
    assert(heltec_chat_store_delete(s,0)==RNS_OK);
    m.state=1;
    for(unsigned i=0;i<8;++i) { m.id[0]=(uint8_t)i; assert(heltec_chat_store_add(s,sender,&m)==RNS_OK); }
    view.screen=1; view.selected=true; view.message_selected=true;
    memcpy(view.sender,sender,16); memcpy(view.message,m.id,32); view.cancel_reply=reply_cancel;
    assert(!heltec_chat_view_poll(&view,s,false,true,lines) && view.screen==2);
    view.action=4;
    assert(!heltec_chat_view_poll(&view,s,false,true,lines) && view.screen==6);
    assert(!heltec_chat_view_poll(&view,s,false,true,lines) && !cancel_calls);
    assert(!heltec_chat_view_poll(&view,s,false,true,lines) && view.screen==2);
    view.action=4;
    assert(!heltec_chat_view_poll(&view,s,false,true,lines) && view.screen==6);
    assert(!heltec_chat_view_poll(&view,s,true,false,lines));
    assert(!heltec_chat_view_poll(&view,s,false,true,lines) && cancel_calls==1);
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
    sender[0]=3;
    assert(heltec_chat_store_set_state(s,sender,m.id,5)==RNS_OK);
    assert(heltec_chat_store_set_state(s,sender,m.id,1)==RNS_ERROR_INVALID_STATE);
    heltec_chat_store_close(s); assert(heltec_chat_store_open(storage,&s)==RNS_OK);
    assert(heltec_chat_store_get(s,2)->messages[0].state==5);
    heltec_chat_store_close(s);
    records[0][0]=99; assert(heltec_chat_store_open(storage,&s)==RNS_ERROR_PROTOCOL && !s);
    rns_storage_destroy(storage); return 0;
}
