#define _POSIX_C_SOURCE 200809L
#include "reticulum/node_registry.h"
#include <assert.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
int main(void){rns_node_registry r;rns_node_registry_init(&r,10);rns_node_record n={0};n.destination[0]=1;n.seen_at=2;assert(rns_node_registry_upsert(&r,&n));assert(rns_node_registry_get(&r,n.destination));char path[]="/tmp/rns-nodes-XXXXXX";int fd=mkstemp(path);assert(fd>=0);close(fd);assert(rns_node_registry_save(&r,path));rns_node_registry loaded;assert(rns_node_registry_load(&loaded,path,10));assert(loaded.count==1&&loaded.records[0].destination[0]==1);unlink(path);assert(rns_node_registry_expire(&r,12)==1);return 0;}
