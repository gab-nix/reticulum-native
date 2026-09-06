#include "reticulum/embedded_link.h"
#include "reticulum/packet.h"
#include "reticulum/crypto.h"
#include "reticulum/hal.h"
#include <stdbool.h>
#include <string.h>

#define WAIT_MS UINT64_C(120000)
#define QUEUE_MS UINT64_C(3600000)
#define KEEP_MS UINT64_C(360000)
#define TX_COUNT 16u
enum { TX_CONTROL, TX_REQUEST, TX_PROOF, TX_RTT, TX_DATA };
typedef struct {
    rns_link link;
    bool used, identified, ready;
    uint8_t destination[16];
    uint64_t deadline, last_rx, last_tx;
} slot;
typedef struct {
    bool used, sent, proved;
    unsigned slot, kind;
    uint32_t id;
    uint64_t deadline;
    uint8_t hash[32];
} tx;
struct rns_embedded_link_manager {
    const rns_platform_ops_t *platform;
    const rns_identity *identity;
    uint8_t destination[16];
    rns_interface_t *interface;
    rns_embedded_link_callbacks callbacks;
    void *context;
    uint64_t now;
    slot slots[RNS_EMBEDDED_LINK_CAPACITY];
    tx transmissions[TX_COUNT];
};
static double clock_now(void *p) { return (double)((rns_embedded_link_manager *)p)->now / 1000.0; }
static slot *find(const rns_embedded_link_manager *m, const uint8_t id[16]) {
    for (size_t i=0;i<RNS_EMBEDDED_LINK_CAPACITY;i++)
        if (m->slots[i].used && memcmp(m->slots[i].link.link_id,id,16)==0)
            return (slot *)&m->slots[i];
    return NULL;
}
static void notify(rns_embedded_link_manager *m,slot *s,rns_status_t status) {
    if(m->callbacks.state) m->callbacks.state(m->context,s->link.link_id,s->link.state,status);
}
static void finish(rns_embedded_link_manager *m,slot *s,rns_status_t status) {
    unsigned index=(unsigned)(s-m->slots);
    s->link.state=RNS_LINK_CLOSED; s->ready=false;
    for(size_t i=0;i<TX_COUNT;i++) if(m->transmissions[i].used && m->transmissions[i].slot==index) {
        tx *t=&m->transmissions[i];
        if(t->kind==TX_DATA && !t->sent && m->callbacks.transmission)
            m->callbacks.transmission(m->context,s->link.link_id,t->hash,status==RNS_OK?RNS_ERROR_INVALID_STATE:status);
        memset(t,0,sizeof *t);
    }
    notify(m,s,status);
    memset(s,0,sizeof *s);
}
static rns_status_t wire(rns_embedded_link_manager *m,slot *s,uint8_t type,uint8_t context,
    const uint8_t *data,size_t length,unsigned kind,uint8_t hash[32]) {
    tx *t=NULL;
    for(size_t i=0;i<TX_COUNT;i++) if(!m->transmissions[i].used) {t=&m->transmissions[i];break;}
    if(!t) return RNS_ERROR_OVERFLOW;
    rns_packet p={0}; uint8_t raw[RNS_MTU];size_t n;
    p.destination_type=kind==TX_REQUEST?0u:3u;p.packet_type=type;p.context=context;
    memcpy(p.destination_hash,kind==TX_REQUEST?s->destination:s->link.link_id,16);
    p.data=data;p.data_length=length;
    if(!rns_packet_encode(&p,raw,sizeof raw,&n) || !rns_packet_hash(raw,n,t->hash)) return RNS_ERROR_OVERFLOW;
    if(n>s->link.mtu)return RNS_ERROR_OVERFLOW;
    if(kind==TX_REQUEST && !rns_link_initiator_set_request_packet(&s->link,raw,n)) return RNS_ERROR_PROTOCOL;
    rns_status_t status=rns_interface_send_with_id(m->interface,raw,n,&t->id);
    if(status!=RNS_OK) return status;
    t->used=true;t->sent=false;t->proved=false;t->kind=kind;t->slot=(unsigned)(s-m->slots);t->deadline=m->now+QUEUE_MS;
    if(hash) memcpy(hash,t->hash,32);
    return RNS_OK;
}
static rns_status_t encrypted(rns_embedded_link_manager *m,slot *s,uint8_t context,
    const uint8_t *data,size_t length,unsigned kind,uint8_t hash[32]) {
    uint8_t token[RNS_MTU];size_t n;
    if(!rns_link_encrypt(&s->link,data,length,token,sizeof token,&n)) return RNS_ERROR_OVERFLOW;
    return wire(m,s,0,context,token,n,kind,hash);
}
rns_status_t rns_embedded_link_create(const rns_identity *identity,const uint8_t destination[16],
    rns_interface_t *interface,const rns_embedded_link_callbacks *callbacks,void *context,rns_embedded_link_manager **out) {
    if(!identity||!identity->has_private||!destination||!interface||!out) return RNS_ERROR_INVALID_ARGUMENT;
    const rns_platform_ops_t *platform=rns_platform_current();
    *out=NULL;if(!platform)return RNS_ERROR_INVALID_STATE;
    *out=platform->allocate(platform->context,sizeof **out);if(!*out)return RNS_ERROR_NO_MEMORY;
    memset(*out,0,sizeof **out);(*out)->platform=platform;
    (*out)->identity=identity;(*out)->interface=interface;(*out)->context=context;
    memcpy((*out)->destination,destination,16);if(callbacks)(*out)->callbacks=*callbacks;return RNS_OK;
}
void rns_embedded_link_destroy(rns_embedded_link_manager *m) {
    if(m){const rns_platform_ops_t *platform=m->platform;memset(m,0,sizeof *m);platform->deallocate(platform->context,m);}
}
rns_status_t rns_embedded_link_connect(rns_embedded_link_manager *m,const uint8_t destination[16],
    const rns_identity *identity,uint64_t now,uint8_t id[16]) {
    if(!m||!destination||!identity||!id)return RNS_ERROR_INVALID_ARGUMENT;
    m->now=now;slot *s=NULL;
    for(size_t i=0;i<RNS_EMBEDDED_LINK_CAPACITY;i++) {
        if(m->slots[i].used && m->slots[i].link.role==RNS_LINK_INITIATOR && memcmp(m->slots[i].destination,destination,16)==0) {
            memcpy(id,m->slots[i].link.link_id,16);return RNS_OK;
        }
        if(!m->slots[i].used && !s)s=&m->slots[i];
    }
    if(!s)return RNS_ERROR_OVERFLOW;
    rns_identity public_identity;uint8_t public_bytes[64];
    rns_identity_export_public(identity,public_bytes);
    if(!rns_identity_from_public(&public_identity,public_bytes)||
       !rns_link_initiator_init(&s->link,&public_identity,RNS_MTU,120,clock_now,m))return RNS_ERROR_CRYPTO;
    s->used=true;s->identified=true;s->deadline=now+QUEUE_MS;memcpy(s->destination,destination,16);
    uint8_t payload[67];rns_link_build_request_payload(&s->link,payload);
    rns_status_t status=wire(m,s,2,0,payload,sizeof payload,TX_REQUEST,NULL);
    if(status!=RNS_OK){memset(s,0,sizeof *s);return status;}
    memcpy(id,s->link.link_id,16);notify(m,s,RNS_OK);return RNS_OK;
}
rns_status_t rns_embedded_link_state(const rns_embedded_link_manager *m,const uint8_t id[16],rns_link_state *state) {
    if(!m||!id||!state)return RNS_ERROR_INVALID_ARGUMENT;
    slot *s=find(m,id);if(!s)return RNS_ERROR_NOT_FOUND;
    *state=s->link.state==RNS_LINK_ACTIVE&&!s->ready?RNS_LINK_HANDSHAKE:s->link.state;return RNS_OK;
}
rns_status_t rns_embedded_link_authenticated_peer(const rns_embedded_link_manager *m,const uint8_t id[16],rns_identity *identity) {
    if(!m||!id||!identity)return RNS_ERROR_INVALID_ARGUMENT;
    slot *s=find(m,id);if(!s||!s->identified||!s->ready)return RNS_ERROR_NOT_FOUND;
    *identity=s->link.remote_identity;return RNS_OK;
}
rns_status_t rns_embedded_link_send(rns_embedded_link_manager *m,const uint8_t id[16],const uint8_t *data,
    size_t length,uint64_t now,uint8_t hash[32]) {
    if(!m||!id||(!data&&length)||!hash)return RNS_ERROR_INVALID_ARGUMENT;
    m->now=now;slot *s=find(m,id);if(!s||!s->ready)return RNS_ERROR_INVALID_STATE;
    return encrypted(m,s,0,data,length,TX_DATA,hash);
}
rns_status_t rns_embedded_link_close(rns_embedded_link_manager *m,const uint8_t id[16],uint64_t now) {
    if(!m||!id)return RNS_ERROR_INVALID_ARGUMENT;
    m->now=now;slot *s=find(m,id);if(!s)return RNS_ERROR_NOT_FOUND;
    rns_status_t status=s->ready?encrypted(m,s,0xfc,s->link.link_id,16,TX_CONTROL,NULL):RNS_OK;
    finish(m,s,RNS_OK);return status;
}
void rns_embedded_link_tx_complete(rns_embedded_link_manager *m,uint32_t id,rns_status_t status,uint64_t now) {
    if(!m)return;
    m->now=now;
    for(size_t i=0;i<TX_COUNT;i++) {
        tx *t=&m->transmissions[i];if(!t->used||t->sent||t->id!=id)continue;
        slot *s=&m->slots[t->slot];s->last_tx=now;
        if(t->kind==TX_DATA && m->callbacks.transmission)m->callbacks.transmission(m->context,s->link.link_id,t->hash,status);
        if(status!=RNS_OK){t->used=false;finish(m,s,status);return;}
        if(t->kind==TX_REQUEST||t->kind==TX_PROOF){s->deadline=now+WAIT_MS;s->link.request_time=clock_now(m);}
        if(t->kind==TX_RTT){
            uint8_t identify[128], signed_data[80];
            s->ready=true;s->last_rx=now;
            rns_identity_export_public(m->identity,identify);
            memcpy(signed_data,s->link.link_id,16);memcpy(signed_data+16,identify,64);
            if(rns_identity_sign(m->identity,signed_data,sizeof signed_data,identify+64))
                (void)encrypted(m,s,0xfb,identify,sizeof identify,TX_CONTROL,NULL);
            notify(m,s,RNS_OK);
        }
        if(t->kind==TX_DATA){
            t->sent=true;t->deadline=now+WAIT_MS;s->deadline=t->deadline;
            if(t->proved){t->used=false;if(m->callbacks.proof)m->callbacks.proof(m->context,s->link.link_id,t->hash);}
        }else t->used=false;
        return;
    }
}
void rns_embedded_link_poll(rns_embedded_link_manager *m,uint64_t now) {
    if(!m)return;
    m->now=now;
    for(size_t i=0;i<TX_COUNT;i++)if(m->transmissions[i].used&&now>=m->transmissions[i].deadline) {
        slot *s=&m->slots[m->transmissions[i].slot];finish(m,s,RNS_ERROR_TIMEOUT);
    }
    for(size_t i=0;i<RNS_EMBEDDED_LINK_CAPACITY;i++) {
        slot *s=&m->slots[i];if(!s->used)continue;
        if(!s->ready){if(now>=s->deadline)finish(m,s,RNS_ERROR_TIMEOUT);continue;}
        bool queued=false;
        for(size_t j=0;j<TX_COUNT;j++)if(m->transmissions[j].used&&!m->transmissions[j].sent&&m->transmissions[j].slot==i)queued=true;
        if(!queued&&now-s->last_rx>=KEEP_MS+WAIT_MS&&now>=s->deadline){finish(m,s,RNS_ERROR_TIMEOUT);continue;}
        if(s->link.role==RNS_LINK_INITIATOR && now-s->last_tx>=KEEP_MS) {
            bool pending=false;for(size_t j=0;j<TX_COUNT;j++)if(m->transmissions[j].used&&m->transmissions[j].slot==i)pending=true;
            uint8_t keep=0xff;if(!pending)(void)wire(m,s,0,0xfa,&keep,1,TX_CONTROL,NULL);
        }
    }
}
rns_status_t rns_embedded_link_receive(rns_embedded_link_manager *m,const uint8_t *raw,size_t length,uint64_t now) {
    if(!m||!raw)return RNS_ERROR_INVALID_ARGUMENT;
    m->now=now;rns_packet p;if(!rns_packet_decode(&p,raw,length))return RNS_ERROR_PROTOCOL;
    if(p.packet_type==2 && p.destination_type==0 && memcmp(p.destination_hash,m->destination,16)==0) {
        uint8_t id[16];if(!rns_link_id_from_request_packet(raw,length,id))return RNS_ERROR_PROTOCOL;
        if(find(m,id))return RNS_OK;
        slot *s=NULL;for(size_t i=0;i<RNS_EMBEDDED_LINK_CAPACITY;i++)if(!m->slots[i].used){s=&m->slots[i];break;}
        if(!s)return RNS_ERROR_OVERFLOW;
        if(!rns_link_responder_accept(&s->link,m->identity,raw,length,120,clock_now,m))return RNS_ERROR_CRYPTO;
        s->used=true;s->deadline=now+QUEUE_MS;
        uint8_t proof[99];if(!rns_link_build_proof(&s->link,proof)){memset(s,0,sizeof *s);return RNS_ERROR_CRYPTO;}
        rns_status_t status=wire(m,s,3,0xff,proof,sizeof proof,TX_PROOF,NULL);
        if(status!=RNS_OK){finish(m,s,status);return status;}notify(m,s,RNS_OK);return RNS_OK;
    }
    slot *s=find(m,p.destination_hash);if(!s)return RNS_ERROR_NOT_FOUND;
    if(p.destination_type!=3)return RNS_ERROR_PROTOCOL;
    if(s->link.state==RNS_LINK_PENDING && p.packet_type==3 && p.context==0xff) {
        if(!rns_link_initiator_accept_proof(&s->link,p.data,p.data_length))return RNS_ERROR_CRYPTO;
        uint8_t rtt[128];size_t n;
        if(!rns_link_build_rtt_confirm(&s->link,rtt,sizeof rtt,&n))return RNS_ERROR_CRYPTO;
        s->deadline=now+QUEUE_MS;
        rns_status_t status=wire(m,s,0,0xfe,rtt,n,TX_RTT,NULL);
        if(status!=RNS_OK)finish(m,s,status);
        return status;
    }
    if(s->link.state==RNS_LINK_HANDSHAKE && p.packet_type==0 && p.context==0xfe) {
        if(!rns_link_responder_accept_rtt(&s->link,p.data,p.data_length)){finish(m,s,RNS_ERROR_CRYPTO);return RNS_ERROR_CRYPTO;}
        s->ready=true;s->last_rx=now;notify(m,s,RNS_OK);return RNS_OK;
    }
    if(!s->ready)return RNS_ERROR_INVALID_STATE;
    if(p.packet_type==3 && p.context==0 && (p.data_length==64||p.data_length==96)) {
        for(size_t i=0;i<TX_COUNT;i++) {
            tx *t=&m->transmissions[i];if(!t->used||t->kind!=TX_DATA||t->slot!=(unsigned)(s-m->slots))continue;
            if(p.data_length==96 && memcmp(t->hash,p.data,32)!=0)continue;
            if(!rns_ed25519_verify(s->link.peer_signing_public,t->hash,32,p.data+(p.data_length==96?32:0)))continue;
            t->proved=true;s->last_rx=now;
            if(t->sent){t->used=false;if(m->callbacks.proof)m->callbacks.proof(m->context,s->link.link_id,t->hash);}
            return RNS_OK;
        }
        return RNS_ERROR_CRYPTO;
    }
    if(p.packet_type!=0)return RNS_ERROR_PROTOCOL;
    if(p.context==0xfa) {
        uint8_t expected=s->link.role==RNS_LINK_INITIATOR?0xfe:0xff;
        if(p.data_length!=1||p.data[0]!=expected)return RNS_ERROR_PROTOCOL;
        /* Raw keepalives do not authenticate a peer; only postpone idle
         * expiration, never establish identity or acknowledge application data. */
        s->last_rx=now;
        if(s->link.role==RNS_LINK_RESPONDER){uint8_t response=0xfe;return wire(m,s,0,0xfa,&response,1,TX_CONTROL,NULL);}
        return RNS_OK;
    }
    if(p.context>=1&&p.context<=8)return RNS_ERROR_UNSUPPORTED;
    uint8_t plain[RNS_MTU];size_t n;
    if(!rns_link_decrypt(&s->link,p.data,p.data_length,plain,sizeof plain,&n))return RNS_ERROR_CRYPTO;
    s->last_rx=now;
    if(p.context==0xfc) {
        if(n!=16||memcmp(plain,s->link.link_id,16)!=0)return RNS_ERROR_PROTOCOL;
        finish(m,s,RNS_OK);return RNS_OK;
    }
    if(p.context==0xfb) {
        rns_identity identity;uint8_t signed_data[80];
        if(s->link.role!=RNS_LINK_RESPONDER||n!=128||!rns_identity_from_public(&identity,plain))return RNS_ERROR_PROTOCOL;
        memcpy(signed_data,s->link.link_id,16);memcpy(signed_data+16,plain,64);
        if(!rns_identity_verify(&identity,signed_data,sizeof signed_data,plain+64))return RNS_ERROR_CRYPTO;
        if(s->identified && memcmp(identity.signing_public,s->link.remote_identity.signing_public,32)!=0)return RNS_ERROR_CRYPTO;
        s->link.remote_identity=identity;s->identified=true;notify(m,s,RNS_OK);return RNS_OK;
    }
    if(p.context!=0)return RNS_ERROR_UNSUPPORTED;
    if(!m->callbacks.data)return RNS_ERROR_UNSUPPORTED;
    rns_status_t accepted=m->callbacks.data(m->context,s->link.link_id,plain,n);
    if(accepted!=RNS_OK)return accepted;
    uint8_t proof[96];
    if(!rns_packet_hash(raw,length,proof)||!rns_ed25519_sign(s->link.signing_private,proof,32,proof+32))return RNS_ERROR_CRYPTO;
    return wire(m,s,3,0,proof,sizeof proof,TX_CONTROL,NULL);
}
