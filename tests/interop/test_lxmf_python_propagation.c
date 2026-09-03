#include "reticulum/lxmf_propagation.h"
#include "../fixtures/lxmf_propagation_vectors.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

static lxmf_status_t roundtrip(unsigned kind,const uint8_t *in,size_t len,
                               uint8_t *out,size_t cap,size_t *written) {
    lxmf_status_t s;
    if(kind==0u) {
        lxmf_pn_announce_t a;
        s=lxmf_pn_announce_decode(in,len,&a);
        return s==LXMF_OK?lxmf_pn_announce_encode(&a,out,cap,written):s;
    }
    if(kind==1u) {
        lxmf_pn_upload_t u;
        s=lxmf_pn_upload_decode(in,len,&u);
        return s==LXMF_OK?lxmf_pn_upload_encode(&u,out,cap,written):s;
    }
    if(kind==2u) {
        lxmf_pn_get_request_t q;
        s=lxmf_pn_get_request_decode(in,len,&q);
        return s==LXMF_OK?lxmf_pn_get_request_encode(&q,out,cap,written):s;
    }
    if(kind==5u) {
        s=lxmf_pn_upload_rejection_decode(in,len);
        return s==LXMF_OK?lxmf_pn_upload_rejection_encode(out,cap,written):s;
    }
    lxmf_pn_get_response_t p;
    s=lxmf_pn_get_response_decode(in,len,kind==3u,&p);
    return s==LXMF_OK?lxmf_pn_get_response_encode(&p,kind==3u,out,cap,written):s;
}

static void fixtures(void) {
    uint8_t out[8192],mut[8192]; size_t written;
    for(size_t i=0;i<sizeof pn_fixtures/sizeof pn_fixtures[0];i++) {
        const pn_fixture *f=&pn_fixtures[i];
        assert(roundtrip(f->kind,f->wire,f->length,out,sizeof out,&written)==LXMF_OK);
        assert(written==f->length&&memcmp(out,f->wire,written)==0);
        written=12345u;
        assert(roundtrip(f->kind,f->wire,f->length,out,f->length-1u,&written)==LXMF_ERR_BOUNDS);
        assert(written==12345u);
        for(size_t n=0;n<f->length;n++) assert(roundtrip(f->kind,f->wire,n,out,sizeof out,&written)!=LXMF_OK);
        memcpy(mut,f->wire,f->length); mut[f->length]=0xc0;
        assert(roundtrip(f->kind,mut,f->length+1u,out,sizeof out,&written)!=LXMF_OK);
        for(size_t n=0;n<f->length;n++) {
            memcpy(mut,f->wire,f->length); mut[n]^=0x80u;
            (void)roundtrip(f->kind,mut,f->length,out,sizeof out,&written);
        }
    }
    lxmf_pn_announce_t a;
    assert(lxmf_pn_announce_decode(pn_fixtures[0].wire,pn_fixtures[0].length,&a)==LXMF_OK);
    assert(a.enabled&&!a.legacy_support&&a.timebase==1700000000u);
    assert(a.stamp_cost==16&&a.stamp_flexibility==3&&a.peering_cost==18);
    assert(!a.transfer_limit_kb.is_float&&a.transfer_limit_kb.integer==256);
    assert(lxmf_pn_announce_decode(pn_fixtures[1].wire,pn_fixtures[1].length,&a)==LXMF_OK);
    assert(!a.enabled&&a.transfer_limit_kb.is_float&&a.transfer_limit_kb.real==256.5);
    assert(lxmf_pn_announce_decode(pn_fixtures[2].wire,pn_fixtures[2].length,&a)==LXMF_OK);
    assert(a.extension_count==1&&a.extensions_msgpack.len>0);
}

static void bounds(void) {
    uint8_t out[20000],id[32]={0}; size_t n=0;
    lxmf_pn_get_request_t q={0},decoded;
    q.wants_null=true; q.haves_null=true;
    assert(lxmf_pn_get_request_encode(&q,out,sizeof out,&n)==LXMF_OK);
    assert(n==3&&out[0]==0x92&&out[1]==0xc0&&out[2]==0xc0);
    q.wants_null=false; q.haves_null=false;
    assert(lxmf_pn_get_request_encode(&q,out,sizeof out,&n)==LXMF_OK);
    assert(n==3&&out[1]==0x90&&out[2]==0x90);
    q.wants_count=LXMF_PN_MAX_ITEMS; q.haves_count=LXMF_PN_MAX_ITEMS;
    for(size_t i=0;i<LXMF_PN_MAX_ITEMS;i++) q.wants[i]=q.haves[i]=(lxmf_slice_t){id,sizeof id};
    q.has_limit=true; q.limit_kb.is_float=true; q.limit_kb.real=12.5;
    assert(lxmf_pn_get_request_encode(&q,out,sizeof out,&n)==LXMF_OK);
    assert(lxmf_pn_get_request_decode(out,n,&decoded)==LXMF_OK);
    assert(decoded.wants_count==LXMF_PN_MAX_ITEMS&&decoded.limit_kb.real==12.5);
    q.wants_count++; assert(lxmf_pn_get_request_encode(&q,out,sizeof out,&n)==LXMF_ERR_FORMAT); q.wants_count--;
    q.wants_null=true; assert(lxmf_pn_get_request_encode(&q,out,sizeof out,&n)==LXMF_ERR_FORMAT); q.wants_null=false;
    q.wants[0].len=31; assert(lxmf_pn_get_request_encode(&q,out,sizeof out,&n)==LXMF_ERR_FORMAT); q.wants[0].len=32;
    q.limit_kb.real=NAN; assert(lxmf_pn_get_request_encode(&q,out,sizeof out,&n)==LXMF_ERR_FORMAT);
    q.limit_kb.real=-1; assert(lxmf_pn_get_request_encode(&q,out,sizeof out,&n)==LXMF_ERR_FORMAT);
    const uint8_t wrong[]={0x92,0x91,0xc4,1,0,0x90};
    memset(&decoded,0xa5,sizeof decoded); lxmf_pn_get_request_t original=decoded;
    assert(lxmf_pn_get_request_decode(wrong,sizeof wrong,&decoded)==LXMF_ERR_FORMAT);
    assert(memcmp(&decoded,&original,sizeof decoded)==0);
    const uint8_t huge[]={0x92,0xdd,0xff,0xff,0xff,0xff,0x90};
    assert(lxmf_pn_get_request_decode(huge,sizeof huge,&decoded)==LXMF_ERR_FORMAT);
    assert(lxmf_pn_get_request_decode(out,LXMF_PN_MAX_WIRE+1u,&decoded)==LXMF_ERR_BOUNDS);
    lxmf_pn_announce_t a={0};
    uint8_t nested[43]={0x81,0}; memset(nested+2,0x91,40); nested[42]=0;
    a.metadata_msgpack=(lxmf_slice_t){nested,sizeof nested};
    assert(lxmf_pn_announce_encode(&a,out,sizeof out,&n)==LXMF_ERR_FORMAT);
    const uint8_t ext[]={0x81,0xff,0xd4,0x2a,0x01};
    a.metadata_msgpack=(lxmf_slice_t){ext,sizeof ext};
    assert(lxmf_pn_announce_encode(&a,out,sizeof out,&n)==LXMF_OK);
    assert(lxmf_pn_announce_decode(out,n,&a)==LXMF_OK);
    assert(a.metadata_msgpack.len==sizeof ext&&memcmp(a.metadata_msgpack.data,ext,sizeof ext)==0);
    lxmf_pn_upload_t u={0}; u.timebase=INFINITY;
    assert(lxmf_pn_upload_encode(&u,out,sizeof out,&n)==LXMF_ERR_FORMAT);
    lxmf_pn_get_response_t p={0},response;
    uint8_t blob[256]={0}; p.count=1; p.items[0]=(lxmf_slice_t){blob,sizeof blob};
    assert(lxmf_pn_get_response_encode(&p,false,out,sizeof out,&n)==LXMF_OK);
    assert(out[1]==0xc5&&out[2]==1&&out[3]==0);
    assert(lxmf_pn_get_response_decode(out,n,false,&response)==LXMF_OK);
    assert(response.items[0].len==sizeof blob);
    assert(lxmf_pn_get_response_decode(out,n,true,&response)==LXMF_ERR_FORMAT);
    p.count=0; p.kind=LXMF_PN_RESPONSE_ERROR; p.error=0xf1;
    assert(lxmf_pn_get_response_encode(&p,false,out,sizeof out,&n)==LXMF_OK);
    assert(n==2&&out[0]==0xcc&&out[1]==0xf1);
    p.error=1; assert(lxmf_pn_get_response_encode(&p,false,out,sizeof out,&n)==LXMF_ERR_FORMAT);
    p.kind=LXMF_PN_RESPONSE_NIL;
    assert(lxmf_pn_get_response_encode(&p,false,out,sizeof out,&n)==LXMF_OK);
    assert(n==1&&out[0]==0xc0);
    assert(lxmf_pn_get_response_decode(out,n,false,&response)==LXMF_OK&&response.kind==LXMF_PN_RESPONSE_NIL);
}
int main(void) {
    fixtures(); bounds();
    puts("Pinned Python propagation codec fixtures and malformed corpus passed");
    return 0;
}
