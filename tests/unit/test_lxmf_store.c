#include "reticulum/lxmf_store.h"
#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static bool count_cb(void *p,const lxmf_store_message_t *m){(*(size_t *)p)++;assert(m->content.len>0);return true;}
int main(void){char path[]="/tmp/lxmf-store-XXXXXX";int fd=mkstemp(path);assert(fd>=0);close(fd);lxmf_store_t s={0};assert(lxmf_store_open(&s,path)==LXMF_OK);lxmf_store_message_t m={0};m.message_id[0]=1;m.destination[0]=2;m.source[0]=3;m.timestamp=12.5;m.status=LXMF_DELIVERY_QUEUED;m.content=(lxmf_slice_t){(const uint8_t *)"hello",5};bool inserted=false;assert(lxmf_store_put(&s,&m,&inserted)==LXMF_OK&&inserted);assert(lxmf_store_put(&s,&m,&inserted)==LXMF_OK&&!inserted&&lxmf_store_count(&s)==1);assert(lxmf_store_update_status(&s,m.message_id,LXMF_DELIVERY_DELIVERED)==LXMF_OK);uint8_t body[16];lxmf_store_message_t got;assert(lxmf_store_read(&s,m.message_id,&got,body,sizeof body)==LXMF_OK);assert(got.status==LXMF_DELIVERY_DELIVERED&&got.timestamp==12.5&&got.content.len==5&&!memcmp(body,"hello",5));size_t count=0;assert(lxmf_store_list(&s,count_cb,&count)==LXMF_OK&&count==1);assert(lxmf_store_compact(&s)==LXMF_OK&&lxmf_store_count(&s)==1);lxmf_store_close(&s);
    fd=open(path,O_WRONLY|O_APPEND);assert(fd>=0);assert(write(fd,"LXMS",4)==4);close(fd);assert(lxmf_store_open(&s,path)==LXMF_OK&&lxmf_store_count(&s)==1);lxmf_store_close(&s);assert(lxmf_store_open(&s,path)==LXMF_OK&&lxmf_store_count(&s)==1);lxmf_store_close(&s);unlink(path);return 0;}
