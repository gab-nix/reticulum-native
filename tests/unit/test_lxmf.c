#include "reticulum/lxmf.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static lxmf_status_t fake_sign(void *ctx,const uint8_t *p,size_t n,uint8_t sig[64]){(void)ctx;uint8_t d[32];lxmf_sha256(p,n,d);memcpy(sig,d,32);memcpy(sig+32,d,32);return LXMF_OK;}
static lxmf_status_t fake_verify(void *ctx,const uint8_t src[16],const uint8_t *p,size_t n,const uint8_t sig[64]){uint8_t want[64];(void)ctx;(void)src;fake_sign(NULL,p,n,want);return memcmp(want,sig,64)==0?LXMF_OK:LXMF_ERR_SIGNATURE;}
static const rns_identity *resolve_one(void *ctx,const uint8_t source[16]){rns_identity *id=(rns_identity *)ctx;return memcmp(id->hash,source,16)==0?id:NULL;}
static bool cancel_now(void *ctx,uint64_t attempts){(void)ctx;(void)attempts;return false;}

int main(void){
    uint8_t sha[32];lxmf_sha256((const uint8_t *)"abc",3,sha);static const uint8_t abc[32]={0xba,0x78,0x16,0xbf,0x8f,0x01,0xcf,0xea,0x41,0x41,0x40,0xde,0x5d,0xae,0x22,0x23,0xb0,0x03,0x61,0xa3,0x96,0x17,0x7a,0x9c,0xb4,0x10,0xff,0x61,0xf2,0x00,0x15,0xad};assert(memcmp(sha,abc,32)==0);
    static const uint8_t fields[]={0x81,0x01,0xc4,0x01,0x78};lxmf_message_t m={0};for(unsigned i=0;i<16;i++){m.destination[i]=(uint8_t)i;m.source[i]=(uint8_t)(0x20+i);}m.timestamp=1.5;m.title=(lxmf_slice_t){(const uint8_t *)"hi",2};m.content=(lxmf_slice_t){(const uint8_t *)"hello",5};m.fields_msgpack=(lxmf_slice_t){fields,sizeof fields};
    static const uint8_t python_1_1_payload[]={0x94,0xcb,0x3f,0xf8,0x00,0x00,0x00,0x00,0x00,0x00,0xc4,0x02,0x68,0x69,0xc4,0x05,0x68,0x65,0x6c,0x6c,0x6f,0x81,0x01,0xc4,0x01,0x78};
    uint8_t packed[512];size_t packed_len=0;assert(lxmf_pack(&m,fake_sign,NULL,packed,sizeof packed,&packed_len)==LXMF_OK);assert(packed_len==96+sizeof python_1_1_payload);assert(memcmp(packed+96,python_1_1_payload,sizeof python_1_1_payload)==0);lxmf_message_t u;assert(lxmf_unpack(packed,packed_len,fake_verify,NULL,&u)==LXMF_OK);assert(u.timestamp==1.5&&u.title.len==2&&memcmp(u.title.data,"hi",2)==0&&u.content.len==5&&memcmp(u.content.data,"hello",5)==0&&u.fields_msgpack.len==sizeof fields);
    packed[40]^=1;assert(lxmf_unpack(packed,packed_len,fake_verify,NULL,&u)==LXMF_ERR_SIGNATURE);packed[40]^=1;
    uint8_t ticket[16]={0},stamp[16];lxmf_ticket_stamp(ticket,u.message_id,stamp);assert(lxmf_ticket_stamp_valid(stamp,ticket,u.message_id));stamp[0]^=1;assert(!lxmf_ticket_stamp_valid(stamp,ticket,u.message_id));
    assert(lxmf_unpack(packed,95,NULL,NULL,&u)==LXMF_ERR_FORMAT);
    rns_identity identity;assert(rns_identity_generate(&identity));memcpy(m.source,identity.hash,16);assert(lxmf_pack(&m,lxmf_identity_signer,&identity,packed,sizeof packed,&packed_len)==LXMF_OK);lxmf_identity_verifier_context_t vc={resolve_one,&identity};assert(lxmf_unpack(packed,packed_len,lxmf_identity_verifier,&vc,&u)==LXMF_OK);uint8_t known_source[16];memcpy(known_source,u.source,16);
    /* An identity the resolver does not hold is unknown, not forged. */
    u.source[0]^=1;assert(lxmf_identity_verifier(&vc,u.source,packed,1,u.signature)==LXMF_ERR_UNKNOWN_SIGNER);
    /* The same identity with a signature it did not make is forged. */
    assert(lxmf_identity_verifier(&vc,known_source,packed,1,u.signature)==LXMF_ERR_SIGNATURE);
    /* An unknown signer still yields a fully parsed message. */
    memcpy(m.source,identity.hash,16);m.source[0]^=1;assert(lxmf_pack(&m,lxmf_identity_signer,&identity,packed,sizeof packed,&packed_len)==LXMF_OK);lxmf_message_t unknown;assert(lxmf_unpack(packed,packed_len,lxmf_identity_verifier,&vc,&unknown)==LXMF_ERR_UNKNOWN_SIGNER);assert(unknown.content.len==5&&memcmp(unknown.content.data,"hello",5)==0);
    assert(strcmp(lxmf_status_string(LXMF_ERR_UNKNOWN_SIGNER),lxmf_status_string(LXMF_ERR_SIGNATURE))!=0);
    assert(strcmp(lxmf_signature_state_string(LXMF_SIGNATURE_UNVERIFIED),lxmf_signature_state_string(LXMF_SIGNATURE_VERIFIED))!=0);
    memcpy(m.source,identity.hash,16);assert(lxmf_pack(&m,lxmf_identity_signer,&identity,packed,sizeof packed,&packed_len)==LXMF_OK);assert(lxmf_unpack(packed,packed_len,lxmf_identity_verifier,&vc,&u)==LXMF_OK);
    uint8_t pow_stamp[32],value=0;uint64_t attempts=0;assert(lxmf_pow_stamp_generate(u.message_id,0,NULL,NULL,pow_stamp,&value,&attempts)==LXMF_ERR_ARGUMENT);assert(lxmf_pow_stamp_generate(u.message_id,1,cancel_now,NULL,pow_stamp,&value,&attempts)==LXMF_ERR_CANCELLED);assert(lxmf_pow_stamp_generate(u.message_id,1,NULL,NULL,pow_stamp,&value,&attempts)==LXMF_OK);assert(attempts>0&&value>=1);assert(lxmf_pow_stamp_validate(u.message_id,1,pow_stamp,&value)==LXMF_OK);pow_stamp[0]^=1; /* A changed stamp is overwhelmingly likely invalid at a stronger cost. */ assert(lxmf_pow_stamp_validate(u.message_id,255,pow_stamp,&value)==LXMF_ERR_FORMAT);
    size_t large_length=8192u;uint8_t *large=malloc(large_length);assert(large);for(size_t i=0u;i<large_length;i++)large[i]=(uint8_t)i;m.content=(lxmf_slice_t){large,large_length};size_t large_bound=lxmf_pack_bound(&m);assert(large_bound>4096u&&large_bound<LXMF_MAX_MESSAGE_SIZE);uint8_t *large_packed=malloc(large_bound);assert(large_packed);assert(lxmf_pack(&m,fake_sign,NULL,large_packed,large_bound,&packed_len)==LXMF_OK);assert(packed_len<=large_bound);assert(lxmf_unpack(large_packed,packed_len,fake_verify,NULL,&u)==LXMF_OK);assert(u.content.len==large_length&&memcmp(u.content.data,large,large_length)==0);free(large_packed);free(large);
    m.content=(lxmf_slice_t){(const uint8_t *)"x",LXMF_MAX_MESSAGE_SIZE};assert(lxmf_pack_bound(&m)==0u);
    puts("test_lxmf: ok");return 0;
}
