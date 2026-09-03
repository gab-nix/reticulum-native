#define _POSIX_C_SOURCE 200809L
#include "reticulum/node_registry.h"
#include "reticulum/destination.h"
#include "reticulum/lxmf_router.h"
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>

typedef struct {
    uint8_t destination[16],next_hop[16],public_key[64],message_destination[16];
    uint8_t app_data[RNS_NODE_APP_DATA_MAX];size_t app_data_length;
    uint64_t announce_timebase;uint8_t hops;uint64_t interface_id;int32_t gravity;
    double seen_at,expires_at;bool reachable,propagation,has_ratchet,has_message_destination;
    rns_node_kind kind;char name[64];
} legacy_record;
int main(void){rns_node_registry r;rns_node_registry_init(&r,10);rns_node_record n={0};n.destination[0]=1;n.seen_at=2;assert(rns_node_registry_upsert(&r,&n));assert(rns_node_registry_get(&r,n.destination));char path[]="/tmp/rns-nodes-XXXXXX";int fd=mkstemp(path);assert(fd>=0);close(fd);assert(rns_node_registry_save(&r,path));rns_node_registry loaded;assert(rns_node_registry_load(&loaded,path,10));assert(loaded.count==1&&loaded.records[0].destination[0]==1);unlink(path);
    char legacy_path[]="/tmp/rns-nodes-v1-XXXXXX";fd=mkstemp(legacy_path);assert(fd>=0);
    FILE *legacy_file=fdopen(fd,"wb");assert(legacy_file);legacy_record legacy={0};
    legacy.destination[0]=2;legacy.has_ratchet=true;memcpy(legacy.name,"Legacy",7);
    uint32_t legacy_count=1;assert(fwrite("RNSN1\0\0\0",1,8,legacy_file)==8);
    assert(fwrite(&legacy_count,sizeof legacy_count,1,legacy_file)==1);
    assert(fwrite(&legacy,sizeof legacy,1,legacy_file)==1&&fclose(legacy_file)==0);
    assert(rns_node_registry_load(&loaded,legacy_path,10));unlink(legacy_path);
    assert(loaded.count==1&&loaded.records[0].destination[0]==2&&loaded.records[0].has_ratchet);
    assert(loaded.records[0].ratchet[0]==0&&strcmp(loaded.records[0].name,"Legacy")==0);
    assert(rns_node_registry_expire(&r,12)==1);uint8_t private_key[64];for(size_t i=0;i<sizeof private_key;i++)private_key[i]=(uint8_t)(i+1);rns_node_result a={0};assert(rns_identity_from_private(&a.announce_identity,private_key));const char *node_aspects[]={"node"};assert(rns_destination_hash(&a.announce_identity,"nomadnetwork",node_aspects,1,a.destination_hash));a.has_verified_announce=1;a.announce_timebase=9;a.received_at=20;a.announce_app_data=(const uint8_t*)"Rei Node";a.announce_app_data_length=8;assert(rns_node_registry_consider_announce(&r,&a));const rns_node_record *record=rns_node_registry_get(&r,a.destination_hash);assert(record&&record->kind==RNS_NODE_KIND_NOMAD&&record->has_message_destination);assert(strcmp(record->name,"Rei Node")==0);
    /* Verified LXMF delivery metadata and the public ratchet are retained. */
    const char *delivery_aspects[]={"delivery"};
    assert(rns_destination_hash(&a.announce_identity,"lxmf",delivery_aspects,1,a.destination_hash));
    lxmf_announce_data_t announce={0};memcpy(announce.display_name,"Rei",3);announce.display_name_len=3;
    announce.has_stamp_cost=true;announce.stamp_cost=8;announce.features=LXMF_FEATURE_COMPRESSION;
    uint8_t app_data[64],ratchet[32];size_t app_data_len=0;memset(ratchet,0xa5,sizeof ratchet);
    assert(lxmf_announce_encode(&announce,app_data,sizeof app_data,&app_data_len)==LXMF_OK);
    a.announce_timebase=10;a.announce_app_data=app_data;a.announce_app_data_length=app_data_len;
    a.announce_has_ratchet=1;a.announce_ratchet=ratchet;
    assert(rns_node_registry_consider_announce(&r,&a));
    record=rns_node_registry_get(&r,a.destination_hash);
    assert(record&&record->kind==RNS_NODE_KIND_LXMF&&record->lxmf_app_data_valid);
    assert(record->has_ratchet&&memcmp(record->ratchet,ratchet,sizeof ratchet)==0);
    assert(record->lxmf_has_stamp_cost&&record->lxmf_stamp_cost==8);
    assert((record->lxmf_features&LXMF_FEATURE_COMPRESSION)!=0&&strcmp(record->name,"Rei")==0);
    assert(record->app_data_length==app_data_len&&memcmp(record->app_data,app_data,app_data_len)==0);
    char metadata_path[]="/tmp/rns-node-metadata-XXXXXX";
    fd=mkstemp(metadata_path);assert(fd>=0);close(fd);
    assert(rns_node_registry_save(&r,metadata_path));
    assert(rns_node_registry_load(&loaded,metadata_path,10));unlink(metadata_path);
    record=rns_node_registry_get(&loaded,a.destination_hash);
    assert(record&&record->has_ratchet&&memcmp(record->ratchet,ratchet,sizeof ratchet)==0);
    assert(record->lxmf_app_data_valid&&record->lxmf_stamp_cost==8&&strcmp(record->name,"Rei")==0);
    /* Equal/older timebases cannot overwrite verified metadata. */
    memset(ratchet,0x11,sizeof ratchet);a.announce_timebase=10;
    assert(!rns_node_registry_consider_announce(&r,&a));
    record=rns_node_registry_get(&r,a.destination_hash);
    assert(record&&record->ratchet[0]==0xa5);
    const char *propagation_aspects[]={"propagation"};
    assert(rns_destination_hash(&a.announce_identity,"lxmf",propagation_aspects,1,a.destination_hash));
    a.announce_timebase=11;a.announce_has_ratchet=0;a.announce_ratchet=NULL;
    assert(rns_node_registry_consider_announce(&r,&a));
    record=rns_node_registry_get(&r,a.destination_hash);
    assert(record&&record->kind==RNS_NODE_KIND_LXMF&&record->propagation);
    return 0;}
