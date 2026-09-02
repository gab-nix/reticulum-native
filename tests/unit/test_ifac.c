#include "reticulum/ifac.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static int hex(const char *s, uint8_t *out, size_t n) {
    size_t i; unsigned int v;
    for (i=0;i<n;i++) { if (sscanf(s+2*i, "%2x", &v)!=1) return 0; out[i]=(uint8_t)v; }
    return s[2*n]=='\0';
}

int main(void) {
    static const uint8_t raw[] = {0x00,0x01,0x11,0x11,0x11,0x11,0x11,0x11,0x11,0x11,0x11,0x11,0x11,0x11,0x11,0x11,0x11,0x11,0x00,0x68,0x65,0x6c,0x6c,0x6f};
    const char *key_hex="3ded9b78bf266bbd517ac9a86da81121b095131b2a2bd53c3e39397eb772c0f16713b5cbf58602df264d0401ec77d86012e41781f70c1c965eac810d27fe7844";
    const char *protected_hex="d612f0713b684339e45dc23cfc257c86c50d2fd285e49f4797405acb9d989d2085acb4876c5a2d79";
    rns_ifac ifac; uint8_t expected_key[64], expected[40], protected_raw[128], decoded[128]; size_t n, dn;
    assert(hex(key_hex, expected_key, 64) && hex(protected_hex, expected, 40));
    assert(rns_ifac_derive(&ifac,(const uint8_t*)"testnet",7,(const uint8_t*)"correct horse battery staple",28,16));
    assert(memcmp(ifac.key,expected_key,64)==0);
    assert(rns_ifac_protect(&ifac,raw,sizeof(raw),protected_raw,sizeof(protected_raw),&n));
    assert(n==40 && memcmp(protected_raw,expected,n)==0);
    assert(rns_ifac_unprotect(&ifac,protected_raw,n,decoded,sizeof(decoded),&dn));
    assert(dn==sizeof(raw) && memcmp(decoded,raw,dn)==0);
    protected_raw[n-1]^=1; assert(!rns_ifac_unprotect(&ifac,protected_raw,n,decoded,sizeof(decoded),&dn));
    assert(!rns_ifac_derive(&ifac,NULL,0,NULL,0,16));
    assert(!rns_ifac_derive(&ifac,(const uint8_t*)"x",1,NULL,0,0));
    return 0;
}
