#include "reticulum/lxmf_router.h"
#include <assert.h>
#include <string.h>

static uint64_t fake_now(void *p){return *(uint64_t *)p;}
int main(void){
    lxmf_announce_data_t a={0};memcpy(a.display_name,"Rei",3);a.display_name_len=3;a.has_stamp_cost=true;a.stamp_cost=8;a.features=LXMF_FEATURE_COMPRESSION;uint8_t wire[64];size_t n;assert(lxmf_announce_encode(&a,wire,sizeof wire,&n)==LXMF_OK);static const uint8_t expected[]={0x93,0xc4,0x03,'R','e','i',0x08,0x91,0x00};assert(n==sizeof expected&&memcmp(wire,expected,n)==0);lxmf_announce_data_t p;assert(lxmf_announce_parse(wire,n,&p)==LXMF_OK);assert(p.display_name_len==3&&p.has_stamp_cost&&p.stamp_cost==8&&(p.features&LXMF_FEATURE_COMPRESSION));
    assert(lxmf_announce_parse((const uint8_t *)"Legacy",6,&p)==LXMF_OK);assert(p.display_name_len==6&&!p.has_stamp_cost&&(p.features&LXMF_FEATURE_COMPRESSION));static const uint8_t short_form[]={0x91,0xc4,0x01,'X'};assert(lxmf_announce_parse(short_form,sizeof short_form,&p)==LXMF_OK);assert(p.features&LXMF_FEATURE_COMPRESSION);
    static const uint8_t extended[]={0x94,0xc4,0x03,'R','e','i',0x08,0x92,0x00,0x2a,0x81,0xa1,'x',0x01};
    assert(lxmf_announce_parse(extended,sizeof extended,&p)==LXMF_OK);
    assert(p.supported_function_count==2&&p.supported_functions[0]==0&&p.supported_functions[1]==42);
    assert(p.extension_count==1&&p.extensions_len==4&&memcmp(p.extensions,extended+10,4)==0);
    uint8_t roundtrip[64];size_t roundtrip_len=0;
    assert(lxmf_announce_encode(&p,roundtrip,sizeof roundtrip,&roundtrip_len)==LXMF_OK);
    assert(roundtrip_len==sizeof extended&&memcmp(roundtrip,extended,sizeof extended)==0);
    p.extension_count=2;
    assert(lxmf_announce_encode(&p,roundtrip,sizeof roundtrip,&roundtrip_len)==LXMF_ERR_ARGUMENT);
    uint64_t now=10;lxmf_contact_t storage[2];lxmf_contact_book_t b;assert(lxmf_contact_book_init(&b,storage,2,fake_now,&now)==LXMF_OK);uint8_t h1[16]={1},h2[16]={2},h3[16]={3},pub[64]={9};assert(lxmf_contact_book_update(&b,h1,pub,&p)==LXMF_OK);assert(lxmf_contact_book_lookup(&b,h1)->last_seen==10);now=20;assert(lxmf_contact_book_update(&b,h1,pub,&p)==LXMF_OK&&b.count==1);assert(lxmf_contact_book_update(&b,h2,pub,&p)==LXMF_OK&&b.count==2);assert(lxmf_contact_book_update(&b,h3,pub,&p)==LXMF_ERR_BOUNDS);now=31;assert(lxmf_contact_book_expire(&b,10)==2&&b.count==0);assert(!lxmf_contact_book_lookup(&b,h1));return 0;
}
