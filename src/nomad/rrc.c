#include "reticulum/rrc.h"
#include <limits.h>
#include <stdbool.h>
#include <string.h>
#define MAX_DEPTH 8u
#define MAX_ITEMS 1024u
typedef struct {const uint8_t *p,*end;size_t items;} reader_t;
typedef struct {uint8_t *p;size_t left;} writer_t;
static bool utf8(const uint8_t *s,size_t n){size_t i=0;while(i<n){uint8_t f=s[i++];if(f<0x80)continue;size_t c;uint32_t v,min;if(f>=0xc2&&f<=0xdf){c=1;v=f&31;min=0x80;}else if(f>=0xe0&&f<=0xef){c=2;v=f&15;min=0x800;}else if(f>=0xf0&&f<=0xf4){c=3;v=f&7;min=0x10000;}else return false;if(c>n-i)return false;while(c--){uint8_t b=s[i++];if((b&0xc0)!=0x80)return false;v=(v<<6)|(b&0x3f);}if(v<min||v>0x10ffff||(v>=0xd800&&v<=0xdfff))return false;}return true;}
static bool arg(reader_t*r,uint8_t a,uint64_t*v){size_t n=0;if(a<24){*v=a;return true;}if(a==24)n=1;else if(a==25)n=2;else if(a==26)n=4;else if(a==27)n=8;else return false;if((size_t)(r->end-r->p)<n)return false;uint64_t x=0;for(size_t i=0;i<n;i++)x=(x<<8)|*r->p++;if((n==1&&x<24)||(n==2&&x<=UINT8_MAX)||(n==4&&x<=UINT16_MAX)||(n==8&&x<=UINT32_MAX))return false;*v=x;return true;}
static bool head(reader_t*r,uint8_t*m,uint64_t*v){if(r->p==r->end||r->items++>=MAX_ITEMS)return false;uint8_t f=*r->p++;*m=f>>5;return arg(r,f&31,v);}
static bool skip(reader_t*r,unsigned d){uint8_t m;uint64_t v;if(d>MAX_DEPTH||!head(r,&m,&v))return false;if(m==0||m==1||m==7)return true;if(m==2||m==3){if(v>(uint64_t)(r->end-r->p))return false;r->p+=(size_t)v;return true;}if(m==4||m==5){if(m==5&&v>UINT64_MAX/2)return false;uint64_t n=m==5?v*2:v;if(n>MAX_ITEMS)return false;for(uint64_t i=0;i<n;i++)if(!skip(r,d+1))return false;return true;}return m==6&&skip(r,d+1);}
static bool uintv(reader_t*r,uint64_t*v){uint8_t m;return head(r,&m,v)&&m==0;}
static bool stringv(reader_t*r,uint8_t want,rns_rrc_slice_t*s){uint8_t m;uint64_t n;if(!head(r,&m,&n)||m!=want||n>(uint64_t)(r->end-r->p))return false;s->data=r->p;s->length=(size_t)n;r->p+=s->length;return want!=3||utf8(s->data,s->length);}
static bool put(writer_t*w,const void*p,size_t n){if(n>w->left)return false;if(n)memcpy(w->p,p,n);w->p+=n;w->left-=n;return true;}
static bool puthead(writer_t*w,uint8_t m,uint64_t v){uint8_t b[9];size_t n;if(v<24){b[0]=(uint8_t)(((uint64_t)m<<5u)|v);n=1;}else if(v<=UINT8_MAX){b[0]=(uint8_t)((m<<5)|24);b[1]=(uint8_t)v;n=2;}else if(v<=UINT16_MAX){b[0]=(uint8_t)((m<<5)|25);b[1]=(uint8_t)(v>>8);b[2]=(uint8_t)v;n=3;}else if(v<=UINT32_MAX){b[0]=(uint8_t)((m<<5)|26);for(size_t i=0;i<4;i++)b[i+1]=(uint8_t)(v>>(24u-8u*i));n=5;}else{b[0]=(uint8_t)((m<<5)|27);for(size_t i=0;i<8;i++)b[i+1]=(uint8_t)(v>>(56u-8u*i));n=9;}return put(w,b,n);}
static bool putstr(writer_t*w,uint8_t m,rns_rrc_slice_t s){return puthead(w,m,s.length)&&put(w,s.data,s.length);}
static bool typeok(rns_rrc_message_type_t t){switch(t){case RNS_RRC_HELLO:case RNS_RRC_WELCOME:case RNS_RRC_JOIN:case RNS_RRC_JOINED:case RNS_RRC_PART:case RNS_RRC_PARTED:case RNS_RRC_MESSAGE:case RNS_RRC_NOTICE:case RNS_RRC_PING:case RNS_RRC_PONG:case RNS_RRC_ERROR:case RNS_RRC_RESOURCE_ENVELOPE:return true;default:return false;}}
rns_status_t rns_rrc_envelope_parse(const uint8_t*in,size_t n,rns_rrc_envelope_t*e){if(!in||!e||!n||n>RNS_RRC_MAX_ENVELOPE_SIZE)return RNS_ERROR_INVALID_ARGUMENT;memset(e,0,sizeof*e);reader_t r={in,in+n,0};uint8_t m;uint64_t pairs;if(!head(&r,&m,&pairs)||m!=5||pairs>64)return RNS_ERROR_PROTOCOL;uint16_t seen=0;for(uint64_t i=0;i<pairs;i++){uint64_t k,v;if(!uintv(&r,&k))return RNS_ERROR_PROTOCOL;if(k<=7&&(seen&(1u<<k)))return RNS_ERROR_PROTOCOL;if(k<=7)seen|=(uint16_t)(1u<<k);rns_rrc_slice_t s;const uint8_t*start;switch(k){case 0:if(!uintv(&r,&v)||v!=1)return RNS_ERROR_PROTOCOL;e->version=1;break;case 1:if(!uintv(&r,&v)||v>INT_MAX||!typeok((rns_rrc_message_type_t)v))return RNS_ERROR_PROTOCOL;e->type=(rns_rrc_message_type_t)v;break;case 2:if(!stringv(&r,2,&s)||s.length!=8)return RNS_ERROR_PROTOCOL;memcpy(e->message_id,s.data,8);break;case 3:if(!uintv(&r,&e->timestamp_ms))return RNS_ERROR_PROTOCOL;break;case 4:if(!stringv(&r,2,&s)||s.length!=16)return RNS_ERROR_PROTOCOL;memcpy(e->source,s.data,16);break;case 5:if(!stringv(&r,3,&e->room)||!e->room.length||e->room.length>255)return RNS_ERROR_PROTOCOL;break;case 6:start=r.p;if(!skip(&r,1))return RNS_ERROR_PROTOCOL;e->body_cbor=(rns_rrc_slice_t){start,(size_t)(r.p-start)};break;case 7:if(!stringv(&r,3,&e->nick)||!e->nick.length||e->nick.length>255)return RNS_ERROR_PROTOCOL;break;default:if(!skip(&r,1))return RNS_ERROR_PROTOCOL;}}
uint16_t req=(uint16_t)((1u<<0)|(1u<<1)|(1u<<2)|(1u<<3)|(1u<<4));return (seen&req)==req&&r.p==r.end?RNS_OK:RNS_ERROR_PROTOCOL;}
rns_status_t rns_rrc_envelope_encode(const rns_rrc_envelope_t *e,
                                     uint8_t *out, size_t cap,
                                     size_t *written) {
    if (!e || !out || !written || e->version != 1 || !typeok(e->type) ||
        e->room.length > RNS_RRC_MAX_ROOM_BYTES ||
        e->nick.length > RNS_RRC_MAX_NICK_BYTES ||
        e->body_cbor.length > RNS_RRC_MAX_ENVELOPE_SIZE ||
        (e->room.length &&
         (!e->room.data || !utf8(e->room.data, e->room.length))) ||
        (e->nick.length &&
         (!e->nick.data || !utf8(e->nick.data, e->nick.length))) ||
        (e->body_cbor.length && !e->body_cbor.data))
        return RNS_ERROR_INVALID_ARGUMENT;
    if (e->body_cbor.length) {
        reader_t r = {e->body_cbor.data,
                      e->body_cbor.data + e->body_cbor.length, 0};
        if (!skip(&r, 0) || r.p != r.end) return RNS_ERROR_PROTOCOL;
    }
    size_t count = 5u + (e->room.length ? 1u : 0u) +
                   (e->body_cbor.length ? 1u : 0u) +
                   (e->nick.length ? 1u : 0u);
    writer_t w = {out, cap};
    bool ok = puthead(&w, 5, (uint64_t)count) && puthead(&w, 0, 0) &&
              puthead(&w, 0, 1) && puthead(&w, 0, 1) &&
              puthead(&w, 0, (uint64_t)e->type) && puthead(&w, 0, 2) &&
              puthead(&w, 2, 8) && put(&w, e->message_id, 8) &&
              puthead(&w, 0, 3) && puthead(&w, 0, e->timestamp_ms) &&
              puthead(&w, 0, 4) && puthead(&w, 2, 16) &&
              put(&w, e->source, 16);
    if (ok && e->room.length)
        ok = puthead(&w, 0, 5) && putstr(&w, 3, e->room);
    if (ok && e->body_cbor.length)
        ok = puthead(&w, 0, 6) &&
             put(&w, e->body_cbor.data, e->body_cbor.length);
    if (ok && e->nick.length)
        ok = puthead(&w, 0, 7) && putstr(&w, 3, e->nick);
    if (!ok) return RNS_ERROR_OVERFLOW;
    *written = cap - w.left;
    return *written <= RNS_RRC_MAX_ENVELOPE_SIZE ? RNS_OK
                                                 : RNS_ERROR_OVERFLOW;
}
rns_status_t rns_rrc_cbor_text(const uint8_t*t,size_t n,uint8_t*out,size_t cap,size_t*written){if((!t&&n)||!out||!written||!utf8(t,n))return RNS_ERROR_INVALID_ARGUMENT;writer_t w={out,cap};if(!puthead(&w,3,n)||!put(&w,t,n))return RNS_ERROR_OVERFLOW;*written=cap-w.left;return RNS_OK;}
rns_status_t rns_rrc_cbor_text_parse(const uint8_t *input,
                                     size_t input_length,
                                     rns_rrc_slice_t *text) {
    if (input == NULL || input_length == 0u || text == NULL ||
        input_length > RNS_RRC_MAX_ENVELOPE_SIZE)
        return RNS_ERROR_INVALID_ARGUMENT;
    reader_t reader = {input, input + input_length, 0u};
    rns_rrc_slice_t parsed = {0};
    if (!stringv(&reader, 3u, &parsed) || reader.p != reader.end)
        return RNS_ERROR_PROTOCOL;
    *text = parsed;
    return RNS_OK;
}

rns_status_t rns_rrc_member_list_parse(
    const uint8_t *input, size_t input_length,
    uint8_t (*members)[RNS_RRC_SOURCE_SIZE], size_t capacity,
    size_t *member_count) {
    if (input == NULL || input_length == 0u || member_count == NULL ||
        (capacity != 0u && members == NULL) ||
        input_length > RNS_RRC_MAX_ENVELOPE_SIZE)
        return RNS_ERROR_INVALID_ARGUMENT;
    reader_t reader = {input, input + input_length, 0u};
    uint8_t major = 0u;
    uint64_t count = 0u;
    if (!head(&reader, &major, &count) || major != 4u)
        return RNS_ERROR_PROTOCOL;
    if (count > capacity) return RNS_ERROR_OVERFLOW;
    for (uint64_t i = 0u; i < count; ++i) {
        rns_rrc_slice_t value = {0};
        if (!stringv(&reader, 2u, &value) ||
            value.length != RNS_RRC_SOURCE_SIZE)
            return RNS_ERROR_PROTOCOL;
        memcpy(members[i], value.data, RNS_RRC_SOURCE_SIZE);
    }
    if (reader.p != reader.end) return RNS_ERROR_PROTOCOL;
    *member_count = (size_t)count;
    return RNS_OK;
}
