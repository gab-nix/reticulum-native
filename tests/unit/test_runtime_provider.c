#include "reticulum/runtime.h"
#include "reticulum/packet.h"
#include <assert.h>
#include <string.h>

typedef struct fake {
    unsigned starts, stops, destroys, polls, sends;
    rns_status_t start_error, poll_error;
    rns_interface_stats_t stats;
    size_t emit;
    uint8_t packet[500];
    size_t length;
    bool overrun;
} fake;
static rns_status_t start(void *p) {
    fake *f = p; f->starts++; return f->start_error;
}
static void stop(void *p) { ((fake *)p)->stops++; }
static void destroy(void *p) { ((fake *)p)->destroys++; }
static rns_status_t stats(void *p, rns_interface_stats_t *out) {
    *out = ((fake *)p)->stats; return RNS_OK;
}
static rns_status_t send_packet(void *p, const uint8_t *bytes, size_t n) {
    fake *f=p;
    assert(bytes != NULL && n != 0U && n <= sizeof f->packet);
    memcpy(f->packet,bytes,n); f->length=n; f->sends++; return RNS_OK;
}
static rns_status_t poll(void *p, rns_interface_receive_fn receive,
                         void *context, size_t budget) {
    fake *f = p; f->polls++;
    const uint8_t invalid[] = {0xff};
    size_t count = f->overrun || f->emit < budget ? f->emit : budget;
    for (size_t i = 0; i < count; i++) {
        rns_status_t status=receive(context, f->length ? f->packet : invalid,
                       f->length ? f->length : sizeof invalid);
        if(status!=RNS_OK) { assert(f->overrun); f->emit-=i+1U; return status; }
    }
    f->emit -= count;
    return f->poll_error;
}
static rns_interface_t *make(fake *f) {
    static const rns_interface_ops_t ops = {.start=start,.poll=poll,.send=send_packet,.get_stats=stats,.stop=stop,.destroy=destroy};
    rns_interface_t *p = NULL;
    f->stats = (rns_interface_stats_t){.online=1,.outbound=1,.broadcast=1,.effective_mtu=500};
    assert(rns_interface_create(&ops, f, &p) == RNS_OK); return p;
}
static rns_runtime_t *runtime(void) {
    rns_config_t config; rns_runtime_t *r = NULL;
    rns_config_init(&config); config.share_instance = false;
    assert(rns_runtime_create(&r, &config, NULL) == RNS_OK); return r;
}
int main(void) {
    rns_runtime_t *r = runtime(), *other = runtime();
    fake a = {0}, b = {0}, rejected = {.start_error=RNS_ERROR_IO};
    rns_interface_t *pa=make(&a), *pb=make(&b), *pr=make(&rejected);
    size_t index = 99U;
    assert(rns_runtime_attach_interface(r, pr, "bad", "fake", false, &index) == RNS_ERROR_IO);
    assert(index == 99U && rejected.destroys == 0 && rejected.stops == 0);
    rns_interface_destroy(pr); assert(rejected.destroys == 1);
    char name[] = "radio";
    assert(rns_runtime_attach_interface(r, pa, name, "sx1262", true, &index) == RNS_OK && index == 0);
    name[0]='X';
    assert(rns_runtime_attach_interface(other, pa, "duplicate", "fake", false, &index) == RNS_ERROR_INVALID_STATE);
    assert(a.starts == 1);
    assert(rns_runtime_attach_interface(r, pb, "second", "fake", false, &index) == RNS_OK && index == 1);
    rns_runtime_interface_info_t info;
    assert(rns_runtime_interface_info(r, 0, &info) == RNS_OK);
    assert(info.id == 1 && strcmp(info.name, "radio") == 0 && strcmp(info.provider_kind,"sx1262") == 0);
    assert(info.type == RNS_CONFIG_PROVIDER);
    a.emit=3; b.emit=1; a.overrun=true;
    size_t processed=0;
    assert(rns_runtime_poll(r, 1, &processed) == RNS_OK && processed == 1);
    assert(a.polls == 1 && b.polls == 1);
    assert(b.emit==1); /* Zero-budget maintenance preserves the pending RX. */
    assert(rns_runtime_interface_info(r,0,&info)==RNS_OK);
    assert(info.state == RNS_RUNTIME_INTERFACE_UP && info.last_error == RNS_OK && info.packets_dropped == 2);
    assert(rns_runtime_poll(r,1,&processed)==RNS_OK && processed==1);
    assert(rns_runtime_interface_info(r,1,&info)==RNS_OK && info.packets_received==1);
    a.emit=0; b.emit=0; a.poll_error=RNS_ERROR_IO; a.stats.online=0;
    assert(rns_runtime_poll(r,2,&processed)==RNS_ERROR_IO);
    a.poll_error=RNS_OK; a.stats.online=1;
    assert(rns_runtime_poll(r,2,&processed)==RNS_OK);
    assert(rns_runtime_interface_info(r,0,&info)==RNS_OK && info.state==RNS_RUNTIME_INTERFACE_UP);
    const uint8_t bytes[501]={0};
    a.stats.outbound=0;
    assert(rns_runtime_send(r,0,bytes,20)==RNS_ERROR_INVALID_STATE && a.sends==0);
    a.stats.outbound=1;
    assert(rns_runtime_send(r,0,bytes,sizeof bytes)==RNS_ERROR_INVALID_ARGUMENT);
    a.stats.effective_mtu=10;
    assert(rns_runtime_send(r,0,bytes,20)==RNS_ERROR_OVERFLOW);
    a.stats.effective_mtu=500;
    assert(rns_runtime_send(r,0,bytes,20)==RNS_OK && a.sends==1);
    a.stats.broadcast=0;
    assert(rns_runtime_request_path(r,bytes)==RNS_OK && a.sends==1 && b.sends==1);
    rns_identity identity; assert(rns_identity_generate(&identity));
    const char *aspects[]={"delivery"};
    assert(rns_runtime_announce(r,&identity,"lxmf",aspects,1,NULL,0)==RNS_OK);
    assert(a.sends==1 && b.sends==2);
    /* Learn a real signed announce on provider 2 and route only to that ID. */
    fake x={0}, y={0};
    rns_interface_t *px=make(&x), *py=make(&y);
    assert(rns_runtime_attach_interface(other,px,"x","fake",false,&index)==RNS_OK);
    assert(rns_runtime_attach_interface(other,py,"y","fake",false,&index)==RNS_OK);
    memcpy(y.packet,b.packet,b.length); y.length=b.length; y.emit=1;
    assert(rns_runtime_poll(other,2,&processed)==RNS_OK && processed==1);
    rns_packet announced; assert(rns_packet_decode(&announced,b.packet,b.length));
    rns_path_entry path;
    assert(rns_runtime_path_lookup(other,announced.destination_hash,&path)==RNS_OK);
    assert(path.interface_id==2);
    rns_packet outbound={0}; size_t wire_length=0; uint8_t wire[500];
    memcpy(outbound.destination_hash,announced.destination_hash,16);
    outbound.data=bytes; outbound.data_length=1;
    assert(rns_packet_encode(&outbound,wire,sizeof wire,&wire_length));
    assert(rns_runtime_send_routed(other,wire,wire_length)==RNS_OK);
    assert(x.sends==0 && y.sends==1);
    rns_interface_stats_t snapshot;
    assert(rns_runtime_interface_provider_stats(r,0,&snapshot)==RNS_OK && snapshot.broadcast==0);
    rns_runtime_destroy(r); rns_runtime_destroy(other);
    assert(a.stops==1 && a.destroys==1 && b.stops==1 && b.destroys==1);
    /* Capacity rejection never starts or consumes the caller's provider. */
    r=runtime();
    fake entries[RNS_CONFIG_MAX_INTERFACES+2U]={0};
    for(size_t i=0;i<RNS_CONFIG_MAX_INTERFACES+1U;i++) {
        rns_interface_t *p=make(&entries[i]);
        assert(rns_runtime_attach_interface(r,p,"capacity","fake",false,&index)==RNS_OK && index==i);
    }
    rns_interface_t *overflow=make(&entries[RNS_CONFIG_MAX_INTERFACES+1U]);
    assert(rns_runtime_attach_interface(r,overflow,"full","fake",false,&index)==RNS_ERROR_OVERFLOW);
    assert(entries[RNS_CONFIG_MAX_INTERFACES+1U].starts==0);
    rns_interface_destroy(overflow); rns_runtime_destroy(r);
    for(size_t i=0;i<RNS_CONFIG_MAX_INTERFACES+2U;i++) assert(entries[i].destroys==1);
    rns_config_t config; rns_config_init(&config); config.share_instance=false;
    config.interface_count=1; /* Disabled POSIX configuration retains slot 0. */
    config.interfaces[0].enabled=false;
    assert(rns_runtime_create(&r,&config,NULL)==RNS_OK);
    fake offline={0}; rns_interface_t *po=make(&offline); offline.stats.online=0;
    assert(rns_runtime_attach_interface(r,po,"offline","fake",false,&index)==RNS_OK && index==1);
    assert(rns_runtime_interface_info(r,index,&info)==RNS_OK && info.id==2 && info.state==RNS_RUNTIME_INTERFACE_STARTING);
    rns_runtime_destroy(r);
    return 0;
}
