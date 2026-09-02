#include "reticulum/node_registry.h"
#include <assert.h>
#include <string.h>
int main(void){rns_node_registry r;rns_node_registry_init(&r,10);rns_node_record n={0};n.destination[0]=1;n.seen_at=2;assert(rns_node_registry_upsert(&r,&n));assert(rns_node_registry_get(&r,n.destination));assert(rns_node_registry_expire(&r,12)==1);return 0;}
