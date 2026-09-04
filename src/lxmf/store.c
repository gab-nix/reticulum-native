#include "reticulum/lxmf_store.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Journal records share a 16 byte header: "LXMS", version, type, two reserved
 * bytes, a big-endian payload length and a CRC32 over the payload.
 *
 * TYPE_PUT is the original 77 byte fixed prefix followed by the content. It is
 * still read so journals written before signature states are carried load
 * unchanged; such messages are verified by construction, because the only path
 * that stored an inbound message required a verified signature. It is no longer
 * written. TYPE_PUT_V2 added the signature state and original packed message.
 * TYPE_PUT_V3 adds restart-safe delivery metadata; compaction migrates older
 * records without discarding their representable state. TYPE_PUT_V4 widens
 * packed length to 32 bits. TYPE_REPLACE_V4 durably replaces a representation
 * under the same ID; ordinary duplicate puts still retain their first bytes.
 * All prior formats remain readable. */
#define HEADER_SIZE 16u
#define PUT_FIXED 77u
#define PUT2_FIXED 80u
#define PUT3_FIXED 132u
#define PUT4_FIXED 134u
#define STATUS_SIZE 33u
#define SIGNATURE_SIZE 33u
#define REMOVE_SIZE 32u
#define DELIVERY_SIZE 84u
#define MAX_PAYLOAD (PUT4_FIXED + LXMF_STORE_MAX_CONTENT + LXMF_STORE_MAX_PACKED)
#define TYPE_PUT 1u
#define TYPE_STATUS 2u
#define TYPE_PUT_V2 3u
#define TYPE_SIGNATURE 4u
#define TYPE_REMOVE 5u
#define TYPE_PUT_V3 6u
#define TYPE_DELIVERY 7u
#define TYPE_PUT_V4 8u
#define TYPE_REPLACE_V4 9u

typedef struct {
    uint8_t id[32];
    uint8_t source[16];
    uint64_t offset;
    uint32_t content_len;
    uint32_t packed_len;
    uint16_t fixed;
    uint8_t status;
    uint8_t signature;
    lxmf_delivery_metadata_t delivery;
} index_entry;
typedef struct { FILE *file; char path[LXMF_STORE_PATH_MAX+1]; index_entry index[LXMF_STORE_MAX_MESSAGES]; size_t count; } store_impl;

static uint16_t get16(const uint8_t *p){return (uint16_t)(((uint16_t)p[0]<<8)|p[1]);}
static void put32(uint8_t *p,uint32_t v){p[0]=(uint8_t)(v>>24);p[1]=(uint8_t)(v>>16);p[2]=(uint8_t)(v>>8);p[3]=(uint8_t)v;}
static uint32_t get32(const uint8_t *p){return ((uint32_t)p[0]<<24)|((uint32_t)p[1]<<16)|((uint32_t)p[2]<<8)|p[3];}
static void put64(uint8_t *p,uint64_t v){for(unsigned i=0;i<8;i++)p[7-i]=(uint8_t)(v>>(8*i));}
static uint64_t get64(const uint8_t *p){uint64_t v=0;for(unsigned i=0;i<8;i++)v=(v<<8)|p[i];return v;}
static uint32_t crc32_update(uint32_t c,const uint8_t *p,size_t n){while(n--){c^=*p++;for(unsigned i=0;i<8;i++)c=(c>>1)^(0xedb88320u&(~(c&1u)+1u));}return c;}
static uint32_t crc32_bytes(const uint8_t *p,size_t n){return ~crc32_update(0xffffffffu,p,n);}
static bool valid_status(uint8_t s){return s<=LXMF_DELIVERY_FAILED;}
static bool valid_signature(uint8_t s){return s<=LXMF_SIGNATURE_FAILED;}
static bool valid_method(uint8_t method){return method<=LXMF_DELIVERY_METHOD_PROPAGATED;}
static bool valid_queue_reason(uint8_t reason){return reason<=LXMF_QUEUE_REASON_RETRY_EXHAUSTED;}
static bool valid_delivery(const lxmf_delivery_metadata_t *delivery){return delivery&&delivery->desired_method>=LXMF_DELIVERY_METHOD_UNKNOWN&&delivery->desired_method<=LXMF_DELIVERY_METHOD_PROPAGATED&&delivery->actual_method>=LXMF_DELIVERY_METHOD_UNKNOWN&&delivery->actual_method<=LXMF_DELIVERY_METHOD_PROPAGATED&&valid_queue_reason((uint8_t)delivery->queue_reason)&&delivery->progress<=LXMF_DELIVERY_PROGRESS_COMPLETE;}
static index_entry *find(store_impl *s,const uint8_t id[32]){for(size_t i=0;i<s->count;i++)if(memcmp(s->index[i].id,id,32)==0)return &s->index[i];return NULL;}
static lxmf_status_t sync_file(FILE *f){if(fflush(f)!=0)return LXMF_ERR_CRYPTO;return fsync(fileno(f))==0?LXMF_OK:LXMF_ERR_CRYPTO;}
static lxmf_status_t append_record(FILE *f,uint8_t type,const uint8_t *payload,uint32_t n,uint64_t *offset){if(fseek(f,0,SEEK_END)!=0)return LXMF_ERR_CRYPTO;long pos=ftell(f);if(pos<0||(uint64_t)pos+HEADER_SIZE+n>LXMF_STORE_MAX_FILE_SIZE)return LXMF_ERR_BOUNDS;uint8_t h[HEADER_SIZE]={'L','X','M','S',1,type,0,0};put32(h+8,n);put32(h+12,crc32_bytes(payload,n));if(fwrite(h,1,sizeof h,f)!=sizeof h||fwrite(payload,1,n,f)!=n)return LXMF_ERR_CRYPTO;lxmf_status_t st=sync_file(f);if(st==LXMF_OK&&offset)*offset=(uint64_t)pos;return st;}

static void encode_delivery(uint8_t *payload,
                            const lxmf_delivery_metadata_t *delivery) {
    memset(payload, 0, 52u);
    payload[0] = (uint8_t)delivery->desired_method;
    payload[1] = (uint8_t)delivery->actual_method;
    payload[2] = (uint8_t)delivery->queue_reason;
    payload[3] = delivery->has_proof_id ? 1u : 0u;
    put32(payload + 4, delivery->attempts);
    put64(payload + 8, delivery->retry_at_ms);
    put32(payload + 16, delivery->progress);
    if (delivery->has_proof_id)
        memcpy(payload + 20, delivery->proof_id, LXMF_MESSAGE_ID_LENGTH);
}

static bool decode_delivery(const uint8_t *payload,
                            lxmf_delivery_metadata_t *delivery) {
    if (!valid_method(payload[0]) || !valid_method(payload[1]) ||
        !valid_queue_reason(payload[2]) || payload[3] > 1u)
        return false;
    memset(delivery, 0, sizeof *delivery);
    delivery->desired_method = (lxmf_delivery_method_t)payload[0];
    delivery->actual_method = (lxmf_delivery_method_t)payload[1];
    delivery->queue_reason = (lxmf_queue_reason_t)payload[2];
    delivery->has_proof_id = payload[3] != 0u;
    delivery->attempts = get32(payload + 4);
    delivery->retry_at_ms = get64(payload + 8);
    delivery->progress = get32(payload + 16);
    if (delivery->has_proof_id)
        memcpy(delivery->proof_id, payload + 20, LXMF_MESSAGE_ID_LENGTH);
    return valid_delivery(delivery);
}

static bool same_delivery(const lxmf_delivery_metadata_t *left,
                          const lxmf_delivery_metadata_t *right) {
    return left->desired_method == right->desired_method &&
           left->actual_method == right->actual_method &&
           left->attempts == right->attempts &&
           left->queue_reason == right->queue_reason &&
           left->retry_at_ms == right->retry_at_ms &&
           left->progress == right->progress &&
           left->has_proof_id == right->has_proof_id &&
           (!left->has_proof_id ||
            memcmp(left->proof_id, right->proof_id,
                   LXMF_MESSAGE_ID_LENGTH) == 0);
}

/* Serialises only the small fixed prefix; content and opaque representation
 * are streamed separately, so an 8 MiB message needs no stack-sized copy. */
static void encode_put(uint8_t payload[PUT4_FIXED],const lxmf_store_message_t *m){
    memcpy(payload,m->message_id,32);
    memcpy(payload+32,m->destination,16);
    memcpy(payload+48,m->source,16);
    uint64_t bits;memcpy(&bits,&m->timestamp,8);put64(payload+64,bits);
    payload[72]=(uint8_t)m->status;
    put32(payload+73,(uint32_t)m->content.len);
    payload[77]=(uint8_t)m->signature_state;
    put32(payload+78,(uint32_t)m->packed.len);
    encode_delivery(payload+82,&m->delivery);
}

static lxmf_status_t append_put_record(FILE *file,const lxmf_store_message_t *m,uint8_t type,uint64_t *offset){
    uint8_t fixed[PUT4_FIXED];
    encode_put(fixed,m);
    uint32_t n=(uint32_t)(PUT4_FIXED+m->content.len+m->packed.len);
    if(fseek(file,0,SEEK_END)!=0)return LXMF_ERR_CRYPTO;
    long pos=ftell(file);
    if(pos<0||(uint64_t)pos+HEADER_SIZE+n>LXMF_STORE_MAX_FILE_SIZE)return LXMF_ERR_BOUNDS;
    uint32_t crc=crc32_update(0xffffffffu,fixed,sizeof fixed);
    crc=crc32_update(crc,m->content.data,m->content.len);
    crc=crc32_update(crc,m->packed.data,m->packed.len);
    uint8_t h[HEADER_SIZE]={'L','X','M','S',1,type,0,0};
    put32(h+8,n);put32(h+12,~crc);
    if(fwrite(h,1,sizeof h,file)!=sizeof h||
       fwrite(fixed,1,sizeof fixed,file)!=sizeof fixed||
       (m->content.len&&fwrite(m->content.data,1,m->content.len,file)!=m->content.len)||
       (m->packed.len&&fwrite(m->packed.data,1,m->packed.len,file)!=m->packed.len))return LXMF_ERR_CRYPTO;
    lxmf_status_t st=sync_file(file);
    if(st==LXMF_OK&&offset)*offset=(uint64_t)pos;
    return st;
}

static lxmf_status_t append_put(FILE *file,const lxmf_store_message_t *m,uint64_t *offset){
    return append_put_record(file,m,TYPE_PUT_V4,offset);
}

static index_entry *upsert(store_impl *s,const uint8_t *payload,uint64_t offset,uint16_t fixed,uint32_t content_len,uint32_t packed_len){
    index_entry *e=find(s,payload);
    if(e)return e;
    if(s->count>=LXMF_STORE_MAX_MESSAGES)return NULL;
    e=&s->index[s->count++];
    memset(e,0,sizeof *e);
    memcpy(e->id,payload,32);
    memcpy(e->source,payload+48,16);
    e->offset=offset;e->fixed=fixed;e->content_len=content_len;e->packed_len=packed_len;
    return e;
}

static void drop_index(store_impl *s,size_t i){
    if(i+1<s->count)memmove(&s->index[i],&s->index[i+1],(s->count-i-1)*sizeof s->index[0]);
    s->count--;
    memset(&s->index[s->count],0,sizeof s->index[0]);
}

static lxmf_status_t scan(store_impl *s){
    s->count=0;
    if(fseek(s->file,0,SEEK_SET)!=0)return LXMF_ERR_CRYPTO;
    uint64_t good=0;
    for(;;){
        uint8_t h[HEADER_SIZE];
        size_t got=fread(h,1,sizeof h,s->file);
        if(got==0&&feof(s->file))break;
        if(got!=sizeof h)goto recover;
        if(memcmp(h,"LXMS",4)!=0||h[4]!=1||h[5]<TYPE_PUT||h[5]>TYPE_REPLACE_V4)goto recover;
        uint32_t n=get32(h+8);
        if(n==0||n>MAX_PAYLOAD||good+HEADER_SIZE+n>LXMF_STORE_MAX_FILE_SIZE)goto recover;
        uint8_t payload[PUT4_FIXED],chunk[4096];
        size_t prefix=n<sizeof payload?n:sizeof payload;
        if(fread(payload,1,prefix,s->file)!=prefix)goto recover;
        uint32_t crc=crc32_update(0xffffffffu,payload,prefix);
        size_t left=n-prefix;
        while(left){
            size_t take=left<sizeof chunk?left:sizeof chunk;
            if(fread(chunk,1,take,s->file)!=take)goto recover;
            crc=crc32_update(crc,chunk,take);left-=take;
        }
        if(~crc!=get32(h+12))goto recover;
        if(h[5]==TYPE_PUT){
            if(n<PUT_FIXED||n-PUT_FIXED>LXMF_STORE_MAX_CONTENT||get32(payload+73)!=n-PUT_FIXED||!valid_status(payload[72]))goto recover;
            index_entry *e=upsert(s,payload,good,(uint16_t)PUT_FIXED,n-(uint32_t)PUT_FIXED,0u);
            if(!e)return LXMF_ERR_BOUNDS;
            e->status=payload[72];
            e->signature=(uint8_t)LXMF_SIGNATURE_VERIFIED;
        }else if(h[5]==TYPE_PUT_V2){
            if(n<PUT2_FIXED||!valid_status(payload[72])||!valid_signature(payload[77]))goto recover;
            uint32_t content_len=get32(payload+73);
            uint32_t packed_len=get16(payload+78);
            if(content_len>LXMF_STORE_MAX_CONTENT||packed_len>LXMF_STORE_MAX_PACKED||
               n!=PUT2_FIXED+content_len+packed_len)goto recover;
            index_entry *e=upsert(s,payload,good,(uint16_t)PUT2_FIXED,content_len,packed_len);
            if(!e)return LXMF_ERR_BOUNDS;
            e->status=payload[72];
            e->signature=payload[77];
        }else if(h[5]==TYPE_PUT_V3||h[5]==TYPE_PUT_V4||h[5]==TYPE_REPLACE_V4){
            bool wide=h[5]!=TYPE_PUT_V3;
            uint16_t fixed=wide?PUT4_FIXED:PUT3_FIXED;
            if(n<fixed||!valid_status(payload[72])||
               !valid_signature(payload[77]))goto recover;
            uint32_t content_len=get32(payload+73);
            uint32_t packed_len=wide?get32(payload+78):get16(payload+78);
            lxmf_delivery_metadata_t delivery;
            if(content_len>LXMF_STORE_MAX_CONTENT||
               packed_len>LXMF_STORE_MAX_PACKED||
               n!=fixed+content_len+packed_len||
               !decode_delivery(payload+(wide?82:80),&delivery))goto recover;
            if(h[5]==TYPE_REPLACE_V4&&!find(s,payload))goto recover;
            index_entry *e=upsert(s,payload,good,fixed,
                                  content_len,packed_len);
            if(!e)return LXMF_ERR_BOUNDS;
            if(h[5]==TYPE_REPLACE_V4){
                e->offset=good;e->fixed=fixed;
                e->content_len=content_len;e->packed_len=packed_len;
            }
            e->status=payload[72];
            e->signature=payload[77];
            e->delivery=delivery;
        }else if(h[5]==TYPE_STATUS){
            if(n!=STATUS_SIZE||!valid_status(payload[32]))goto recover;
            index_entry *e=find(s,payload);
            if(e)e->status=payload[32];
        }else if(h[5]==TYPE_SIGNATURE){
            if(n!=SIGNATURE_SIZE||!valid_signature(payload[32]))goto recover;
            index_entry *e=find(s,payload);
            if(e)e->signature=payload[32];
        }else if(h[5]==TYPE_REMOVE){
            if(n!=REMOVE_SIZE)goto recover;
            index_entry *e=find(s,payload);
            if(e)drop_index(s,(size_t)(e-s->index));
        }else{
            if(n!=DELIVERY_SIZE)goto recover;
            lxmf_delivery_metadata_t delivery;
            if(!decode_delivery(payload+32,&delivery))goto recover;
            index_entry *e=find(s,payload);
            if(e)e->delivery=delivery;
        }
        good+=HEADER_SIZE+n;
    }
    clearerr(s->file);
    return LXMF_OK;
recover:
    clearerr(s->file);
    if(ftruncate(fileno(s->file),(off_t)good)!=0)return LXMF_ERR_CRYPTO;
    if(fseek(s->file,0,SEEK_END)!=0)return LXMF_ERR_CRYPTO;
    return sync_file(s->file);
}

lxmf_status_t lxmf_store_open(lxmf_store_t *store,const char *path){if(!store||!path||store->implementation||strlen(path)>LXMF_STORE_PATH_MAX)return LXMF_ERR_ARGUMENT;store_impl *s=calloc(1,sizeof *s);if(!s)return LXMF_ERR_BOUNDS;memcpy(s->path,path,strlen(path)+1);s->file=fopen(path,"a+b");if(!s->file){free(s);return LXMF_ERR_CRYPTO;}store->implementation=s;lxmf_status_t st=scan(s);if(st!=LXMF_OK){lxmf_store_close(store);return st;}return LXMF_OK;}
void lxmf_store_close(lxmf_store_t *store){if(!store||!store->implementation)return;store_impl *s=store->implementation;if(s->file)fclose(s->file);free(s);store->implementation=NULL;}
size_t lxmf_store_count(const lxmf_store_t *store){return store&&store->implementation?((store_impl *)store->implementation)->count:0;}

static size_t unverified_total(const store_impl *s){size_t n=0;for(size_t i=0;i<s->count;i++)if(s->index[i].signature!=(uint8_t)LXMF_SIGNATURE_VERIFIED)n++;return n;}
static size_t unverified_from(const store_impl *s,const uint8_t source[16]){size_t n=0;for(size_t i=0;i<s->count;i++)if(s->index[i].signature!=(uint8_t)LXMF_SIGNATURE_VERIFIED&&memcmp(s->index[i].source,source,16)==0)n++;return n;}
static size_t unverified_sources(const store_impl *s){
    size_t n=0;
    for(size_t i=0;i<s->count;i++){
        if(s->index[i].signature==(uint8_t)LXMF_SIGNATURE_VERIFIED)continue;
        bool seen=false;
        for(size_t j=0;j<i&&!seen;j++)
            seen=s->index[j].signature!=(uint8_t)LXMF_SIGNATURE_VERIFIED&&
                 memcmp(s->index[j].source,s->index[i].source,16)==0;
        if(!seen)n++;
    }
    return n;
}

size_t lxmf_store_unverified_count(const lxmf_store_t *store){return store&&store->implementation?unverified_total(store->implementation):0;}

static lxmf_status_t remove_at(store_impl *s,size_t i){
    uint8_t payload[REMOVE_SIZE];
    memcpy(payload,s->index[i].id,32);
    lxmf_status_t st=append_record(s->file,TYPE_REMOVE,payload,REMOVE_SIZE,NULL);
    if(st!=LXMF_OK)return st;
    drop_index(s,i);
    return LXMF_OK;
}

/* Evicts the oldest retained message of whichever unknown sender holds the most
 * of them, so one flooding sender is drained before anyone else's message is.
 * Ties resolve to the earliest journal position, which makes the choice
 * identical across restarts. */
static lxmf_status_t evict_one_unverified(store_impl *s){
    size_t victim=s->count,best=0;
    for(size_t i=0;i<s->count;i++){
        if(s->index[i].signature==(uint8_t)LXMF_SIGNATURE_VERIFIED)continue;
        size_t n=unverified_from(s,s->index[i].source);
        if(n>best){best=n;victim=i;}
    }
    if(victim==s->count)return LXMF_ERR_BOUNDS;
    return remove_at(s,victim);
}

static lxmf_status_t enforce_unverified_caps(store_impl *s,const uint8_t source[16]){
    for(;;){
        size_t total=unverified_total(s);
        bool known=unverified_from(s,source)>0;
        if(total<LXMF_STORE_MAX_UNVERIFIED&&
           (known||unverified_sources(s)<LXMF_STORE_MAX_UNVERIFIED_SOURCES))
            return LXMF_OK;
        lxmf_status_t st=evict_one_unverified(s);
        if(st!=LXMF_OK)return st;
    }
}

lxmf_status_t lxmf_store_put(lxmf_store_t *store,const lxmf_store_message_t *m,bool *inserted){
    if(inserted)*inserted=false;
    if(!store||!store->implementation||!m||m->content.len>LXMF_STORE_MAX_CONTENT||
       (m->content.len&&!m->content.data)||m->packed.len>LXMF_STORE_MAX_PACKED||
       (m->packed.len&&!m->packed.data)||!valid_status((uint8_t)m->status)||
       !valid_signature((uint8_t)m->signature_state)||
       !valid_delivery(&m->delivery))
        return LXMF_ERR_ARGUMENT;
    store_impl *s=store->implementation;
    if(find(s,m->message_id))return LXMF_OK;
    if(m->signature_state!=LXMF_SIGNATURE_VERIFIED){
        lxmf_status_t capped=enforce_unverified_caps(s,m->source);
        if(capped!=LXMF_OK)return capped;
    }
    if(s->count>=LXMF_STORE_MAX_MESSAGES)return LXMF_ERR_BOUNDS;
    uint8_t payload[PUT4_FIXED];
    encode_put(payload,m);
    uint64_t offset;
    lxmf_status_t st=append_put(s->file,m,&offset);
    if(st!=LXMF_OK)return st;
    index_entry *e=upsert(s,payload,offset,(uint16_t)PUT4_FIXED,(uint32_t)m->content.len,(uint32_t)m->packed.len);
    if(!e)return LXMF_ERR_BOUNDS;
    e->status=(uint8_t)m->status;
    e->signature=(uint8_t)m->signature_state;
    e->delivery=m->delivery;
    if(inserted)*inserted=true;
    return LXMF_OK;
}

lxmf_status_t lxmf_store_update_status(lxmf_store_t *store,const uint8_t id[32],lxmf_delivery_status_t status){if(!store||!store->implementation||!id||!valid_status((uint8_t)status))return LXMF_ERR_ARGUMENT;store_impl *s=store->implementation;index_entry *e=find(s,id);if(!e)return LXMF_ERR_FORMAT;if(e->status==(uint8_t)status)return LXMF_OK;uint8_t p[STATUS_SIZE];memcpy(p,id,32);p[32]=(uint8_t)status;lxmf_status_t st=append_record(s->file,TYPE_STATUS,p,sizeof p,NULL);if(st==LXMF_OK)e->status=(uint8_t)status;return st;}
lxmf_status_t lxmf_store_update_signature(lxmf_store_t *store,const uint8_t id[32],lxmf_signature_state_t state){if(!store||!store->implementation||!id||!valid_signature((uint8_t)state))return LXMF_ERR_ARGUMENT;store_impl *s=store->implementation;index_entry *e=find(s,id);if(!e)return LXMF_ERR_FORMAT;if(e->signature==(uint8_t)state)return LXMF_OK;uint8_t p[SIGNATURE_SIZE];memcpy(p,id,32);p[32]=(uint8_t)state;lxmf_status_t st=append_record(s->file,TYPE_SIGNATURE,p,sizeof p,NULL);if(st==LXMF_OK)e->signature=(uint8_t)state;return st;}
lxmf_status_t lxmf_store_update_delivery(lxmf_store_t *store,const uint8_t id[32],const lxmf_delivery_metadata_t *delivery){if(!store||!store->implementation||!id||!valid_delivery(delivery))return LXMF_ERR_ARGUMENT;store_impl *s=store->implementation;index_entry *e=find(s,id);if(!e)return LXMF_ERR_FORMAT;if(same_delivery(&e->delivery,delivery))return LXMF_OK;uint8_t p[DELIVERY_SIZE];memcpy(p,id,32);encode_delivery(p+32,delivery);lxmf_status_t st=append_record(s->file,TYPE_DELIVERY,p,sizeof p,NULL);if(st==LXMF_OK)e->delivery=*delivery;return st;}
lxmf_status_t lxmf_store_remove(lxmf_store_t *store,const uint8_t id[32]){if(!store||!store->implementation||!id)return LXMF_ERR_ARGUMENT;store_impl *s=store->implementation;index_entry *e=find(s,id);if(!e)return LXMF_ERR_FORMAT;return remove_at(s,(size_t)(e-s->index));}

static lxmf_status_t seek_body(store_impl *s,const index_entry *e,uint64_t skip){
    uint64_t at=e->offset+HEADER_SIZE+skip;
    if(at>(uint64_t)LONG_MAX)return LXMF_ERR_BOUNDS;
    return fseek(s->file,(long)at,SEEK_SET)==0?LXMF_OK:LXMF_ERR_CRYPTO;
}

lxmf_status_t lxmf_store_read(lxmf_store_t *store,const uint8_t id[32],lxmf_store_message_t *m,uint8_t *content,size_t cap){
    if(!store||!store->implementation||!id||!m)return LXMF_ERR_ARGUMENT;
    store_impl *s=store->implementation;
    index_entry *e=find(s,id);
    if(!e)return LXMF_ERR_FORMAT;
    if(e->content_len>cap||(e->content_len&&!content))return LXMF_ERR_BOUNDS;
    lxmf_status_t st=seek_body(s,e,0);
    if(st!=LXMF_OK)return st;
    uint8_t fixed[PUT4_FIXED];
    if(fread(fixed,1,e->fixed,s->file)!=e->fixed)return LXMF_ERR_CRYPTO;
    memset(m,0,sizeof *m);
    memcpy(m->message_id,fixed,32);
    memcpy(m->destination,fixed+32,16);
    memcpy(m->source,fixed+48,16);
    uint64_t bits=get64(fixed+64);
    memcpy(&m->timestamp,&bits,8);
    m->status=(lxmf_delivery_status_t)e->status;
    m->signature_state=(lxmf_signature_state_t)e->signature;
    m->delivery=e->delivery;
    m->content.data=content;
    m->content.len=e->content_len;
    if(e->content_len&&fread(content,1,e->content_len,s->file)!=e->content_len)return LXMF_ERR_CRYPTO;
    return LXMF_OK;
}

lxmf_status_t lxmf_store_read_packed(lxmf_store_t *store,const uint8_t id[32],uint8_t *packed,size_t capacity,size_t *packed_len){
    if(!store||!store->implementation||!id||!packed||!packed_len)return LXMF_ERR_ARGUMENT;
    store_impl *s=store->implementation;
    index_entry *e=find(s,id);
    if(!e)return LXMF_ERR_FORMAT;
    if(e->packed_len==0u)return LXMF_ERR_FORMAT;
    if(e->packed_len>capacity)return LXMF_ERR_BOUNDS;
    lxmf_status_t st=seek_body(s,e,(uint64_t)e->fixed+e->content_len);
    if(st!=LXMF_OK)return st;
    if(fread(packed,1,e->packed_len,s->file)!=e->packed_len)return LXMF_ERR_CRYPTO;
    *packed_len=e->packed_len;
    return LXMF_OK;
}

lxmf_status_t lxmf_store_packed_size(lxmf_store_t *store,const uint8_t id[32],size_t *packed_len){
    if(!store||!store->implementation||!id||!packed_len)return LXMF_ERR_ARGUMENT;
    *packed_len=0;
    index_entry *e=find(store->implementation,id);
    if(!e||!e->packed_len)return LXMF_ERR_FORMAT;
    *packed_len=e->packed_len;
    return LXMF_OK;
}

lxmf_status_t lxmf_store_update_packed(lxmf_store_t *store,
    const uint8_t id[32], const uint8_t *packed, size_t packed_len) {
    if (!store || !store->implementation || !id || !packed ||
        !packed_len || packed_len > LXMF_STORE_MAX_PACKED)
        return LXMF_ERR_ARGUMENT;
    lxmf_message_t parsed;
    lxmf_status_t status = lxmf_unpack(packed, packed_len, NULL, NULL, &parsed);
    if (status != LXMF_OK) return status;
    uint8_t content[LXMF_STORE_MAX_CONTENT];
    lxmf_store_message_t message;
    status = lxmf_store_read(store, id, &message, content, sizeof content);
    if (status != LXMF_OK) return status;
    if (memcmp(parsed.message_id, id, 32u) != 0 ||
        memcmp(parsed.source, message.source, 16u) != 0 ||
        memcmp(parsed.destination, message.destination, 16u) != 0)
        return LXMF_ERR_FORMAT;
    message.packed = (lxmf_slice_t){packed, packed_len};
    store_impl *s = store->implementation;
    uint64_t offset;
    status = append_put_record(s->file, &message, TYPE_REPLACE_V4, &offset);
    if (status != LXMF_OK) return status;
    uint8_t fixed[PUT4_FIXED];
    encode_put(fixed, &message);
    index_entry *entry = upsert(s, fixed, offset, PUT4_FIXED,
        (uint32_t)message.content.len, (uint32_t)packed_len);
    if (!entry) return LXMF_ERR_BOUNDS;
    entry->offset = offset;
    entry->fixed = PUT4_FIXED;
    entry->content_len = (uint32_t)message.content.len;
    entry->packed_len = (uint32_t)packed_len;
    entry->status = (uint8_t)message.status;
    entry->signature = (uint8_t)message.signature_state;
    entry->delivery = message.delivery;
    return LXMF_OK;
}

lxmf_status_t lxmf_store_read_delivery(lxmf_store_t *store,const uint8_t id[32],lxmf_delivery_metadata_t *delivery){if(!store||!store->implementation||!id||!delivery)return LXMF_ERR_ARGUMENT;store_impl *s=store->implementation;index_entry *e=find(s,id);if(!e)return LXMF_ERR_FORMAT;*delivery=e->delivery;return LXMF_OK;}

lxmf_status_t lxmf_store_list(lxmf_store_t *store,lxmf_store_list_fn cb,void *ctx){if(!store||!store->implementation||!cb)return LXMF_ERR_ARGUMENT;store_impl *s=store->implementation;uint8_t content[LXMF_STORE_MAX_CONTENT];for(size_t i=0;i<s->count;i++){lxmf_store_message_t m;lxmf_status_t st=lxmf_store_read(store,s->index[i].id,&m,content,sizeof content);if(st!=LXMF_OK)return st;if(!cb(ctx,&m))break;}return LXMF_OK;}

lxmf_status_t lxmf_store_compact(lxmf_store_t *store){
    if(!store||!store->implementation)return LXMF_ERR_ARGUMENT;
    store_impl *s=store->implementation;
    char tmp[LXMF_STORE_PATH_MAX+5];
    if(strlen(s->path)+4>=sizeof tmp)return LXMF_ERR_BOUNDS;
    snprintf(tmp,sizeof tmp,"%s.tmp",s->path);
    FILE *out=fopen(tmp,"w+b");
    if(!out)return LXMF_ERR_CRYPTO;
    uint8_t content[LXMF_STORE_MAX_CONTENT];
    lxmf_status_t st=LXMF_OK;
    for(size_t i=0;i<s->count&&st==LXMF_OK;i++){
        lxmf_store_message_t m;
        size_t packed_len=0;
        uint8_t *packed=NULL;
        st=lxmf_store_read(store,s->index[i].id,&m,content,sizeof content);
        if(st!=LXMF_OK)break;
        if(s->index[i].packed_len){
            packed=malloc(s->index[i].packed_len);
            if(!packed){st=LXMF_ERR_BOUNDS;break;}
            st=lxmf_store_read_packed(store,s->index[i].id,packed,s->index[i].packed_len,&packed_len);
            if(st!=LXMF_OK){free(packed);break;}
            m.packed=(lxmf_slice_t){packed,packed_len};
        }
        st=append_put(out,&m,NULL);
        free(packed);
    }
    if(st==LXMF_OK)st=sync_file(out);
    if(fclose(out)!=0&&st==LXMF_OK)st=LXMF_ERR_CRYPTO;
    if(st!=LXMF_OK){unlink(tmp);return st;}
    if(rename(tmp,s->path)!=0){unlink(tmp);return LXMF_ERR_CRYPTO;}
    fclose(s->file);
    s->file=fopen(s->path,"r+b");
    if(!s->file)return LXMF_ERR_CRYPTO;
    return scan(s);
}
