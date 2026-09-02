#include "reticulum/micron.h"
#include <assert.h>
#include <string.h>
int main(void){const char *s="# Hello\nWelcome\n[Open](/index)\ninput name\n!media photo.png\n";rns_micron_page p;assert(rns_micron_parse(&p,(const uint8_t*)s,strlen(s)));assert(p.count==5&&p.items[2].kind==RNS_MICRON_LINK&&strcmp(p.items[2].target,"/index")==0);char u[128];assert(rns_micron_normalize_url("lxmf://node/apps/home", "next", u, sizeof u)&&strcmp(u,"lxmf://node/apps/next")==0);rns_micron_history h;rns_micron_history_init(&h);assert(rns_micron_history_push(&h,"a"));assert(rns_micron_history_push(&h,"b"));assert(strcmp(rns_micron_history_back(&h),"a")==0);assert(strcmp(rns_micron_history_forward(&h),"b")==0);return 0;}
