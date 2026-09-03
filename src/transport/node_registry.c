#include "reticulum/node_registry.h"
#include "reticulum/lxmf_router.h"
#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include "reticulum/destination.h"
#define REGISTRY_MAGIC "RNSN2\0\0\0"
#define REGISTRY_MAGIC_V1 "RNSN1\0\0\0"

typedef struct {
    uint8_t destination[16],next_hop[16],public_key[64],message_destination[16];
    uint8_t app_data[RNS_NODE_APP_DATA_MAX];size_t app_data_length;
    uint64_t announce_timebase;uint8_t hops;uint64_t interface_id;int32_t gravity;
    double seen_at,expires_at;bool reachable,propagation,has_ratchet,has_message_destination;
    rns_node_kind kind;char name[64];
} rns_node_record_v1;

static void migrate_v1(rns_node_record *to,const rns_node_record_v1 *from){
    memset(to,0,sizeof *to);
    memcpy(to->destination,from->destination,sizeof to->destination);
    memcpy(to->next_hop,from->next_hop,sizeof to->next_hop);
    memcpy(to->public_key,from->public_key,sizeof to->public_key);
    memcpy(to->message_destination,from->message_destination,sizeof to->message_destination);
    memcpy(to->app_data,from->app_data,sizeof to->app_data);
    to->app_data_length=from->app_data_length;to->announce_timebase=from->announce_timebase;
    to->hops=from->hops;to->interface_id=from->interface_id;to->gravity=from->gravity;
    to->seen_at=from->seen_at;to->expires_at=from->expires_at;to->reachable=from->reachable;
    to->propagation=from->propagation;to->has_ratchet=from->has_ratchet;
    to->has_message_destination=from->has_message_destination;to->kind=from->kind;
    memcpy(to->name,from->name,sizeof to->name);to->name[sizeof to->name-1u]='\0';
}

void rns_node_registry_init(rns_node_registry *r, double lifetime) { if (r) { memset(r, 0, sizeof(*r)); r->lifetime = lifetime > 0 ? lifetime : 3600.0; } }
int rns_node_registry_upsert(rns_node_registry *r, const rns_node_record *x) {
    if (!r || !x) return 0; size_t i; for (i=0;i<r->count;i++) if (!memcmp(r->records[i].destination,x->destination,16)) break;
    if (i==r->count) { if (r->count==RNS_NODE_REGISTRY_MAX) return 0; r->count++; }
    r->records[i]=*x; if (r->records[i].expires_at <= 0.0) r->records[i].expires_at=r->records[i].seen_at+r->lifetime; r->records[i].name[sizeof(r->records[i].name)-1]=0; return 1;
}
size_t rns_node_registry_expire(rns_node_registry *r, double now) { if (!r) return 0; size_t n=0; for(size_t i=0;i<r->count;) { if(r->records[i].expires_at>0&&r->records[i].expires_at<=now){r->records[i]=r->records[--r->count];n++;} else i++; } return n; }
const rns_node_record *rns_node_registry_get(const rns_node_registry *r,const uint8_t d[16]) { if(!r||!d)return NULL; for(size_t i=0;i<r->count;i++)if(!memcmp(r->records[i].destination,d,16))return &r->records[i]; return NULL; }
static bool contains_ascii_casefold(const char *haystack, const char *needle) {
    if (*needle == '\0') return true;
    for (size_t i = 0u; haystack[i] != '\0'; ++i) {
        size_t j = 0u;
        while (needle[j] != '\0' && haystack[i + j] != '\0' &&
               tolower((unsigned char)haystack[i + j]) ==
                   tolower((unsigned char)needle[j]))
            ++j;
        if (needle[j] == '\0') return true;
    }
    return false;
}

static bool record_matches(const rns_node_record *record, const char *filter) {
    if (filter == NULL || *filter == '\0') return true;
    if (contains_ascii_casefold(record->name, filter)) return true;
    char address[33];
    for (size_t i = 0u; i < sizeof record->destination; ++i)
        (void)snprintf(address + i * 2u, 3u, "%02x", record->destination[i]);
    return contains_ascii_casefold(address, filter);
}

size_t rns_node_registry_list(const rns_node_registry *r, rns_node_record *out,
                              size_t cap, const char *filter) {
    if (r == NULL) return 0u;
    size_t count = 0u;
    for (size_t i = 0u; i < r->count && count < cap; ++i) {
        if (!record_matches(&r->records[i], filter)) continue;
        if (out != NULL) out[count] = r->records[i];
        ++count;
    }
    return count;
}
int rns_node_registry_consider_announce(rns_node_registry *r,const rns_node_result *a){
    if(!r||!a||!a->has_verified_announce||
       a->announce_app_data_length>RNS_NODE_APP_DATA_MAX||
       (a->announce_app_data_length&&!a->announce_app_data)||
       (a->announce_has_ratchet&&!a->announce_ratchet))return 0;
    const rns_node_record *old=rns_node_registry_get(r,a->destination_hash);
    if(old&&old->announce_timebase>=a->announce_timebase)return 0;
    rns_node_record n={0};
    memcpy(n.destination,a->destination_hash,16);memcpy(n.next_hop,a->next_hop,16);
    rns_identity_export_public(&a->announce_identity,n.public_key);
    if(a->announce_app_data_length)memcpy(n.app_data,a->announce_app_data,a->announce_app_data_length);
    n.app_data_length=a->announce_app_data_length;n.announce_timebase=a->announce_timebase;
    n.hops=a->hops;n.interface_id=a->received_interface_id;n.seen_at=a->received_at;
    n.expires_at=a->received_at+r->lifetime;n.reachable=true;
    n.has_ratchet=a->announce_has_ratchet!=0;
    if(n.has_ratchet)memcpy(n.ratchet,a->announce_ratchet,sizeof n.ratchet);
    const char *node_aspects[]={"node"};
    const char *delivery_aspects[]={"delivery"};
    const char *propagation_aspects[]={"propagation"};
    uint8_t node_hash[16],delivery_hash[16],propagation_hash[16];
    if(rns_destination_hash(&a->announce_identity,"nomadnetwork",node_aspects,1,node_hash)&&
       !memcmp(node_hash,a->destination_hash,16)){
        n.kind=RNS_NODE_KIND_NOMAD;
        if(rns_destination_hash(&a->announce_identity,"lxmf",delivery_aspects,1,delivery_hash)){
            memcpy(n.message_destination,delivery_hash,16);n.has_message_destination=true;
        }
        size_t name_length=a->announce_app_data_length<sizeof(n.name)-1?a->announce_app_data_length:sizeof(n.name)-1;
        for(size_t i=0;i<name_length;i++){unsigned char c=a->announce_app_data[i];n.name[i]=c>=32u&&c!=127u?(char)c:'?';}
    }else if(rns_destination_hash(&a->announce_identity,"lxmf",delivery_aspects,1,delivery_hash)&&
             !memcmp(delivery_hash,a->destination_hash,16)){
        lxmf_announce_data_t decoded;
        n.kind=RNS_NODE_KIND_LXMF;memcpy(n.message_destination,a->destination_hash,16);
        n.has_message_destination=true;
        if(a->announce_app_data_length&&
           lxmf_announce_parse(a->announce_app_data,a->announce_app_data_length,&decoded)==LXMF_OK){
            n.lxmf_app_data_valid=true;n.lxmf_has_stamp_cost=decoded.has_stamp_cost;
            n.lxmf_stamp_cost=decoded.stamp_cost;n.lxmf_features=decoded.features;
            size_t name_length=decoded.display_name_len<sizeof(n.name)-1?decoded.display_name_len:sizeof(n.name)-1;
            memcpy(n.name,decoded.display_name,name_length);n.name[name_length]='\0';
        }
    }else if(rns_destination_hash(&a->announce_identity,"lxmf",propagation_aspects,1,propagation_hash)&&
             !memcmp(propagation_hash,a->destination_hash,16)){
        n.kind=RNS_NODE_KIND_LXMF;n.propagation=true;
    }
    return rns_node_registry_upsert(r,&n);
}
static int before(const rns_node_record *a,const rns_node_record *b){if(a->reachable!=b->reachable)return a->reachable;if(a->seen_at!=b->seen_at)return a->seen_at>b->seen_at;return memcmp(a->destination,b->destination,16)<0;}
size_t rns_node_registry_sorted_filter(const rns_node_registry *r,
                                       rns_node_record *out, size_t cap,
                                       const char *filter) {
    size_t count = rns_node_registry_list(r, out, cap, filter);
    for (size_t i = 1u; i < count; ++i) {
        rns_node_record value = out[i];
        size_t j = i;
        while (j != 0u && before(&value, &out[j - 1u])) {
            out[j] = out[j - 1u];
            --j;
        }
        out[j] = value;
    }
    return count;
}

size_t rns_node_registry_sorted(const rns_node_registry *r, rns_node_record *out,
                                size_t cap) {
    return rns_node_registry_sorted_filter(r, out, cap, NULL);
}
int rns_node_registry_save(const rns_node_registry *r,const char *path){if(!r||!path)return 0;FILE *f=fopen(path,"wb");if(!f)return 0;uint32_t n=(uint32_t)r->count;int ok=fwrite(REGISTRY_MAGIC,1,8,f)==8&&fwrite(&n,sizeof n,1,f)==1&&fwrite(r->records,sizeof(r->records[0]),r->count,f)==r->count&&fflush(f)==0;if(fclose(f)!=0)ok=0;return ok;}
int rns_node_registry_load(rns_node_registry *r,const char *path,double lifetime){
    if(!r||!path)return 0;FILE *f=fopen(path,"rb");if(!f)return 0;
    char magic[8];uint32_t n=0;
    int header=fread(magic,1,8,f)==8&&fread(&n,sizeof n,1,f)==1&&n<=RNS_NODE_REGISTRY_MAX;
    int ok=0;
    if(header&&!memcmp(magic,REGISTRY_MAGIC,8)){
        rns_node_registry_init(r,lifetime);
        ok=fread(r->records,sizeof(r->records[0]),n,f)==n&&fgetc(f)==EOF;
    }else if(header&&!memcmp(magic,REGISTRY_MAGIC_V1,8)){
        rns_node_registry_init(r,lifetime);ok=1;
        for(uint32_t i=0;i<n&&ok;i++){
            rns_node_record_v1 old;
            ok=fread(&old,sizeof old,1,f)==1;
            if(ok)migrate_v1(&r->records[i],&old);
        }
        if(ok)ok=fgetc(f)==EOF;
    }
    if(ok)r->count=n;fclose(f);return ok;
}
