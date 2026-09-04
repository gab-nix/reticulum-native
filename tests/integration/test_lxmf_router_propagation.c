#define _POSIX_C_SOURCE 200809L
#include "reticulum/lxmf_router.h"
#include "reticulum/destination.h"
#include "reticulum/crypto.h"
#include "reticulum/hal.h"
#include "reticulum/udp.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef struct {
    const rns_identity *recipient, *sender;
    uint8_t recipient_hash[16], sender_hash[16];
    rns_runtime_link_t *links[4]; size_t link_count;
    size_t uploads; bool valid;
    uint64_t now_ms;
} host_t;
static uint16_t port(void) { rns_udp_endpoint_t *e=NULL; rns_udp_address_t a;
    assert(rns_udp_endpoint_create(&e,RNS_UDP_IPV4)==RNS_OK);
    assert(rns_udp_bind(e,"127.0.0.1",0)==RNS_OK);
    assert(rns_udp_local_address(e,&a)==RNS_OK); rns_udp_endpoint_destroy(e);
    return a.port; }
static void config(rns_config_t *c,uint16_t listen,uint16_t forward) {
    rns_config_init(c); c->interface_count=1; rns_config_interface_t *i=&c->interfaces[0];
    strcpy(i->name,"propagation-test"); i->type=RNS_CONFIG_UDP; i->type_set=true;
    i->enabled=true; strcpy(i->listen_ip,"127.0.0.1");
    strcpy(i->forward_ip,"127.0.0.1"); i->listen_port=listen; i->forward_port=forward; }
static const rns_identity *resolve(void *context,const uint8_t hash[16]) {
    host_t *h=context; return memcmp(hash,h->recipient_hash,16)==0?h->recipient:NULL; }
static uint64_t wall(void *context) { (void)context; return 1700000000u; }
static uint64_t monotonic(void *context) { return ((host_t *)context)->now_ms; }
static void accepted(rns_runtime_destination_t *d,rns_runtime_link_t *l,void *context) {
    host_t *h=context; (void)d; assert(h->link_count<4); h->links[h->link_count++]=l; }
static bool accept_resource(rns_runtime_link_t *l,
    const rns_resource_advertisement_t *a,void *context) {
    (void)l;(void)a;(void)context; return true; }
static const rns_identity *verify_sender(void *context,const uint8_t hash[16]) {
    host_t *h=context; return memcmp(hash,h->sender_hash,16)==0?h->sender:NULL; }
static void received(rns_runtime_link_t *l,const uint8_t hash[32],rns_status_t status,
    const uint8_t *data,size_t length,void *context) {
    host_t *h=context; (void)l;(void)hash; h->uploads++;
    lxmf_pn_upload_t upload; h->valid=status==RNS_OK &&
        lxmf_pn_upload_decode(data,length,&upload)==LXMF_OK && upload.count==1;
    if (!h->valid) return;
    lxmf_slice_t item=upload.messages[0];
    if(item.len<=16u+RNS_TOKEN_OVERHEAD+32u ||
       memcmp(item.data,h->recipient_hash,16)!=0){h->valid=false;return;}
    size_t transient_length=item.len-32u; uint8_t transient[32];
    lxmf_sha256(item.data,transient_length,transient);
    if(lxmf_pow_stamp_validate_expanded(transient,1,
       LXMF_PROPAGATION_STAMP_WORKBLOCK_ROUNDS,item.data+transient_length,NULL)!=LXMF_OK){h->valid=false;return;}
    uint8_t plain[4096],packed[4112]; size_t plain_length=0;
    if(!rns_identity_decrypt(h->recipient,item.data+16,
       transient_length-16,plain,sizeof plain,&plain_length)){h->valid=false;return;}
    memcpy(packed,item.data,16); memcpy(packed+16,plain,plain_length);
    lxmf_message_t message; lxmf_identity_verifier_context_t verifier={
       .resolve=verify_sender,.resolve_context=h};
    h->valid=lxmf_unpack(packed,plain_length+16,lxmf_identity_verifier,&verifier,&message)==LXMF_OK &&
       message.content.len==2048 && memcmp(message.destination,h->recipient_hash,16)==0;
}
static void queue(lxmf_store_t *store,const rns_identity *sender,
                  const uint8_t destination[16],uint8_t id[32]) {
    uint8_t content[2048],packed[2304]; for(size_t i=0;i<sizeof content;i++)content[i]=(uint8_t)(i*71u+i/3u);
    const char *aspects[]={"delivery"}; uint8_t source[16];
    assert(rns_destination_hash(sender,"lxmf",aspects,1,source));
    lxmf_message_t message={0},decoded; memcpy(message.destination,destination,16);
    memcpy(message.source,source,16); message.timestamp=123;
    message.content=(lxmf_slice_t){content,sizeof content}; size_t packed_length;
    assert(lxmf_pack(&message,lxmf_identity_signer,(void*)sender,packed,sizeof packed,&packed_length)==LXMF_OK);
    assert(lxmf_unpack(packed,packed_length,NULL,NULL,&decoded)==LXMF_OK); memcpy(id,decoded.message_id,32);
    lxmf_store_message_t stored={0}; memcpy(stored.message_id,id,32);
    memcpy(stored.destination,destination,16); memcpy(stored.source,source,16);
    stored.timestamp=123; stored.status=LXMF_DELIVERY_QUEUED;
    stored.signature_state=LXMF_SIGNATURE_VERIFIED; stored.content=message.content;
    stored.packed=(lxmf_slice_t){packed,packed_length};
    stored.delivery.desired_method=LXMF_DELIVERY_METHOD_PROPAGATED;
    bool inserted; assert(lxmf_store_put(store,&stored,&inserted)==LXMF_OK&&inserted);
}
static void pump(rns_runtime_t *a,rns_runtime_t *b,lxmf_router_t *router,
                 const uint8_t id[32],lxmf_delivery_status_t wanted) {
    for(size_t i=0;i<15000;i++){size_t processed; lxmf_router_poll_result_t result;
        assert(rns_runtime_poll(a,32,&processed)==RNS_OK);
        assert(rns_runtime_poll(b,32,&processed)==RNS_OK);
        assert(lxmf_router_poll(router,4,&result)==LXMF_OK);
        lxmf_store_message_t stored; uint8_t content[LXMF_STORE_MAX_CONTENT];
        assert(lxmf_store_read(router->config.store,id,&stored,content,sizeof content)==LXMF_OK);
        if(stored.status==wanted)return;
    } assert(0&&"propagation operation did not complete"); }
static void fresh_store(char path[48],lxmf_store_t *store) {
    strcpy(path,"/tmp/lxmf-router-propagation-XXXXXX");
    int fd=mkstemp(path);assert(fd>=0);close(fd);unlink(path);
    memset(store,0,sizeof *store);assert(lxmf_store_open(store,path)==LXMF_OK);
}
int main(void) {
    uint16_t a=port(),b=port(); while(a==b)b=port(); rns_config_t ca,cb; config(&ca,a,b);config(&cb,b,a);
    rns_runtime_t *client=NULL,*server=NULL; assert(rns_runtime_create(&client,&ca,NULL)==RNS_OK);
    assert(rns_runtime_create(&server,&cb,NULL)==RNS_OK);
    rns_identity sender,recipient,node; assert(rns_identity_generate(&sender));
    assert(rns_identity_generate(&recipient)); assert(rns_identity_generate(&node));
    host_t host={.recipient=&recipient,.sender=&sender,.now_ms=1234000u}; const char *delivery[]={"delivery"};
    assert(rns_destination_hash(&recipient,"lxmf",delivery,1,host.recipient_hash));
    assert(rns_destination_hash(&sender,"lxmf",delivery,1,host.sender_hash));
    const char *propagation[]={"propagation"}; uint8_t node_hash[16];
    assert(rns_destination_hash(&node,"lxmf",propagation,1,node_hash));
    rns_runtime_link_options_t links={.resource_accept_callback=accept_resource,
       .resource_receive_callback=received,.max_incoming_resource_size=4096,.callback_context=&host};
    rns_runtime_destination_t *registration=NULL; assert(rns_runtime_register_link_destination(server,node_hash,
       &node,&links,accepted,&host,&registration)==RNS_OK);
    assert(rns_runtime_announce(server,&node,"lxmf",propagation,1,NULL,0)==RNS_OK);
    size_t processed; assert(rns_runtime_poll(client,32,&processed)==RNS_OK);
    char path[48]; lxmf_store_t store; fresh_store(path,&store);uint8_t id[32];queue(&store,&sender,host.recipient_hash,id);
    lxmf_router_config_t options={.identity=&sender,.store=&store,.runtime=client,
      .resolve_identity=resolve,.resolve_context=&host,.wall_clock=wall,
      .monotonic_clock=monotonic,.monotonic_clock_context=&host,
      .preferred_delivery_method=LXMF_DELIVERY_METHOD_PROPAGATED,
      .propagation_node_identity=&node,.propagation_stamp_cost=1,
      .propagation_retry_base_ms=1,.propagation_retry_limit=2};
    memcpy(options.propagation_node_destination,node_hash,16); lxmf_router_t router;
    options.propagation_node_destination[0]^=1u;
    assert(lxmf_router_init(&router,&options)==LXMF_ERR_ARGUMENT);
    options.propagation_node_destination[0]^=1u;
    options.propagation_retry_limit=LXMF_ROUTER_PROPAGATION_MAX_RETRIES+1u;
    assert(lxmf_router_init(&router,&options)==LXMF_ERR_ARGUMENT);
    options.propagation_retry_limit=2;
    options.propagation_retry_base_ms=
        LXMF_ROUTER_PROPAGATION_MAX_RETRY_BASE_MS+1u;
    assert(lxmf_router_init(&router,&options)==LXMF_ERR_ARGUMENT);
    options.propagation_retry_base_ms=1;
    assert(lxmf_router_init(&router,&options)==LXMF_OK);
    assert(lxmf_router_set_propagation_node(&router,NULL,NULL,0u)==LXMF_OK);
    assert(router.config.propagation_node_identity==NULL);
    assert(lxmf_router_set_propagation_node(&router,&node,node_hash,1u)==LXMF_OK);
    uint8_t wrong_hash[16];memcpy(wrong_hash,node_hash,sizeof wrong_hash);wrong_hash[0]^=1u;
    assert(lxmf_router_set_propagation_node(&router,&node,wrong_hash,1u)==LXMF_ERR_ARGUMENT);
    pump(client,server,&router,id,LXMF_DELIVERY_SENT);
    assert(host.uploads==1&&host.valid); lxmf_delivery_metadata_t metadata;
    assert(lxmf_store_read_delivery(&store,id,&metadata)==LXMF_OK);
    assert(metadata.actual_method==LXMF_DELIVERY_METHOD_PROPAGATED&&metadata.progress==LXMF_DELIVERY_PROGRESS_COMPLETE);
    assert(metadata.attempts==1&&!metadata.has_proof_id);
    lxmf_router_destroy(&router); lxmf_store_close(&store);unlink(path);

    /* Explicit cancellation stops a propagation stamp worker durably. */
    fresh_store(path,&store); queue(&store,&sender,host.recipient_hash,id);
    options.store=&store; options.propagation_stamp_cost=254;
    assert(lxmf_router_init(&router,&options)==LXMF_OK);
    lxmf_router_poll_result_t result; assert(lxmf_router_poll(&router,1,&result)==LXMF_OK);
    assert(router.propagation.used&&router.propagation.stamp_job!=NULL);
    assert(lxmf_router_cancel_message(&router,id)==LXMF_OK);
    assert(lxmf_store_read_delivery(&store,id,&metadata)==LXMF_OK);
    assert(metadata.queue_reason==LXMF_QUEUE_REASON_CANCELLED);
    lxmf_router_destroy(&router);lxmf_store_close(&store);unlink(path);

    /* A selected method without a verified node stays actionably queued. */
    fresh_store(path,&store);queue(&store,&sender,host.recipient_hash,id);
    options.store=&store;options.propagation_node_identity=NULL;
    options.propagation_stamp_cost=0;
    assert(lxmf_router_init(&router,&options)==LXMF_OK);
    assert(lxmf_router_poll(&router,1,&result)==LXMF_OK&&result.deferred==1);
    assert(lxmf_store_read_delivery(&store,id,&metadata)==LXMF_OK);
    assert(metadata.queue_reason==LXMF_QUEUE_REASON_PROPAGATION_NODE&&metadata.attempts==0);
    metadata.attempts=2;metadata.queue_reason=LXMF_QUEUE_REASON_RETRY_EXHAUSTED;
    assert(lxmf_store_update_delivery(&store,id,&metadata)==LXMF_OK);
    assert(lxmf_store_update_status(&store,id,LXMF_DELIVERY_FAILED)==LXMF_OK);
    assert(lxmf_router_send_message(&router,id)==LXMF_ERR_PENDING);
    assert(lxmf_store_read_delivery(&store,id,&metadata)==LXMF_OK);
    assert(metadata.queue_reason==LXMF_QUEUE_REASON_PROPAGATION_NODE&&metadata.attempts==0);
    lxmf_router_destroy(&router);lxmf_store_close(&store);unlink(path);

    /* An unannounced but valid node times out, retries once, then becomes a
     * terminal actionable failure instead of polling forever. */
    rns_identity missing;assert(rns_identity_generate(&missing));uint8_t missing_hash[16];
    assert(rns_destination_hash(&missing,"lxmf",propagation,1,missing_hash));
    fresh_store(path,&store);queue(&store,&sender,host.recipient_hash,id);
    options.store=&store;options.propagation_node_identity=&missing;
    options.monotonic_clock=NULL;options.monotonic_clock_context=NULL;
    memcpy(options.propagation_node_destination,missing_hash,16);
    options.propagation_stamp_cost=1;options.resource_timeout_seconds=0.01;
    assert(lxmf_router_init(&router,&options)==LXMF_OK);
    for(size_t i=0;i<1000;i++){
        assert(rns_runtime_poll(client,16,&processed)==RNS_OK);
        assert(lxmf_router_poll(&router,2,&result)==LXMF_OK);
        assert(lxmf_store_read_delivery(&store,id,&metadata)==LXMF_OK);
        if(metadata.queue_reason==LXMF_QUEUE_REASON_RETRY_EXHAUSTED)break;
        assert(rns_hal_sleep_ms(2)==RNS_OK);
    }
    assert(metadata.queue_reason==LXMF_QUEUE_REASON_RETRY_EXHAUSTED);
    assert(metadata.attempts==2);
    lxmf_router_destroy(&router);lxmf_store_close(&store);unlink(path);

    /* Restart recovery establishes a bounded durable deadline and safely
     * requeues an interrupted propagated SENDING record. */
    fresh_store(path,&store);queue(&store,&sender,host.recipient_hash,id);
    options.store=&store;options.propagation_node_identity=&node;
    memcpy(options.propagation_node_destination,node_hash,16);
    options.resource_timeout_seconds=0;
    assert(lxmf_store_update_status(&store,id,LXMF_DELIVERY_SENDING)==LXMF_OK);
    assert(lxmf_router_init(&router,&options)==LXMF_OK);
    lxmf_store_message_t recovered;uint8_t content[LXMF_STORE_MAX_CONTENT];
    assert(lxmf_store_read(&store,id,&recovered,content,sizeof content)==LXMF_OK);
    assert(recovered.status==LXMF_DELIVERY_QUEUED);
    assert(recovered.delivery.desired_method==LXMF_DELIVERY_METHOD_PROPAGATED);
    assert(recovered.delivery.retry_at_ms!=0);
    lxmf_router_destroy(&router);lxmf_store_close(&store);unlink(path);
    for(size_t i=0;i<host.link_count;i++)rns_runtime_link_destroy(host.links[i]);
    rns_runtime_destination_destroy(registration);rns_runtime_destroy(client);rns_runtime_destroy(server);return 0;
}
