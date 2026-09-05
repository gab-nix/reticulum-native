/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "chat_journal.h"
#include "chat_store.h"
#include "message_archive.h"
#include "chat_admission.h"
#include <assert.h>
#include <stdbool.h>
#include <string.h>
static uint8_t flash[HELTEC_CHAT_JOURNAL_BYTES]; static bool torn; static size_t cut=32;
static rns_status_t rd(void *c,size_t o,uint8_t *p,size_t n) { (void)c; assert(o+n<=sizeof(flash)); memcpy(p,flash+o,n); return RNS_OK; }
static rns_status_t erase(void *c,size_t o,size_t n) { (void)c; assert(o+n<=sizeof(flash)); memset(flash+o,255,n); return RNS_OK; }
static rns_status_t wr(void *c,size_t o,const uint8_t *p,size_t n) {
    (void)c; assert(o+n<=sizeof(flash)); memcpy(flash+o,p,torn?cut:n); return torn?RNS_ERROR_IO:RNS_OK;
}
static void first_write_quarantine(const heltec_chat_flash_ops *ops) {
    const char *keys[]={"chat0","msg00","out0"};
    const size_t offsets[]={0,8U*8192U,72U*8192U};
    const size_t cuts[]={0,1,4,20,24,32,2048,4095,4096};
    uint8_t saved[8192],out[8]; size_t n;
    for(size_t k=0;k<3;++k) for(size_t t=0;t<sizeof(cuts)/sizeof(cuts[0]);++t) {
        memset(flash,255,sizeof(flash)); torn=false;
        rns_storage_t *s=NULL; size_t quarantined=99;
        assert(heltec_chat_journal_open(ops,&s)==RNS_OK);
        assert(rns_storage_write_atomic(s,"prefs",(const uint8_t *)"safe",4)==RNS_OK);
        torn=true; cut=cuts[t];
        assert(rns_storage_write_atomic(s,keys[k],(const uint8_t *)"data",4)==RNS_ERROR_IO);
        memcpy(saved,flash+offsets[k],sizeof(saved));
        rns_storage_destroy(s); torn=false;
        assert(heltec_chat_journal_open_report(ops,&s,&quarantined)==RNS_OK);
        assert(rns_storage_read(s,"prefs",out,sizeof(out),&n)==RNS_OK && n==4 && !memcmp(out,"safe",4));
        if(cut && cut<4096) {
            assert(quarantined==1);
            assert(rns_storage_read(s,keys[k],out,sizeof(out),&n)==RNS_ERROR_QUARANTINED);
            assert(rns_storage_write_atomic(s,keys[k],(const uint8_t *)"x",1)==RNS_ERROR_QUARANTINED);
            assert(rns_storage_remove(s,keys[k])==RNS_ERROR_QUARANTINED);
            assert(!memcmp(saved,flash+offsets[k],sizeof(saved)));
            if(k==0) {
                heltec_chat_store *chats=NULL; uint8_t sender[16]={1};
                heltec_chat_message message={.length=1}; message.id[0]=1;
                assert(heltec_chat_store_open(s,&chats)==RNS_OK);
                assert(heltec_chat_store_add(chats,sender,&message)==RNS_OK);
                assert(!heltec_chat_store_get(chats,0) && heltec_chat_store_get(chats,1));
                heltec_chat_store_close(chats);
            } else if(k==1) {
                heltec_message_archive *archive=NULL;
                heltec_archived_message message={.signature=LXMF_SIGNATURE_UNVERIFIED,.packet_length=1};
                message.id[0]=1; message.source[0]=1;
                assert(heltec_message_archive_open(s,&archive)==RNS_OK);
                assert(heltec_message_archive_put(archive,&message)==RNS_OK);
                assert(!heltec_message_archive_get(archive,0) && heltec_message_archive_get(archive,1));
                heltec_message_archive_close(archive);
            }
            assert(!memcmp(saved,flash+offsets[k],sizeof(saved)));
        } else assert(quarantined==0);
        rns_storage_destroy(s);
    }
    /* With no trustworthy record, foreign or ambiguous initialization data
     * must still fail closed; never infer permission to format it. */
    memset(flash,255,sizeof(flash)); flash[0]='R';
    rns_storage_t *s=NULL;
    assert(heltec_chat_journal_open(ops,&s)==RNS_ERROR_PROTOCOL && !s);
}
static void admission_rotation(const heltec_chat_flash_ops *ops) {
    for(unsigned pending=0;pending<2;++pending) {
        memset(flash,255,sizeof(flash)); torn=false;
        rns_storage_t *s=NULL; heltec_chat_store *chats=NULL; heltec_message_archive *archive=NULL;
        assert(heltec_chat_journal_open(ops,&s)==RNS_OK);
        assert(heltec_chat_store_open(s,&chats)==RNS_OK);
        assert(heltec_message_archive_open(s,&archive)==RNS_OK);
        heltec_chat_message m={.length=1,.state=pending?1:0};
        uint8_t sender[16]={0};
        for(unsigned i=0;i<8;++i) { sender[0]=(uint8_t)(i+1); m.id[0]=sender[0];
            assert(heltec_chat_store_add(chats,sender,&m)==RNS_OK); }
        lxmf_message_t incoming={0}; incoming.source[0]=9; incoming.message_id[0]=99;
        assert(!heltec_chat_admission_available(chats,archive,&incoming,false));
        assert(heltec_chat_admission_available(chats,archive,&incoming,true)==!pending);
        incoming.source[0]=1;
        for(unsigned i=0;i<7;++i) { m.id[0]=(uint8_t)(30+i);
            assert(heltec_chat_store_add(chats,incoming.source,&m)==RNS_OK); }
        assert(heltec_chat_admission_available(chats,archive,&incoming,true)==!pending);
        assert(!heltec_chat_admission_available(chats,archive,&incoming,false));
        heltec_archived_message unknown={.signature=LXMF_SIGNATURE_UNVERIFIED,.packet_length=1};
        unknown.source[0]=2; unknown.id[0]=100;
        assert(heltec_message_archive_put(archive,&unknown)==RNS_OK);
        incoming.source[0]=9;
        assert(!heltec_chat_admission_available(chats,archive,&incoming,true));
        /* Seven verified records plus one unverified record fill this chat.
         * Preflight must not accept a reply, even if its placeholder ID matches. */
        incoming.source[0]=2;
        for(unsigned i=0;i<6;++i) { m.id[0]=(uint8_t)(50+i);
            assert(heltec_chat_store_add(chats,incoming.source,&m)==RNS_OK); }
        memcpy(incoming.message_id,m.id,32);
        assert(heltec_chat_admission_available(chats,archive,&incoming,true));
        assert(!heltec_chat_reply_available(chats,archive,incoming.source));
        heltec_message_archive_close(archive); heltec_chat_store_close(chats); rns_storage_destroy(s);
    }
}
int main(void) {
    memset(flash,255,sizeof(flash)); heltec_chat_flash_ops ops={NULL,rd,erase,wr};
    rns_storage_t *s=NULL; uint8_t out[8]; size_t n=0;
    assert(heltec_chat_journal_open(&ops,&s)==RNS_OK);
    assert(rns_storage_read(s,"chat0",out,sizeof(out),&n)==RNS_ERROR_NOT_FOUND);
    assert(rns_storage_write_atomic(s,"identity",(const uint8_t *)"x",1)==RNS_ERROR_INVALID_ARGUMENT);
    assert(rns_storage_write_atomic(s,"chat0",(const uint8_t *)"old",3)==RNS_OK);
    assert(rns_storage_write_atomic(s,"msg00",(const uint8_t *)"first",5)==RNS_OK);
    assert(rns_storage_write_atomic(s,"msg63",(const uint8_t *)"last",4)==RNS_OK);
    assert(rns_storage_write_atomic(s,"out3",(const uint8_t *)"queued",6)==RNS_OK);
    assert(rns_storage_write_atomic(s,"prefs",(const uint8_t *)"off",3)==RNS_OK);
    assert(rns_storage_write_atomic(s,"msg64",(const uint8_t *)"x",1)==RNS_ERROR_INVALID_ARGUMENT);
    assert(rns_storage_write_atomic(s,"out4",(const uint8_t *)"x",1)==RNS_ERROR_INVALID_ARGUMENT);
    torn=true; assert(rns_storage_write_atomic(s,"chat0",(const uint8_t *)"new",3)==RNS_ERROR_IO);
    rns_storage_destroy(s); assert(heltec_chat_journal_open(&ops,&s)==RNS_OK);
    assert(rns_storage_read(s,"msg63",out,sizeof(out),&n)==RNS_OK && n==4 && !memcmp(out,"last",4));
    assert(rns_storage_read(s,"out3",out,sizeof(out),&n)==RNS_OK && n==6 && !memcmp(out,"queued",6));
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
    first_write_quarantine(&ops);
    admission_rotation(&ops);
    return 0;
}
