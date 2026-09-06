/* SPDX-License-Identifier: GPL-3.0-or-later */
#define _POSIX_C_SOURCE 200809L
#include "reticulum/lxmf_packet_node.h"
#include "reticulum/hal.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
typedef struct {uint8_t data[4096];size_t length;} record;
typedef struct {uint8_t data[500];size_t length;uint32_t id;} frame;
typedef struct {
    record identity,out[4],peers[32];
    frame frames[64];size_t head,tail;
    uint32_t serial;int socket;struct sockaddr_in remote;
    rns_storage_t *storage;rns_interface_t *interface;lxmf_packet_node_t *node;
    unsigned received;bool failed;
} endpoint;
static uint64_t now_ms(void){uint64_t now=0;(void)rns_hal_monotonic_ms(&now);return now;}
static record *lookup(endpoint *e,const char *key) {
    if(!strcmp(key,"identity"))return &e->identity;
    if(strlen(key)==4&&!strncmp(key,"out",3)&&key[3]>='0'&&key[3]<='3')return &e->out[key[3]-'0'];
    if(strlen(key)==6&&!strncmp(key,"peer",4)&&key[4]>='0'&&key[4]<='3'&&key[5]>='0'&&key[5]<='9') {
        unsigned i=(unsigned)(key[4]-'0')*10u+(unsigned)(key[5]-'0');if(i<32)return &e->peers[i];
    }
    return NULL;
}
static rns_status_t read_record(void *ctx,const char *key,uint8_t *out,size_t cap,size_t *length) {
    record *r=lookup(ctx,key);if(!r)return RNS_ERROR_INVALID_ARGUMENT;if(!r->length)return RNS_ERROR_NOT_FOUND;
    if(r->length>cap)return RNS_ERROR_OVERFLOW;
    memcpy(out,r->data,r->length);*length=r->length;return RNS_OK;
}
static rns_status_t write_record(void *ctx,const char *key,const uint8_t *data,size_t length) {
    record *r=lookup(ctx,key);if(!r||length>sizeof r->data)return RNS_ERROR_OVERFLOW;
    memcpy(r->data,data,length);r->length=length;return RNS_OK;
}
static rns_status_t remove_record(void *ctx,const char *key){record *r=lookup(ctx,key);if(!r)return RNS_ERROR_INVALID_ARGUMENT;r->length=0;return RNS_OK;}
static rns_status_t start(void *ctx){(void)ctx;return RNS_OK;}
static void stop(void *ctx){(void)ctx;}
static rns_status_t enqueue(endpoint *e,const uint8_t *data,size_t length,uint32_t id) {
    if(length>500||e->tail-e->head>=64)return RNS_ERROR_OVERFLOW;
    frame *f=&e->frames[e->tail++%64];memcpy(f->data,data,length);f->length=length;f->id=id;return RNS_OK;
}
static rns_status_t send_frame(void *ctx,const uint8_t *data,size_t length){return enqueue(ctx,data,length,0);}
static rns_status_t tracked(void *ctx,const uint8_t *data,size_t length,uint32_t *id){endpoint *e=ctx;*id=++e->serial;return enqueue(e,data,length,*id);}
static rns_status_t stats(void *ctx,rns_interface_stats_t *out){(void)ctx;memset(out,0,sizeof *out);out->online=out->outbound=1;out->effective_mtu=500;return RNS_OK;}
static rns_status_t poll_interface(void *ctx,rns_interface_receive_fn receive,void *user,size_t budget) {
    endpoint *e=ctx;
    while(e->head<e->tail) {
        frame *f=&e->frames[e->head++%64];ssize_t n=sendto(e->socket,f->data,f->length,0,(const struct sockaddr *)&e->remote,sizeof e->remote);
        rns_status_t status=n==(ssize_t)f->length?RNS_OK:RNS_ERROR_IO;
        if(f->id)lxmf_packet_node_tx_complete(e->node,f->id,status,now_ms());
        if(status!=RNS_OK)return status;
    }
    for(size_t i=0;i<budget;i++) {
        uint8_t data[501];ssize_t n=recv(e->socket,data,sizeof data,0);
        if(n<0){if(errno==EAGAIN||errno==EWOULDBLOCK)return RNS_OK;return RNS_ERROR_IO;}
        if(n>0)(void)receive(user,data,(size_t)n);
    }
    return RNS_OK;
}
static rns_status_t receive(void *ctx,const uint8_t *raw,size_t length){
    if(length&&(raw[0]&3u)==2u)puts("{\"event\":\"inbound_link_request\"}");
    return lxmf_packet_node_receive(((endpoint *)ctx)->node,raw,length);
}
static rns_status_t accept_message(void *ctx,const lxmf_message_t *m,lxmf_signature_state_t signature,const uint8_t *raw,size_t length) {
    endpoint *e=ctx;
    if(!length||raw[0]!=0xff||signature!=LXMF_SIGNATURE_VERIFIED||m->content.len!=5||memcmp(m->content.data,"reply",5)) {e->failed=true;return RNS_ERROR_PROTOCOL;}
    return RNS_OK;
}
static void incoming(void *ctx,const lxmf_message_t *m){(void)m;((endpoint *)ctx)->received++;puts("{\"event\":\"received\",\"verified\":true}");}
static bool boot(endpoint *e) {
    if(lxmf_packet_node_create(e->storage,e->interface,incoming,e,&e->node)!=RNS_OK||
       lxmf_packet_node_open_peers(e->node,e->storage,NULL,NULL)!=RNS_OK||
       lxmf_packet_node_enable_links(e->node)!=RNS_OK||
       lxmf_packet_node_open_outbox(e->node,e->storage)!=RNS_OK)return false;
    lxmf_packet_node_set_accept(e->node,accept_message,e);return true;
}
int main(int argc,char **argv) {
    if(argc!=4||strlen(argv[3])!=32)return 2;
    setvbuf(stdout,NULL,_IOLBF,0);endpoint *e=calloc(1,sizeof *e);if(!e)return 2;
    uint8_t peer[16];for(size_t i=0;i<16;i++){unsigned byte;if(sscanf(argv[3]+2*i,"%2x",&byte)!=1)return 2;peer[i]=(uint8_t)byte;}
    e->socket=socket(AF_INET,SOCK_DGRAM,0);if(e->socket<0)return 2;
    struct sockaddr_in local={.sin_family=AF_INET,.sin_port=htons((uint16_t)atoi(argv[1])),.sin_addr={.s_addr=htonl(INADDR_LOOPBACK)}};
    e->remote=(struct sockaddr_in){.sin_family=AF_INET,.sin_port=htons((uint16_t)atoi(argv[2])),.sin_addr={.s_addr=htonl(INADDR_LOOPBACK)}};
    if(bind(e->socket,(const struct sockaddr *)&local,sizeof local)||fcntl(e->socket,F_SETFL,O_NONBLOCK)<0)return 2;
    static const rns_storage_ops_t store={.read=read_record,.write_atomic=write_record,.remove=remove_record};
    static const rns_interface_ops_t radio={.start=start,.stop=stop,.poll=poll_interface,.send=send_frame,.send_with_id=tracked,.get_stats=stats};
    if(rns_storage_create(&store,e,&e->storage)!=RNS_OK||rns_interface_create(&radio,e,&e->interface)!=RNS_OK||rns_interface_start(e->interface)!=RNS_OK||!boot(e))return 2;
    printf("{\"event\":\"ready\",\"destination\":\"");for(size_t i=0;i<16;i++)printf("%02x",lxmf_packet_node_address(e->node)[i]);puts("\"}");
    uint64_t started=now_ms(),announced=0,done=0;unsigned sent=0,delivered=0;bool known=false,rebooted=false;
    while(now_ms()-started<120000&&!e->failed) {
        uint64_t now=now_ms(),wall=0;(void)rns_hal_wallclock_ms(&wall);
        if(!known&&now-announced>=2000){(void)lxmf_packet_node_announce(e->node,wall/1000);announced=now;}
        lxmf_packet_node_poll(e->node,now);
        if(rns_interface_poll(e->interface,receive,e,32)!=RNS_OK){e->failed=true;break;}
        lxmf_packet_peer_info info;
        if(!known&&lxmf_packet_node_peer_info(e->node,peer,&info)&&info.delivery){known=true;puts("{\"event\":\"peer_verified\"}");}
        if(known&&e->received&&sent==0){uint8_t id[32];if(lxmf_packet_node_send(e->node,peer,(const uint8_t *)"hello",5,wall/1000,id)!=RNS_OK){e->failed=true;break;}sent=1;}
        lxmf_packet_outgoing out;
        if(lxmf_packet_node_outgoing(e->node,0,&out)&&out.state==LXMF_PACKET_FAILED){puts("{\"event\":\"failed\"}");e->failed=true;break;}
        if(lxmf_packet_node_outgoing(e->node,0,&out)&&out.state==LXMF_PACKET_DELIVERED&&out.durable) {
            delivered++;printf("{\"event\":\"delivered\",\"count\":%u}\n",delivered);(void)lxmf_packet_node_release(e->node,0);
        }
        if(delivered==1&&e->received&&!rebooted&&e->head==e->tail){
            lxmf_packet_node_destroy(e->node);if(!boot(e)){e->failed=true;break;}rebooted=true;
            if(!lxmf_packet_node_peer_info(e->node,peer,&info)||!info.delivery){e->failed=true;break;}
            puts("{\"event\":\"reboot\",\"retained_peer\":true}");uint8_t id[32];
            if(lxmf_packet_node_send(e->node,peer,(const uint8_t *)"again",5,wall/1000+1,id)!=RNS_OK){e->failed=true;break;}
        }
        if(delivered==2&&e->received){if(!done)done=now;if(now-done>2000)break;}
        struct timespec pause={.tv_nsec=10000000};nanosleep(&pause,NULL);
    }
    bool success=!e->failed&&delivered==2&&e->received==1;
    printf("{\"event\":\"complete\",\"ok\":%s}\n",success?"true":"false");
    lxmf_packet_node_destroy(e->node);rns_interface_destroy(e->interface);rns_storage_destroy(e->storage);close(e->socket);rns_hal_secure_zero(e,sizeof *e);free(e);return success?0:1;
}
