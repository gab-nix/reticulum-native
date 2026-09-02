#include "reticulum/lxmf_router.h"
#include "reticulum/destination.h"
#include <assert.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
typedef struct { const rns_identity *peer; uint8_t packet[500]; size_t length; } send_state;
static const rns_identity *resolve(void *ctx,const uint8_t hash[16]){send_state *s=ctx;uint8_t expected[16];const char *a[]={"delivery"};return rns_destination_hash(s->peer,"lxmf",a,1,expected)&&!memcmp(expected,hash,16)?s->peer:NULL;}
static lxmf_status_t send_packet(void *ctx,const uint8_t *p,size_t n){send_state *s=ctx;assert(n<=sizeof s->packet);memcpy(s->packet,p,n);s->length=n;return LXMF_OK;}
int main(void){char path[]="/tmp/lxmf-router-XXXXXX";int fd=mkstemp(path);assert(fd>=0);close(fd);unlink(path);rns_identity alice,bob;uint8_t ka[64],kb[64],source[16],destination[16],id[32],body[64];for(size_t i=0;i<64;i++){ka[i]=(uint8_t)(i+1);kb[i]=(uint8_t)(i+65);}assert(rns_identity_from_private(&alice,ka)&&rns_identity_from_private(&bob,kb));const char *a[]={"delivery"};assert(rns_destination_hash(&alice,"lxmf",a,1,source)&&rns_destination_hash(&bob,"lxmf",a,1,destination));lxmf_store_t store={0};assert(lxmf_store_open(&store,path)==LXMF_OK);lxmf_store_message_t m={0};memcpy(m.destination,destination,16);memcpy(m.source,source,16);m.timestamp=1;m.status=LXMF_DELIVERY_QUEUED;m.content=(lxmf_slice_t){(const uint8_t*)"hello",5};bool inserted=false;assert(lxmf_store_put(&store,&m,&inserted)==LXMF_OK&&inserted);memcpy(id,m.message_id,32);send_state state={.peer=&bob};lxmf_router_t router;lxmf_router_config_t config={&alice,&store,resolve,&state,send_packet,&state};assert(lxmf_router_init(&router,&config)==LXMF_OK);assert(lxmf_router_send_message(&router,id)==LXMF_OK&&state.length>0);lxmf_store_message_t got;assert(lxmf_store_read(&store,id,&got,body,sizeof body)==LXMF_OK&&got.status==LXMF_DELIVERY_SENT);assert(lxmf_router_send_message(&router,id)==LXMF_ERR_ARGUMENT);lxmf_store_close(&store);unlink(path);return 0;}
