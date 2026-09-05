/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "chat_journal.h"
#include "chat_store.h"
#include <assert.h>
#include <stdbool.h>
#include <string.h>
static uint8_t flash[65536]; static bool torn;
static rns_status_t rd(void *c,size_t o,uint8_t *p,size_t n) { (void)c; assert(o+n<=sizeof(flash)); memcpy(p,flash+o,n); return RNS_OK; }
static rns_status_t erase(void *c,size_t o,size_t n) { (void)c; assert(o+n<=sizeof(flash)); memset(flash+o,255,n); return RNS_OK; }
static rns_status_t wr(void *c,size_t o,const uint8_t *p,size_t n) {
    (void)c; assert(o+n<=sizeof(flash)); memcpy(flash+o,p,torn?32:n); return torn?RNS_ERROR_IO:RNS_OK;
}
int main(void) {
    memset(flash,255,sizeof(flash)); heltec_chat_flash_ops ops={NULL,rd,erase,wr};
    rns_storage_t *s=NULL; uint8_t out[8]; size_t n=0;
    assert(heltec_chat_journal_open(&ops,&s)==RNS_OK);
    assert(rns_storage_read(s,"chat0",out,sizeof(out),&n)==RNS_ERROR_NOT_FOUND);
    assert(rns_storage_write_atomic(s,"identity",(const uint8_t *)"x",1)==RNS_ERROR_INVALID_ARGUMENT);
    assert(rns_storage_write_atomic(s,"chat0",(const uint8_t *)"old",3)==RNS_OK);
    torn=true; assert(rns_storage_write_atomic(s,"chat0",(const uint8_t *)"new",3)==RNS_ERROR_IO);
    rns_storage_destroy(s); assert(heltec_chat_journal_open(&ops,&s)==RNS_OK);
    assert(rns_storage_read(s,"chat0",out,sizeof(out),&n)==RNS_OK && n==3 && !memcmp(out,"old",3));
    torn=false; assert(rns_storage_write_atomic(s,"chat0",(const uint8_t *)"new",3)==RNS_OK);
    assert(rns_storage_remove(s,"chat0")==RNS_OK);
    rns_storage_destroy(s); assert(heltec_chat_journal_open(&ops,&s)==RNS_OK);
    assert(rns_storage_read(s,"chat0",out,sizeof(out),&n)==RNS_ERROR_NOT_FOUND);
    heltec_chat_store *chats=NULL;
    assert(heltec_chat_store_open(s,&chats)==RNS_OK);
    heltec_chat_message message={.timestamp=123,.length=5};
    memcpy(message.text,"hello",5); message.id[0]=1;
    uint8_t sender[16]={7};
    assert(heltec_chat_store_add(chats,sender,&message)==RNS_OK);
    heltec_chat_store_close(chats); rns_storage_destroy(s);
    assert(heltec_chat_journal_open(&ops,&s)==RNS_OK);
    assert(heltec_chat_store_open(s,&chats)==RNS_OK);
    const heltec_chat *restored=heltec_chat_store_get(chats,0);
    assert(restored && restored->sender[0]==7 && restored->count==1);
    assert(restored->messages[0].length==5 && !memcmp(restored->messages[0].text,"hello",5));
    assert(heltec_chat_store_add(chats,sender,&message)==RNS_OK && restored->count==1);
    heltec_chat_store_close(chats);
    rns_storage_destroy(s); memset(flash,0,sizeof(flash));
    assert(heltec_chat_journal_open(&ops,&s)==RNS_ERROR_PROTOCOL && !s);
    return 0;
}
