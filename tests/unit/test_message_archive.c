/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "message_archive.h"
#include "archive_view.h"
#include "archive_scan.h"
#include <assert.h>
#include <string.h>
static uint8_t records[64][1024]; static size_t sizes[64]; static bool fail;
static size_t slot(const char *k) { assert(!strncmp(k,"msg",3)); return (size_t)(k[3]-'0')*10U+(size_t)(k[4]-'0'); }
static rns_status_t read_record(void *c,const char *k,uint8_t *out,size_t cap,size_t *n) {
    (void)c; size_t i=slot(k); assert(i<64); if(!sizes[i]) return RNS_ERROR_NOT_FOUND;
    assert(cap>=sizes[i]); memcpy(out,records[i],sizes[i]); *n=sizes[i]; return RNS_OK;
}
static rns_status_t write_record(void *c,const char *k,const uint8_t *data,size_t n) {
    (void)c; if(fail) return RNS_ERROR_IO; size_t i=slot(k); assert(i<64 && n<=1024);
    memcpy(records[i],data,n); sizes[i]=n; return RNS_OK;
}
static rns_status_t remove_record(void *c,const char *k) { (void)c; if(fail) return RNS_ERROR_IO; sizes[slot(k)]=0; return RNS_OK; }
int main(void) {
    heltec_archive_scan scan={.cursor=64}; size_t selected=99;
    assert(!heltec_archive_scan_next(&scan,0,&selected));
    heltec_archive_scan_request(&scan);
    assert(heltec_archive_scan_next(&scan,0,&selected) && selected==0);
    heltec_archive_scan_complete(&scan,0,true);
    /* Failed first record stays scheduled; new announces do not accelerate it. */
    heltec_archive_scan_request(&scan);
    assert(!heltec_archive_scan_next(&scan,4999,&selected));
    uint64_t now=5000;
    for(size_t i=1;i<64;++i,now+=250U) {
        assert(heltec_archive_scan_next(&scan,now,&selected) && selected==i);
        heltec_archive_scan_complete(&scan,now,false);
        assert(!heltec_archive_scan_next(&scan,now,&selected));
    }
    assert(heltec_archive_scan_next(&scan,now,&selected) && selected==0);
    heltec_archive_scan_complete(&scan,now,false);
    for(size_t i=1;i<64;++i) {
        now+=250U; assert(heltec_archive_scan_next(&scan,now,&selected) && selected==i);
        heltec_archive_scan_complete(&scan,now,i==63);
    }
    assert(!heltec_archive_scan_next(&scan,now+4999U,&selected));
    now+=5000U;
    /* Failed last record restarts a bounded sweep, then quiesces after success. */
    for(size_t i=0;i<64;++i,now+=250U) {
        assert(heltec_archive_scan_next(&scan,now,&selected) && selected==i);
        heltec_archive_scan_complete(&scan,now,false);
    }
    assert(!heltec_archive_scan_next(&scan,now,&selected));
    heltec_archive_scan_request(&scan);
    assert(heltec_archive_scan_next(&scan,UINT64_MAX-1U,&selected));
    heltec_archive_scan_complete(&scan,UINT64_MAX-1U,true);
    assert(!heltec_archive_scan_next(&scan,UINT64_MAX,&selected));
    rns_storage_ops_t ops={.read=read_record,.write_atomic=write_record,.remove=remove_record};
    rns_storage_t *storage; assert(rns_storage_create(&ops,NULL,&storage)==RNS_OK);
    heltec_message_archive *a; assert(heltec_message_archive_open(storage,&a)==RNS_OK);
    heltec_archived_message m={.signature=LXMF_SIGNATURE_UNVERIFIED,.text_length=5,.packet_length=3};
    memcpy(m.text,"hello",5); memcpy(m.packet,"raw",3); m.source[0]=1;
    for(unsigned i=0;i<4;++i) { m.id[0]=(uint8_t)i; assert(heltec_message_archive_put(a,&m)==RNS_OK); }
    m.id[0]=5; assert(heltec_message_archive_put(a,&m)==RNS_ERROR_OVERFLOW);
    m.source[0]=2; assert(heltec_message_archive_put(a,&m)==RNS_OK);
    m.source[0]=3; m.id[0]=6; assert(heltec_message_archive_put(a,&m)==RNS_ERROR_OVERFLOW);
    m.source[0]=1; m.id[0]=0; m.signature=LXMF_SIGNATURE_VERIFIED;
    fail=true; assert(heltec_message_archive_put(a,&m)==RNS_ERROR_IO);
    assert(heltec_message_archive_get(a,0)->signature==LXMF_SIGNATURE_UNVERIFIED);
    fail=false; assert(heltec_message_archive_put(a,&m)==RNS_OK);
    m.signature=LXMF_SIGNATURE_UNVERIFIED; assert(heltec_message_archive_put(a,&m)==RNS_ERROR_INVALID_STATE);
    m.id[0]=1; m.signature=LXMF_SIGNATURE_FAILED; assert(heltec_message_archive_put(a,&m)==RNS_OK);
    assert(heltec_message_archive_get(a,1)->text_length==0);
    heltec_archive_view view={0}; char lines[8][22];
    assert(!heltec_archive_view_poll(&view,a,false,false,lines));
    assert(!strcmp(lines[1],"INVALID SIGNATURE") && !strcmp(lines[2],"CONTENT HIDDEN"));
    assert(!heltec_archive_view_poll(&view,a,true,false,lines));
    assert(!strcmp(lines[1],"UNVERIFIED SENDER") && !strncmp(lines[2],"hello",5));
    assert(!heltec_archive_view_poll(&view,a,false,true,lines));
    assert(strstr(lines[2],"CANCEL"));
    assert(heltec_archive_view_poll(&view,a,false,true,lines));
    assert(!heltec_archive_view_poll(&view,a,false,true,lines));
    assert(!heltec_archive_view_poll(&view,a,true,false,lines));
    fail=true;
    assert(!heltec_archive_view_poll(&view,a,false,true,lines));
    assert(!view.deleted && heltec_message_archive_get(a,2)); fail=false;
    assert(!heltec_archive_view_poll(&view,a,false,true,lines));
    assert(!heltec_archive_view_poll(&view,a,true,false,lines));
    assert(!heltec_archive_view_poll(&view,a,false,true,lines));
    assert(view.deleted && !heltec_message_archive_get(a,2));
    heltec_message_archive_close(a); assert(heltec_message_archive_open(storage,&a)==RNS_OK);
    assert(heltec_message_archive_get(a,0)->signature==LXMF_SIGNATURE_VERIFIED);
    assert(!memcmp(heltec_message_archive_get(a,0)->packet,"raw",3));
    assert(heltec_message_archive_get(a,1)->text_length==0);
    fail=true; assert(heltec_message_archive_remove(a,1)==RNS_ERROR_IO);
    assert(heltec_message_archive_get(a,1)); fail=false;
    assert(heltec_message_archive_remove(a,1)==RNS_OK);
    assert(!heltec_message_archive_get(a,1));
    heltec_message_archive_close(a); assert(heltec_message_archive_open(storage,&a)==RNS_OK);
    assert(!heltec_message_archive_get(a,1)); heltec_message_archive_close(a);
    /* A structurally valid stored record cannot bypass startup sender quotas. */
    memcpy(records[5],records[4],sizes[4]); sizes[5]=sizes[4];
    records[5][6]=3; records[5][22]=9;
    assert(heltec_message_archive_open(storage,&a)==RNS_ERROR_PROTOCOL && !a);
    memset(sizes,0,sizeof(sizes));
    assert(heltec_message_archive_open(storage,&a)==RNS_OK);
    m.source[0]=1;
    for(unsigned i=0;i<4;++i) {
        m.id[0]=(uint8_t)i; m.signature=LXMF_SIGNATURE_UNVERIFIED;
        assert(heltec_message_archive_put(a,&m)==RNS_OK);
        m.signature=LXMF_SIGNATURE_FAILED;
        assert(heltec_message_archive_put(a,&m)==RNS_OK);
    }
    m.id[0]=7; m.signature=LXMF_SIGNATURE_UNVERIFIED;
    assert(heltec_message_archive_put(a,&m)==RNS_ERROR_OVERFLOW);
    heltec_message_archive_close(a); memset(sizes,0,sizeof(sizes));
    assert(heltec_message_archive_open(storage,&a)==RNS_OK);
    m.signature=LXMF_SIGNATURE_VERIFIED;
    m.text_length=384; m.packet_length=500;
    for(unsigned sender=0;sender<8;++sender) {
        m.source[0]=(uint8_t)sender;
        for(unsigned index=0;index<8;++index) {
            m.id[0]=(uint8_t)(sender*8+index);
            assert(heltec_message_archive_put(a,&m)==RNS_OK);
        }
    }
    m.source[0]=9; m.id[0]=65;
    assert(heltec_message_archive_put(a,&m)==RNS_ERROR_OVERFLOW);
    m.packet_length=501;
    assert(heltec_message_archive_put(a,&m)==RNS_ERROR_INVALID_ARGUMENT);
    heltec_message_archive_close(a);
    assert(heltec_message_archive_open(storage,&a)==RNS_OK);
    assert(heltec_message_archive_get(a,63)->packet_length==500);
    heltec_message_archive_close(a);
    records[0][0]=2;
    assert(heltec_message_archive_open(storage,&a)==RNS_ERROR_PROTOCOL && !a);
    rns_storage_destroy(storage); return 0;
}
