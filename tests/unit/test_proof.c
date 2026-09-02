#include "reticulum/proof.h"
#include "reticulum/crypto.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static int hex(const char *s, uint8_t *out, size_t n) { size_t i; unsigned int v; for(i=0;i<n;i++){if(sscanf(s+2*i,"%2x",&v)!=1)return 0;out[i]=(uint8_t)v;}return s[2*n]=='\0'; }
int main(void) {
    const char *private_hex="3ded9b78bf266bbd517ac9a86da81121b095131b2a2bd53c3e39397eb772c0f16713b5cbf58602df264d0401ec77d86012e41781f70c1c965eac810d27fe7844";
    const char *proof_hex="1d40c6a2a6c68b5c8da219d84064a87d97aa110fce6fad86bc672c4206d385bb4c565c49523979f091be4f297e8a17dbdff2d29a343d2b71c54301c8184a44d782a6ca3183d474f4eca8203eda380df1ef3257956df400dfd139afaad7078608";
    static const uint8_t raw[]={0x00,0x01,0x11,0x11,0x11,0x11,0x11,0x11,0x11,0x11,0x11,0x11,0x11,0x11,0x11,0x11,0x11,0x11,0x00,0x68,0x65,0x6c,0x6c,0x6f};
    rns_identity priv,pub; uint8_t key[64], public_key[64], hash[32], expected[96], proof[96], implicit[64];
    assert(hex(private_hex,key,64)&&hex(proof_hex,expected,96)); assert(rns_identity_from_private(&priv,key));
    rns_identity_export_public(&priv,public_key); assert(rns_identity_from_public(&pub,public_key));
    assert(rns_sha256(raw,sizeof(raw),hash)); assert(rns_proof_generate_explicit(&priv,hash,proof)); assert(memcmp(proof,expected,96)==0);
    assert(rns_proof_validate(&pub,hash,proof,96)); assert(rns_proof_generate_implicit(&priv,hash,implicit)); assert(rns_proof_validate(&pub,hash,implicit,64));
    proof[0]^=1; assert(!rns_proof_validate(&pub,hash,proof,96)); proof[0]^=1; proof[95]^=1; assert(!rns_proof_validate(&pub,hash,proof,96));
    assert(!rns_proof_validate(&pub,hash,proof,95)); return 0;
}
