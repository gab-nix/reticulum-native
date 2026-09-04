#include "reticulum/lxmf_peer_store.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define HEADER_SIZE 24u
#define RECORD_FIXED_V1 60u
#define RECORD_FIXED 64u
#define MAX_PAYLOAD (LXMF_PEER_STORE_MAX_PEERS * (RECORD_FIXED + LXMF_PEER_NAME_MAX + LXMF_PEER_NOTE_MAX + LXMF_PEER_DRAFT_MAX))

typedef struct {
    char path[LXMF_PEER_STORE_PATH_MAX + 1u];
    lxmf_peer_t *peers;
    size_t count;
} peer_store_impl;

static void put16(uint8_t *p,uint16_t v){p[0]=(uint8_t)(v>>8);p[1]=(uint8_t)v;}
static uint16_t get16(const uint8_t *p){return (uint16_t)(((uint16_t)p[0]<<8)|p[1]);}
static void put32(uint8_t *p,uint32_t v){p[0]=(uint8_t)(v>>24);p[1]=(uint8_t)(v>>16);p[2]=(uint8_t)(v>>8);p[3]=(uint8_t)v;}
static uint32_t get32(const uint8_t *p){return ((uint32_t)p[0]<<24)|((uint32_t)p[1]<<16)|((uint32_t)p[2]<<8)|p[3];}
static void put64(uint8_t *p,uint64_t v){for(unsigned i=0;i<8;i++)p[7-i]=(uint8_t)(v>>(i*8));}
static uint64_t get64(const uint8_t *p){uint64_t v=0;for(unsigned i=0;i<8;i++)v=(v<<8)|p[i];return v;}
static uint32_t crc32_bytes(const uint8_t *p,size_t n){uint32_t c=0xffffffffu;while(n--){c^=*p++;for(unsigned i=0;i<8;i++)c=(c>>1)^(0xedb88320u&(~(c&1u)+1u));}return ~c;}

static bool valid_utf8(const uint8_t *s,size_t n){size_t i=0;while(i<n){uint8_t c=s[i++];if(c<0x80){if(c==0)return false;continue;}unsigned more;uint32_t cp;if((c&0xe0)==0xc0){more=1;cp=c&0x1f;if(cp<2)return false;}else if((c&0xf0)==0xe0){more=2;cp=c&15;}else if((c&0xf8)==0xf0){more=3;cp=c&7;}else return false;if(i+more>n)return false;for(unsigned j=0;j<more;j++){uint8_t d=s[i++];if((d&0xc0)!=0x80)return false;cp=(cp<<6)|(d&63);}if(cp>0x10ffff||(cp>=0xd800&&cp<=0xdfff)||(more==2&&cp<0x800)||(more==3&&cp<0x10000))return false;}return true;}
static bool valid_peer(const lxmf_peer_t *p){return p&&p->display_name_len<=LXMF_PEER_NAME_MAX&&p->note_len<=LXMF_PEER_NOTE_MAX&&p->draft_len<=LXMF_PEER_DRAFT_MAX&&p->display_name[p->display_name_len]=='\0'&&p->note[p->note_len]=='\0'&&p->draft[p->draft_len]=='\0'&&valid_utf8((const uint8_t *)p->display_name,p->display_name_len)&&valid_utf8((const uint8_t *)p->note,p->note_len)&&valid_utf8((const uint8_t *)p->draft,p->draft_len)&&p->trust<=LXMF_PEER_TRUST_UNTRUSTED&&p->propagation<=LXMF_PEER_PROPAGATION_PREFERRED;}
static ptrdiff_t find_peer(const peer_store_impl *s,const uint8_t address[16]){for(size_t i=0;i<s->count;i++)if(memcmp(s->peers[i].address,address,16)==0)return (ptrdiff_t)i;return -1;}

static lxmf_status_t decode_file(const char *path, lxmf_peer_t *peers,
                                 size_t *count) {
    FILE *f = fopen(path, "rb");
    if (!f) return errno == ENOENT ? LXMF_ERR_FORMAT : LXMF_ERR_CRYPTO;
    uint8_t h[HEADER_SIZE];
    lxmf_status_t st = LXMF_ERR_FORMAT;
    if (fread(h, 1, sizeof h, f) != sizeof h ||
        memcmp(h, "LXPEERS\0", 8) != 0 || get16(h + 10) != HEADER_SIZE)
        goto done;
    uint16_t version = get16(h + 8);
    if (version != 1u && version != LXMF_PEER_STORE_VERSION) goto done;
    size_t fixed = version == 1u ? RECORD_FIXED_V1 : RECORD_FIXED;
    size_t unread_offset = version == 1u ? 50u : 54u;
    size_t length_offset = version == 1u ? 54u : 58u;
    uint32_t n = get32(h + 12);
    uint32_t payload_len = get32(h + 16);
    uint32_t want_crc = get32(h + 20);
    if (n > LXMF_PEER_STORE_MAX_PEERS || payload_len > MAX_PAYLOAD) goto done;
    uint8_t *payload = payload_len ? malloc(payload_len) : NULL;
    if (payload_len && !payload) {
        st = LXMF_ERR_BOUNDS;
        goto done;
    }
    if ((payload_len && fread(payload, 1, payload_len, f) != payload_len) ||
        fgetc(f) != EOF || crc32_bytes(payload, payload_len) != want_crc) {
        free(payload);
        goto done;
    }
    size_t off = 0;
    for (uint32_t i = 0; i < n; i++) {
        if (payload_len - off < fixed) {
            free(payload);
            goto done;
        }
        const uint8_t *r = payload + off;
        uint16_t nl = get16(r + length_offset);
        uint16_t ol = get16(r + length_offset + 2u);
        uint16_t dl = get16(r + length_offset + 4u);
        size_t rec = fixed + (size_t)nl + ol + dl;
        if (nl > LXMF_PEER_NAME_MAX || ol > LXMF_PEER_NOTE_MAX ||
            dl > LXMF_PEER_DRAFT_MAX || rec > payload_len - off) {
            free(payload);
            goto done;
        }
        lxmf_peer_t *p = &peers[i];
        memset(p, 0, sizeof *p);
        memcpy(p->address, r, 16);
        p->trust = (lxmf_peer_trust_t)r[16];
        p->blocked = r[17] != 0;
        p->pinned = r[18] != 0;
        p->propagation = (lxmf_peer_propagation_t)r[19];
        p->has_propagation_node = r[20] != 0;
        if (r[17] > 1 || r[18] > 1 || r[20] > 1) {
            free(payload);
            goto done;
        }
        memcpy(p->propagation_node, r + 22, 16);
        p->last_seen_ms = get64(r + 38);
        p->last_announce_ms = get64(r + 46);
        p->unread_count = get32(r + unread_offset);
        const uint8_t *peer_text = r + fixed;
        memcpy(p->display_name, peer_text, nl);
        p->display_name_len = nl;
        peer_text += nl;
        memcpy(p->note, peer_text, ol);
        p->note_len = ol;
        peer_text += ol;
        memcpy(p->draft, peer_text, dl);
        p->draft_len = dl;
        if (!valid_peer(p) ||
            find_peer(&(peer_store_impl){.peers = peers, .count = i},
                      p->address) >= 0) {
            free(payload);
            goto done;
        }
        off += rec;
    }
    free(payload);
    if (off != payload_len) goto done;
    *count = n;
    st = LXMF_OK;
done:
    fclose(f);
    return st;
}

static lxmf_status_t sync_parent(const char *path){char dir[LXMF_PEER_STORE_PATH_MAX+1];size_t n=strlen(path);if(n>=sizeof dir)return LXMF_ERR_BOUNDS;memcpy(dir,path,n+1);char *slash=strrchr(dir,'/');if(slash){if(slash==dir)slash[1]='\0';else *slash='\0';}else strcpy(dir,".");int fd=open(dir,O_RDONLY);if(fd<0)return LXMF_ERR_CRYPTO;int rc=fsync(fd);close(fd);return rc==0?LXMF_OK:LXMF_ERR_CRYPTO;}

lxmf_status_t lxmf_peer_store_open(lxmf_peer_store_t *store,const char *path){if(!store||!path||store->implementation||strlen(path)>LXMF_PEER_STORE_PATH_MAX)return LXMF_ERR_ARGUMENT;peer_store_impl *s=calloc(1,sizeof *s);if(!s)return LXMF_ERR_BOUNDS;s->peers=calloc(LXMF_PEER_STORE_MAX_PEERS,sizeof *s->peers);if(!s->peers){free(s);return LXMF_ERR_BOUNDS;}strcpy(s->path,path);char tmp[LXMF_PEER_STORE_PATH_MAX+5];snprintf(tmp,sizeof tmp,"%s.tmp",path);lxmf_status_t main_st=decode_file(path,s->peers,&s->count);if(main_st!=LXMF_OK){size_t recovered=0;lxmf_status_t tmp_st=decode_file(tmp,s->peers,&recovered);if(tmp_st==LXMF_OK){if(rename(tmp,path)!=0){free(s->peers);free(s);return LXMF_ERR_CRYPTO;}s->count=recovered;(void)sync_parent(path);}else if(access(path,F_OK)==0){free(s->peers);free(s);return main_st;}else{s->count=0;}}else unlink(tmp);store->implementation=s;return LXMF_OK;}
void lxmf_peer_store_close(lxmf_peer_store_t *store){if(!store||!store->implementation)return;peer_store_impl *s=store->implementation;free(s->peers);free(s);store->implementation=NULL;}
size_t lxmf_peer_store_count(const lxmf_peer_store_t *store){return store&&store->implementation?((peer_store_impl *)store->implementation)->count:0;}
lxmf_status_t lxmf_peer_store_get(const lxmf_peer_store_t *store,const uint8_t address[16],lxmf_peer_t *peer){if(!store||!store->implementation||!address||!peer)return LXMF_ERR_ARGUMENT;peer_store_impl *s=store->implementation;ptrdiff_t i=find_peer(s,address);if(i<0)return LXMF_ERR_FORMAT;*peer=s->peers[i];return LXMF_OK;}
lxmf_status_t lxmf_peer_store_put(lxmf_peer_store_t *store,const lxmf_peer_t *peer,bool *inserted){if(inserted)*inserted=false;if(!store||!store->implementation||!valid_peer(peer))return LXMF_ERR_ARGUMENT;peer_store_impl *s=store->implementation;ptrdiff_t i=find_peer(s,peer->address);if(i<0){if(s->count>=LXMF_PEER_STORE_MAX_PEERS)return LXMF_ERR_BOUNDS;i=(ptrdiff_t)s->count++;if(inserted)*inserted=true;}s->peers[i]=*peer;return LXMF_OK;}
lxmf_status_t lxmf_peer_store_remove(lxmf_peer_store_t *store,const uint8_t address[16],bool *removed){if(removed)*removed=false;if(!store||!store->implementation||!address)return LXMF_ERR_ARGUMENT;peer_store_impl *s=store->implementation;ptrdiff_t i=find_peer(s,address);if(i<0)return LXMF_OK;s->peers[i]=s->peers[s->count-1];s->count--;if(removed)*removed=true;return LXMF_OK;}
lxmf_status_t lxmf_peer_store_list(const lxmf_peer_store_t *store,lxmf_peer_store_list_fn cb,void *ctx){if(!store||!store->implementation||!cb)return LXMF_ERR_ARGUMENT;peer_store_impl *s=store->implementation;for(size_t i=0;i<s->count;i++)if(!cb(ctx,&s->peers[i]))break;return LXMF_OK;}

lxmf_status_t lxmf_peer_store_save(lxmf_peer_store_t *store) {
    if (!store || !store->implementation) return LXMF_ERR_ARGUMENT;
    peer_store_impl *s = store->implementation;
    size_t payload_len = 0;
    for (size_t i = 0; i < s->count; i++) {
        if (!valid_peer(&s->peers[i])) return LXMF_ERR_ARGUMENT;
        payload_len += RECORD_FIXED + s->peers[i].display_name_len +
                       s->peers[i].note_len + s->peers[i].draft_len;
    }
    uint8_t *payload = payload_len ? malloc(payload_len) : NULL;
    if (payload_len && !payload) return LXMF_ERR_BOUNDS;
    size_t off = 0;
    for (size_t i = 0; i < s->count; i++) {
        lxmf_peer_t *p = &s->peers[i];
        uint8_t *r = payload + off;
        memset(r, 0, RECORD_FIXED);
        memcpy(r, p->address, 16);
        r[16] = (uint8_t)p->trust;
        r[17] = (uint8_t)p->blocked;
        r[18] = (uint8_t)p->pinned;
        r[19] = (uint8_t)p->propagation;
        r[20] = (uint8_t)p->has_propagation_node;
        memcpy(r + 22, p->propagation_node, 16);
        put64(r + 38, p->last_seen_ms);
        put64(r + 46, p->last_announce_ms);
        put32(r + 54, p->unread_count);
        put16(r + 58, (uint16_t)p->display_name_len);
        put16(r + 60, (uint16_t)p->note_len);
        put16(r + 62, (uint16_t)p->draft_len);
        uint8_t *peer_text = r + RECORD_FIXED;
        memcpy(peer_text, p->display_name, p->display_name_len);
        peer_text += p->display_name_len;
        memcpy(peer_text, p->note, p->note_len);
        peer_text += p->note_len;
        memcpy(peer_text, p->draft, p->draft_len);
        off += RECORD_FIXED + p->display_name_len + p->note_len +
               p->draft_len;
    }
    uint8_t h[HEADER_SIZE] = {0};
    memcpy(h, "LXPEERS\0", 8);
    put16(h + 8, LXMF_PEER_STORE_VERSION);
    put16(h + 10, HEADER_SIZE);
    put32(h + 12, (uint32_t)s->count);
    put32(h + 16, (uint32_t)payload_len);
    put32(h + 20, crc32_bytes(payload, payload_len));
    char tmp[LXMF_PEER_STORE_PATH_MAX + 5];
    snprintf(tmp, sizeof tmp, "%s.tmp", s->path);
    FILE *f = fopen(tmp, "wb");
    if (!f) {
        free(payload);
        return LXMF_ERR_CRYPTO;
    }
    lxmf_status_t st = LXMF_OK;
    if (fwrite(h, 1, sizeof h, f) != sizeof h ||
        (payload_len && fwrite(payload, 1, payload_len, f) != payload_len) ||
        fflush(f) != 0 || fsync(fileno(f)) != 0)
        st = LXMF_ERR_CRYPTO;
    free(payload);
    if (fclose(f) != 0) st = LXMF_ERR_CRYPTO;
    if (st == LXMF_OK && rename(tmp, s->path) != 0) st = LXMF_ERR_CRYPTO;
    if (st == LXMF_OK) st = sync_parent(s->path);
    return st;
}
