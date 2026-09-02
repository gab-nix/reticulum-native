#include "reticulum/lxmf_peer_store.h"
#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static bool count_peer(void *ctx,const lxmf_peer_t *p){(*(size_t *)ctx)++;assert(p->address[0]!=0);return true;}
static void set_text(char *dst,size_t *len,const char *src){*len=strlen(src);memcpy(dst,src,*len+1);}

int main(void){char path[]="/tmp/lxmf-peers-XXXXXX";int fd=mkstemp(path);assert(fd>=0);close(fd);unlink(path);lxmf_peer_store_t s={0};assert(lxmf_peer_store_open(&s,path)==LXMF_OK);lxmf_peer_t p={0};p.address[0]=7;p.trust=LXMF_PEER_TRUST_TRUSTED;p.blocked=true;p.pinned=true;p.propagation=LXMF_PEER_PROPAGATION_PREFERRED;p.has_propagation_node=true;p.propagation_node[0]=9;p.last_seen_ms=123;p.last_announce_ms=100;p.unread_count=4;set_text(p.display_name,&p.display_name_len,"Rei ð¸");set_text(p.note,&p.note_len,"met on Nomad");set_text(p.draft,&p.draft_len,"unfinished message");bool changed=false;assert(lxmf_peer_store_put(&s,&p,&changed)==LXMF_OK&&changed);assert(lxmf_peer_store_save(&s)==LXMF_OK);lxmf_peer_store_close(&s);assert(lxmf_peer_store_open(&s,path)==LXMF_OK&&lxmf_peer_store_count(&s)==1);lxmf_peer_t got;assert(lxmf_peer_store_get(&s,p.address,&got)==LXMF_OK);assert(got.trust==p.trust&&got.blocked&&got.pinned&&got.unread_count==4);assert(!strcmp(got.display_name,p.display_name)&&!strcmp(got.note,p.note)&&!strcmp(got.draft,p.draft));size_t count=0;assert(lxmf_peer_store_list(&s,count_peer,&count)==LXMF_OK&&count==1);p.unread_count=0;assert(lxmf_peer_store_put(&s,&p,&changed)==LXMF_OK&&!changed);assert(lxmf_peer_store_save(&s)==LXMF_OK);lxmf_peer_store_close(&s);
/* A corrupt main snapshot is recovered from a complete temporary snapshot. */
char tmp[sizeof path+4];snprintf(tmp,sizeof tmp,"%s.tmp",path);assert(rename(path,tmp)==0);fd=open(path,O_CREAT|O_WRONLY|O_TRUNC,0600);assert(fd>=0&&write(fd,"bad",3)==3);close(fd);assert(lxmf_peer_store_open(&s,path)==LXMF_OK);assert(lxmf_peer_store_get(&s,p.address,&got)==LXMF_OK&&got.unread_count==0);lxmf_peer_store_close(&s);
/* Invalid UTF-8 and overlong declared strings are rejected before mutation. */
assert(lxmf_peer_store_open(&s,path)==LXMF_OK);p.display_name[0]=(char)0xc0;p.display_name[1]=(char)0x80;p.display_name[2]='\0';p.display_name_len=2;assert(lxmf_peer_store_put(&s,&p,NULL)==LXMF_ERR_ARGUMENT);memset(&p,0,sizeof p);p.address[0]=8;p.display_name_len=LXMF_PEER_NAME_MAX+1;assert(lxmf_peer_store_put(&s,&p,NULL)==LXMF_ERR_ARGUMENT);bool removed=false;uint8_t address[16]={7};assert(lxmf_peer_store_remove(&s,address,&removed)==LXMF_OK&&removed);assert(lxmf_peer_store_save(&s)==LXMF_OK);lxmf_peer_store_close(&s);unlink(path);return 0;}
