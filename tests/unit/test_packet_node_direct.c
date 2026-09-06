/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "reticulum/lxmf_packet_node.h"
#include "reticulum/hal.h"
#include "reticulum/packet.h"
#include "reticulum/proof.h"
#include "reticulum/link.h"
#include "reticulum/announce.h"
#include "reticulum/destination.h"
#include "reticulum/lxmf_router.h"
#include "reticulum/crypto.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>
typedef struct { uint8_t data[680]; size_t length; } record;
typedef struct endpoint endpoint;
typedef struct { uint8_t data[500]; size_t length; uint32_t id; } frame;
struct endpoint {
    record identity, out[4], peers[32];
    frame queue[64]; size_t head,tail;
    uint32_t serial;
    rns_storage_t *storage; rns_interface_t *radio; lxmf_packet_node_t *node;
    endpoint *other;
    bool reject, packet_only; unsigned received; int fail_out_after;
};
static uint64_t now;
static rns_status_t clock_ms(void *c,uint64_t *out) { (void)c; *out=now; return RNS_OK; }
static record *lookup(endpoint *e,const char *key) {
    if(!strcmp(key,"identity")) return &e->identity;
    if(!strncmp(key,"out",3) && key[3]>='0' && key[3]<='3' && !key[4]) return &e->out[key[3]-'0'];
    if(!strncmp(key,"peer",4)) {
        unsigned i=(unsigned)(key[4]-'0')*10U+(unsigned)(key[5]-'0'); assert(i<32 && !key[6]); return &e->peers[i];
    }
    assert(0); return NULL;
}
static rns_status_t read_record(void *c,const char *key,uint8_t *out,size_t cap,size_t *length) {
    record *r=lookup(c,key); if(!r->length) return RNS_ERROR_NOT_FOUND;
    if(cap<r->length) return RNS_ERROR_OVERFLOW;
    memcpy(out,r->data,r->length); *length=r->length; return RNS_OK;
}
static rns_status_t write_record(void *c,const char *key,const uint8_t *in,size_t length) {
    endpoint *e=c;
    if(!strncmp(key,"out",3)) {if(e->fail_out_after==0)return RNS_ERROR_IO;if(e->fail_out_after>0)--e->fail_out_after;}
    record *r=lookup(c,key); assert(length<=sizeof(r->data)); memcpy(r->data,in,length); r->length=length; return RNS_OK;
}
static rns_status_t remove_record(void *c,const char *key) { lookup(c,key)->length=0; return RNS_OK; }
static rns_status_t start(void *c) { (void)c; return RNS_OK; }
static void stop(void *c) { (void)c; }
static rns_status_t poll_radio(void *c,rns_interface_receive_fn fn,void *ctx,size_t budget) {
    (void)c; (void)fn; (void)ctx; (void)budget; return RNS_OK;
}
static rns_status_t enqueue(endpoint *e,const uint8_t *data,size_t length,uint32_t id) {
    assert(e->tail-e->head<64 && length<=500);
    frame *f=&e->queue[e->tail++%64]; memcpy(f->data,data,length); f->length=length; f->id=id; return RNS_OK;
}
static rns_status_t send_radio(void *c,const uint8_t *data,size_t length) { return enqueue(c,data,length,0); }
static rns_status_t tracked(void *c,const uint8_t *data,size_t length,uint32_t *id) {
    endpoint *e=c; *id=++e->serial; return enqueue(e,data,length,*id);
}
static rns_status_t stats(void *c,rns_interface_stats_t *out) {
    (void)c; memset(out,0,sizeof(*out)); out->online=out->outbound=1; out->effective_mtu=500; return RNS_OK;
}
static rns_status_t accept(void *c,const lxmf_message_t *m,lxmf_signature_state_t signature,const uint8_t *raw,size_t length) {
    endpoint *e=c; assert(signature==LXMF_SIGNATURE_VERIFIED && m->content.len==5);
    assert(length>5 && (e->packet_only?raw[0]!=0xff:raw[0]==0xff)); return e->reject?RNS_ERROR_IO:RNS_OK;
}
static void incoming(void *c,const lxmf_message_t *m) {
    endpoint *e=c; assert(m->content.len==5 && !memcmp(m->content.data,"hello",5)); ++e->received;
}
static void boot(endpoint *e) {
    assert(lxmf_packet_node_create(e->storage,e->radio,incoming,e,&e->node)==RNS_OK);
    assert(lxmf_packet_node_open_peers(e->node,e->storage,NULL,NULL)==RNS_OK);
    if(!e->packet_only)assert(lxmf_packet_node_enable_links(e->node)==RNS_OK);
    assert(lxmf_packet_node_open_outbox(e->node,e->storage)==RNS_OK);
    lxmf_packet_node_set_accept(e->node,accept,e);
}
static void init(endpoint *e) {
    e->fail_out_after=-1;
    static const rns_storage_ops_t storage={.read=read_record,.write_atomic=write_record,.remove=remove_record};
    static const rns_interface_ops_t radio={.start=start,.stop=stop,.poll=poll_radio,.send=send_radio,.send_with_id=tracked,.get_stats=stats};
    assert(rns_storage_create(&storage,e,&e->storage)==RNS_OK);
    assert(rns_interface_create(&radio,e,&e->radio)==RNS_OK);
    assert(rns_interface_start(e->radio)==RNS_OK); boot(e);
}
static bool drain(endpoint *e) {
    bool any=false;
    while(e->head<e->tail) {
        frame f=e->queue[e->head++%64]; any=true;
        if(f.id) lxmf_packet_node_tx_complete(e->node,f.id,RNS_OK,now);
        (void)lxmf_packet_node_receive(e->other->node,f.data,f.length);
    }
    return any;
}
static void run(endpoint *a,endpoint *b) {
    for(unsigned i=0;i<30;++i) {
        lxmf_packet_node_poll(a->node,now); lxmf_packet_node_poll(b->node,now);
        bool one=drain(a),two=drain(b); ++now;
        if(!one && !two) break;
    }
}
static void announce_capabilities(endpoint *from,endpoint *to,uint64_t timestamp,bool ratchet,uint8_t cost) {
    rns_identity identity;assert(rns_identity_from_private(&identity,from->identity.data+1));
    uint8_t name[10],public_ratchet[32],app[192],body[480],raw[500],random[5]={0};size_t app_length,raw_length;
    const char *aspects[]={"delivery"};assert(rns_destination_name_hash("lxmf",aspects,1,name));
    assert(rns_x25519_public_from_private(from->identity.data+65,public_ratchet));
    lxmf_announce_data_t metadata={.has_stamp_cost=cost!=0,.stamp_cost=cost};
    assert(lxmf_announce_encode(&metadata,app,sizeof app,&app_length)==LXMF_OK);
    rns_packet packet={.packet_type=1,.data=body};memcpy(packet.destination_hash,lxmf_packet_node_address(from->node),16);
    assert(rns_announce_build(&identity,packet.destination_hash,name,random,timestamp,ratchet?public_ratchet:NULL,
        app,app_length,body,sizeof body,&packet.data_length,&packet.context_flag));
    assert(rns_packet_encode(&packet,raw,sizeof raw,&raw_length));assert(lxmf_packet_node_receive(to->node,raw,raw_length)==RNS_OK);
}
int main(void) {
    rns_platform_ops_t platform=*rns_platform_current(); platform.monotonic_ms=clock_ms;
    assert(rns_platform_install(&platform)==RNS_OK);
    endpoint *a=calloc(1,sizeof(*a)),*b=calloc(1,sizeof(*b)); assert(a && b);
    init(a); init(b); a->other=b; b->other=a;
    assert(lxmf_packet_node_announce(a->node,1000)==RNS_OK);
    uint8_t address[16],id[32]; memcpy(address,lxmf_packet_node_address(b->node),16);
    assert(lxmf_packet_node_request_peer(a->node,address,now)==RNS_OK);
    assert(lxmf_packet_node_request_peer(a->node,address,now)==RNS_ERROR_INVALID_STATE);
    run(a,b);
    lxmf_packet_peer_info peer;
    assert(lxmf_packet_node_peer_info(a->node,address,&peer) && peer.delivery);
    assert(lxmf_packet_node_send(a->node,address,(const uint8_t *)"hello",5,1001,id)==RNS_OK);
    assert(a->out[0].data[0]==3);
    lxmf_packet_node_poll(a->node,now);
    now+=200000; lxmf_packet_node_poll(a->node,now);
    lxmf_packet_outgoing out;
    assert(lxmf_packet_node_outgoing(a->node,0,&out) && out.state==LXMF_PACKET_QUEUED && out.attempts==1);
    b->reject=true; run(a,b);
    assert(lxmf_packet_node_outgoing(a->node,0,&out) && out.direct && out.state==LXMF_PACKET_AWAITING_PROOF && !b->received);
    b->reject=false; now+=120001; run(a,b); now+=10001; run(a,b);
    assert(lxmf_packet_node_outgoing(a->node,0,&out) && out.direct && out.state==LXMF_PACKET_DELIVERED && out.attempts==2 && out.durable);
    assert(b->received==1); assert(lxmf_packet_node_release(a->node,0)==RNS_OK);
    lxmf_packet_node_destroy(a->node); lxmf_packet_node_destroy(b->node); now=0;
    boot(a); boot(b);
    assert(lxmf_packet_node_send(a->node,address,(const uint8_t *)"hello",5,1002,id)==RNS_OK);
    run(a,b);
    assert(lxmf_packet_node_outgoing(a->node,0,&out) && out.state==LXMF_PACKET_DELIVERED);
    assert(b->received==2);
    /* A legacy v2 envelope survives another restart and is not mistaken for
     * an opportunistic encrypted packet or a previously completed send. */
    assert(lxmf_packet_node_release(a->node,0)==RNS_OK);
    assert(lxmf_packet_node_send(a->node,address,(const uint8_t *)"hello",5,1003,id)==RNS_OK);
    a->out[0].data[0]=2;a->out[0].data[119]=1;
    lxmf_packet_node_destroy(a->node); boot(a); run(a,b);
    assert(lxmf_packet_node_outgoing(a->node,0,&out) && out.state==LXMF_PACKET_DELIVERED);
    assert(b->received==3);
    assert(lxmf_packet_node_release(a->node,0)==RNS_OK);
    assert(lxmf_packet_node_send(a->node,address,(const uint8_t *)"hello",5,1004,id)==RNS_OK);
    lxmf_packet_node_poll(a->node,now);
    assert(lxmf_packet_node_outgoing(a->node,0,&out) && out.state==LXMF_PACKET_TRANSMITTING);
    frame *queued=&a->queue[(a->tail-1U)%64];
    uint8_t hash[32],proof_data[96],proof_raw[500]; size_t proof_length;
    rns_identity remote;
    assert(rns_identity_from_private(&remote,b->identity.data+1));
    assert(rns_packet_hash(queued->data,queued->length,hash));
    assert(rns_proof_generate_explicit(&remote,hash,proof_data));
    rns_packet proof={.packet_type=3,.data=proof_data,.data_length=sizeof(proof_data)};
    memcpy(proof.destination_hash,hash,16);
    assert(rns_packet_encode(&proof,proof_raw,sizeof(proof_raw),&proof_length));
    assert(lxmf_packet_node_receive(a->node,proof_raw,proof_length)==RNS_OK);
    assert(lxmf_packet_node_outgoing(a->node,0,&out) && out.state==LXMF_PACKET_TRANSMITTING);
    run(a,b);
    assert(lxmf_packet_node_outgoing(a->node,0,&out) && out.state==LXMF_PACKET_DELIVERED);
    assert(b->received==4);
    assert(lxmf_packet_node_release(a->node,0)==RNS_OK);
    lxmf_packet_node_destroy(a->node);lxmf_packet_node_destroy(b->node);
    b->packet_only=true;boot(a);boot(b);
    /* No direct handshake support: timed out LR may fall back, but only
     * after the exact alternative packet is durable. */
    assert(lxmf_packet_node_send(a->node,address,(const uint8_t *)"hello",5,1005,id)==RNS_OK);
    lxmf_packet_node_poll(a->node,now);assert(drain(a));
    now+=120001;lxmf_packet_node_poll(a->node,now);
    assert(lxmf_packet_node_outgoing(a->node,0,&out)&&out.direct&&out.attempts==1&&out.state==LXMF_PACKET_QUEUED);
    a->fail_out_after=0;now+=5001;lxmf_packet_node_poll(a->node,now);
    assert(lxmf_packet_node_outgoing(a->node,0,&out)&&!out.direct&&!out.durable&&out.attempts==1);
    assert(a->head==a->tail&&a->out[0].data[0]==3);
    a->fail_out_after=-1;lxmf_packet_node_poll_ready(a->node,now,0);
    assert(a->out[0].data[0]==1);
    lxmf_packet_node_destroy(a->node);boot(a);
    lxmf_packet_node_poll(a->node,now);frame early=a->queue[a->head++%64];
    assert(lxmf_packet_node_receive(b->node,early.data,early.length)==RNS_OK);assert(drain(b));
    assert(lxmf_packet_node_outgoing(a->node,0,&out)&&out.state==LXMF_PACKET_TRANSMITTING);
    lxmf_packet_node_tx_complete(a->node,early.id,RNS_ERROR_IO,now);
    assert(lxmf_packet_node_outgoing(a->node,0,&out)&&out.state==LXMF_PACKET_QUEUED);
    now+=10001;lxmf_packet_node_poll(a->node,now);early=a->queue[a->head++%64];
    assert(lxmf_packet_node_receive(b->node,early.data,early.length)==RNS_OK);assert(drain(b));
    assert(lxmf_packet_node_outgoing(a->node,0,&out)&&out.state==LXMF_PACKET_TRANSMITTING);
    lxmf_packet_node_tx_complete(a->node,early.id,RNS_OK,now);run(a,b);
    assert(lxmf_packet_node_outgoing(a->node,0,&out)&&!out.direct&&out.state==LXMF_PACKET_DELIVERED&&out.attempts==3&&!memcmp(id,out.id,32));
    assert(b->received==5);assert(lxmf_packet_node_release(a->node,0)==RNS_OK);
    /* Cancellation never converts or transmits an alternative. */
    assert(lxmf_packet_node_send(a->node,address,(const uint8_t *)"hello",5,1006,id)==RNS_OK);
    lxmf_packet_node_poll(a->node,now);assert(drain(a));now+=120001;lxmf_packet_node_poll(a->node,now);
    assert(lxmf_packet_node_cancel(a->node,id)==RNS_OK);now+=5001;lxmf_packet_node_poll(a->node,now);
    assert(lxmf_packet_node_outgoing(a->node,0,&out)&&out.direct&&out.state==LXMF_PACKET_CANCELLED);
    assert(a->head==a->tail);assert(lxmf_packet_node_release(a->node,0)==RNS_OK);
    /* Invalid LR proof permanently blocks downgrade, including an interrupted
     * journal write followed by reboot. */
    assert(lxmf_packet_node_send(a->node,address,(const uint8_t *)"hello",5,1007,id)==RNS_OK);
    lxmf_packet_node_poll(a->node,now);frame request=a->queue[a->head%64];assert(drain(a));
    uint8_t link_id[16],bad[99]={0};assert(rns_link_id_from_request_packet(request.data,request.length,link_id));
    rns_packet invalid={.packet_type=3,.destination_type=3,.context=0xff,.data=bad,.data_length=sizeof bad};
    memcpy(invalid.destination_hash,link_id,16);assert(rns_packet_encode(&invalid,proof_raw,sizeof proof_raw,&proof_length));
    assert(lxmf_packet_node_receive(a->node,proof_raw,proof_length)==RNS_ERROR_CRYPTO);
    a->fail_out_after=0;lxmf_packet_node_poll(a->node,now);
    lxmf_packet_node_destroy(a->node);a->fail_out_after=-1;boot(a);
    lxmf_packet_node_poll(a->node,now);assert(drain(a));now+=120001;lxmf_packet_node_poll(a->node,now);
    now+=10001;lxmf_packet_node_poll(a->node,now);
    assert(lxmf_packet_node_outgoing(a->node,0,&out)&&out.direct&&out.attempts==3);
    assert(drain(a));now+=120001;lxmf_packet_node_poll(a->node,now);
    assert(lxmf_packet_node_outgoing(a->node,0,&out)&&out.direct&&out.state==LXMF_PACKET_FAILED);
    assert(lxmf_packet_node_release(a->node,0)==RNS_OK);
    announce_capabilities(b,a,2000,false,0);
    assert(lxmf_packet_node_send(a->node,address,(const uint8_t *)"hello",5,2001,id)==RNS_OK);
    lxmf_packet_node_poll(a->node,now);assert(drain(a));now+=120001;lxmf_packet_node_poll(a->node,now);
    now+=5001;lxmf_packet_node_poll(a->node,now);
    assert(lxmf_packet_node_outgoing(a->node,0,&out)&&out.direct&&out.attempts==2);
    uint8_t valid_flags=a->out[0].data[119];a->out[0].data[119]|=8u;
    lxmf_packet_node_destroy(a->node);
    assert(lxmf_packet_node_create(a->storage,a->radio,incoming,a,&a->node)==RNS_OK);
    assert(lxmf_packet_node_open_outbox(a->node,a->storage)==RNS_ERROR_PROTOCOL);
    lxmf_packet_node_destroy(a->node);a->out[0].data[119]=valid_flags;boot(a);
    assert(drain(a));assert(lxmf_packet_node_cancel(a->node,id)==RNS_OK);lxmf_packet_node_poll(a->node,now);
    assert(lxmf_packet_node_release(a->node,0)==RNS_OK);
    /* A newer verified stamp requirement prevents fallback even if the
     * original queued message was eligible when created. */
    lxmf_packet_node_destroy(a->node);boot(a);
    announce_capabilities(b,a,2002,true,0);
    assert(lxmf_packet_node_send(a->node,address,(const uint8_t *)"hello",5,2003,id)==RNS_OK);
    announce_capabilities(b,a,2004,true,1);
    lxmf_packet_node_poll(a->node,now);assert(drain(a));now+=120001;lxmf_packet_node_poll(a->node,now);
    now+=5001;lxmf_packet_node_poll(a->node,now);
    assert(lxmf_packet_node_outgoing(a->node,0,&out)&&out.direct&&out.attempts==2);
    lxmf_packet_node_destroy(a->node); lxmf_packet_node_destroy(b->node);
    rns_interface_destroy(a->radio); rns_interface_destroy(b->radio);
    rns_storage_destroy(a->storage); rns_storage_destroy(b->storage); free(a); free(b);
    return 0;
}
