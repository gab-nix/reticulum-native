#include "../../apps/tui_model.h"

#include <assert.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>

static void put(lxmf_store_t *s,uint8_t id,uint8_t src,uint8_t dst,double when,const char *text){lxmf_store_message_t m={0};m.message_id[0]=id;m.source[0]=src;m.destination[0]=dst;m.timestamp=when;m.status=LXMF_DELIVERY_DELIVERED;m.content=(lxmf_slice_t){(const uint8_t*)text,strlen(text)};bool inserted;assert(lxmf_store_put(s,&m,&inserted)==LXMF_OK&&inserted);}
typedef struct{uint8_t local[16];uint8_t id;} queue_ctx;
static lxmf_status_t queue(void *p,const uint8_t peer[16],const uint8_t *body,size_t n,lxmf_store_message_t *out){queue_ctx *q=p;assert(peer[0]==2);assert(n==5&&!memcmp(body,"hello",5));memcpy(out->source,q->local,16);memcpy(out->destination,peer,16);out->message_id[0]=q->id;out->timestamp=30;return LXMF_OK;}

int main(void){
    char path[]="/tmp/tui-model-XXXXXX";int fd=mkstemp(path);assert(fd>=0);close(fd);
    lxmf_store_t store={0};assert(lxmf_store_open(&store,path)==LXMF_OK);
    put(&store,1,2,1,10,"first");put(&store,2,1,3,20,"sent");put(&store,3,4,1,15,"other");put(&store,4,8,9,99,"foreign");
    uint8_t local[16]={1};tui_model_t m;tui_model_init(&m,local);assert(tui_model_load(&m,&store)==LXMF_OK);
    assert(tui_model_conversation_count(&m)==3);assert(tui_model_conversation(&m,0)->peer[0]==4);assert(tui_model_conversation(&m,0)->unread_count==1);
    assert(tui_model_select(&m,1));assert(tui_model_conversation(&m,1)->peer[0]==2);assert(tui_model_selected_message_count(&m)==1);assert(!memcmp(tui_model_selected_message(&m,0)->content,"first",5));
    tui_model_mark_selected_read(&m);assert(tui_model_conversation(&m,tui_model_selected(&m))->unread_count==0);assert(tui_model_conversation(&m,tui_model_selected(&m))->peer[0]==2);
    assert(tui_model_composer_insert(&m,"h\xc3\xa9",3));assert(tui_model_composer_cursor(&m)==3);assert(tui_model_composer_left(&m)&&tui_model_composer_cursor(&m)==1);assert(tui_model_composer_backspace(&m));assert(!strcmp(tui_model_composer(&m),"\xc3\xa9"));assert(tui_model_composer_right(&m));assert(tui_model_composer_insert(&m,"llo",3));assert(!strcmp(tui_model_composer(&m),"\xc3\xa9llo"));assert(tui_model_composer_backspace(&m));tui_model_composer_clear(&m);
    assert(!tui_model_composer_insert(&m,"\xc0\x80",2));assert(tui_model_composer_insert(&m,"hello",5));queue_ctx q={{1},9};assert(tui_model_submit(&m,queue,&q)==LXMF_OK);assert(!tui_model_composer_len(&m));assert(tui_model_selected_message_count(&m)==2);const tui_message_t *last=tui_model_selected_message(&m,1);assert(last&&last->outgoing&&last->status==LXMF_DELIVERY_QUEUED&&!memcmp(last->content,"hello",5));
    tui_model_set_scroll(&m,7);assert(tui_model_scroll(&m)==7);assert(!tui_model_select(&m,99));
    uint8_t seeded[16]={7};assert(tui_model_seed_conversation(&m,seeded));assert(tui_model_selected_peer(&m)[0]==7);assert(tui_model_selected_message_count(&m)==0);
    lxmf_store_close(&store);unlink(path);return 0;
}
