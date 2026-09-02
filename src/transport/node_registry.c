#include "reticulum/node_registry.h"
#include <string.h>

void rns_node_registry_init(rns_node_registry *r, double lifetime) { if (r) { memset(r, 0, sizeof(*r)); r->lifetime = lifetime > 0 ? lifetime : 3600.0; } }
int rns_node_registry_upsert(rns_node_registry *r, const rns_node_record *x) {
    if (!r || !x) return 0; size_t i; for (i=0;i<r->count;i++) if (!memcmp(r->records[i].destination,x->destination,16)) break;
    if (i==r->count) { if (r->count==RNS_NODE_REGISTRY_MAX) return 0; r->count++; }
    r->records[i]=*x; if (r->records[i].expires_at <= 0.0) r->records[i].expires_at=r->records[i].seen_at+r->lifetime; r->records[i].name[sizeof(r->records[i].name)-1]=0; return 1;
}
size_t rns_node_registry_expire(rns_node_registry *r, double now) { if (!r) return 0; size_t n=0; for(size_t i=0;i<r->count;) { if(r->records[i].expires_at>0&&r->records[i].expires_at<=now){r->records[i]=r->records[--r->count];n++;} else i++; } return n; }
const rns_node_record *rns_node_registry_get(const rns_node_registry *r,const uint8_t d[16]) { if(!r||!d)return NULL; for(size_t i=0;i<r->count;i++)if(!memcmp(r->records[i].destination,d,16))return &r->records[i]; return NULL; }
size_t rns_node_registry_list(const rns_node_registry *r,rns_node_record *out,size_t cap,const char *filter){if(!r)return 0;size_t n=0;for(size_t i=0;i<r->count&&n<cap;i++){if(filter&&*filter&&!strstr(r->records[i].name,filter))continue;if(out)out[n]=r->records[i];n++;}return n;}
