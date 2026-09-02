#define _POSIX_C_SOURCE 200809L
#include "reticulum/node_registry.h"
#include "reticulum/destination.h"
#include <assert.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
int main(void){rns_node_registry r;rns_node_registry_init(&r,10);rns_node_record n={0};n.destination[0]=1;n.seen_at=2;assert(rns_node_registry_upsert(&r,&n));assert(rns_node_registry_get(&r,n.destination));char path[]="/tmp/rns-nodes-XXXXXX";int fd=mkstemp(path);assert(fd>=0);close(fd);assert(rns_node_registry_save(&r,path));rns_node_registry loaded;assert(rns_node_registry_load(&loaded,path,10));assert(loaded.count==1&&loaded.records[0].destination[0]==1);unlink(path);assert(rns_node_registry_expire(&r,12)==1);uint8_t private_key[64];for(size_t i=0;i<sizeof private_key;i++)private_key[i]=(uint8_t)(i+1);rns_node_result a={0};assert(rns_identity_from_private(&a.announce_identity,private_key));const char *node_aspects[]={"node"};assert(rns_destination_hash(&a.announce_identity,"nomadnetwork",node_aspects,1,a.destination_hash));a.has_verified_announce=1;a.announce_timebase=9;a.received_at=20;a.announce_app_data=(const uint8_t*)"Rei Node";a.announce_app_data_length=8;assert(rns_node_registry_consider_announce(&r,&a));const rns_node_record *record=rns_node_registry_get(&r,a.destination_hash);assert(record&&record->kind==RNS_NODE_KIND_NOMAD&&record->has_message_destination);assert(strcmp(record->name,"Rei Node")==0);return 0;}
