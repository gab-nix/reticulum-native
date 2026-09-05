/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "message_archive.h"
#include "reticulum/hal.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#define ARCHIVE_RECORD 946U
struct heltec_message_archive { rns_storage_t *storage; heltec_archived_message entries[64]; uint8_t wire[ARCHIVE_RECORD]; bool quarantined[64]; };
static void key_for(size_t i,char key[8]) { (void)snprintf(key,8,"msg%02u",(unsigned)i); }
static bool valid(const heltec_archived_message *m) {
    return m->signature <= LXMF_SIGNATURE_FAILED && m->signature >= LXMF_SIGNATURE_VERIFIED &&
           m->text_length <= sizeof(m->text) && m->packet_length && m->packet_length <= sizeof(m->packet);
}
static void encode(heltec_message_archive *s,const heltec_archived_message *m) {
    memset(s->wire,0,sizeof(s->wire)); s->wire[0]=1; s->wire[1]=(uint8_t)m->signature;
    s->wire[2]=(uint8_t)(m->text_length>>8); s->wire[3]=(uint8_t)m->text_length;
    s->wire[4]=(uint8_t)(m->packet_length>>8); s->wire[5]=(uint8_t)m->packet_length;
    memcpy(s->wire+6,m->source,16); memcpy(s->wire+22,m->id,32);
    for(unsigned i=0;i<8;++i) s->wire[54+i]=(uint8_t)(m->received>>(56U-8U*i));
    if(m->signature!=LXMF_SIGNATURE_FAILED) memcpy(s->wire+62,m->text,m->text_length);
    memcpy(s->wire+446,m->packet,m->packet_length);
}
static bool decode(heltec_message_archive *s,heltec_archived_message *m) {
    if(s->wire[0]!=1) return false;
    m->signature=(lxmf_signature_state_t)s->wire[1];
    m->text_length=(uint16_t)((unsigned)s->wire[2]*256U+s->wire[3]);
    m->packet_length=(uint16_t)((unsigned)s->wire[4]*256U+s->wire[5]);
    if(!valid(m)) return false;
    memcpy(m->source,s->wire+6,16); memcpy(m->id,s->wire+22,32);
    for(unsigned i=0;i<8;++i) m->received=(m->received<<8)|s->wire[54+i];
    if(m->signature!=LXMF_SIGNATURE_FAILED) memcpy(m->text,s->wire+62,m->text_length);
    else m->text_length=0;
    memcpy(m->packet,s->wire+446,m->packet_length); m->used=true; return true;
}
static bool within_limits(const heltec_message_archive *s) {
    size_t senders=0, unknown_senders=0;
    for(size_t i=0;i<64;++i) {
        const heltec_archived_message *e=&s->entries[i];
        if(!e->used) continue;
        bool first=true, first_unknown=true;
        size_t count=0, unknown=0;
        for(size_t j=0;j<64;++j) {
            const heltec_archived_message *p=&s->entries[j];
            if(!p->used || memcmp(e->source,p->source,16)) continue;
            ++count;
            if(j<i) first=false;
            if(p->signature!=LXMF_SIGNATURE_VERIFIED) {
                ++unknown;
                if(j<i) first_unknown=false;
            }
        }
        if(count>8 || unknown>4) return false;
        if(first) ++senders;
        if(e->signature!=LXMF_SIGNATURE_VERIFIED && first_unknown) ++unknown_senders;
    }
    return senders<=8 && unknown_senders<=2;
}
rns_status_t heltec_message_archive_open(rns_storage_t *storage,heltec_message_archive **out) {
    if(!storage || !out) return RNS_ERROR_INVALID_ARGUMENT;
    *out=NULL; heltec_message_archive *s=calloc(1,sizeof(*s)); if(!s) return RNS_ERROR_NO_MEMORY;
    s->storage=storage;
    for(size_t i=0;i<64;++i) {
        char key[8]; size_t n=0; key_for(i,key);
        rns_status_t status=rns_storage_read(storage,key,s->wire,sizeof(s->wire),&n);
        if(status==RNS_ERROR_NOT_FOUND) continue;
        if(status==RNS_ERROR_QUARANTINED) { s->quarantined[i]=true; continue; }
        if(status!=RNS_OK || n!=ARCHIVE_RECORD || !decode(s,&s->entries[i])) {
            heltec_message_archive_close(s); return status==RNS_OK?RNS_ERROR_PROTOCOL:status;
        }
        for(size_t j=0;j<i;++j) if(s->entries[j].used && !memcmp(s->entries[j].id,s->entries[i].id,32)) {
            heltec_message_archive_close(s); return RNS_ERROR_PROTOCOL;
        }
    }
    if(!within_limits(s)) { heltec_message_archive_close(s); return RNS_ERROR_PROTOCOL; }
    *out=s; return RNS_OK;
}
void heltec_message_archive_close(heltec_message_archive *s) { if(s) { rns_hal_secure_zero(s,sizeof(*s)); free(s); } }
const heltec_archived_message *heltec_message_archive_get(const heltec_message_archive *s,size_t i) { return s && i<64 && s->entries[i].used?&s->entries[i]:NULL; }
rns_status_t heltec_message_archive_remove(heltec_message_archive *s,size_t slot) {
    if(!s || slot>=64) return RNS_ERROR_INVALID_ARGUMENT;
    if(!s->entries[slot].used) return RNS_ERROR_NOT_FOUND;
    char key[8]; key_for(slot,key);
    rns_status_t result=rns_storage_remove(s->storage,key);
    if(result==RNS_OK) rns_hal_secure_zero(&s->entries[slot],sizeof(s->entries[slot]));
    return result;
}
rns_status_t heltec_message_archive_put(heltec_message_archive *s,const heltec_archived_message *m) {
    if(!s || !m || !valid(m)) return RNS_ERROR_INVALID_ARGUMENT;
    size_t slot=64, same=0, unknown_same=0, senders=0, unknown_senders=0;
    bool sender_known=false, unknown_sender_known=false, replacement=false;
    for(size_t i=0;i<64;++i) {
        const heltec_archived_message *e=&s->entries[i];
        if(!e->used) { if(slot==64 && !s->quarantined[i]) slot=i; continue; }
        if(!memcmp(e->id,m->id,32)) {
            if(memcmp(e->source,m->source,16)) return RNS_ERROR_PROTOCOL;
            if(e->signature==m->signature) return RNS_OK;
            if(e->signature!=LXMF_SIGNATURE_UNVERIFIED) return RNS_ERROR_INVALID_STATE;
            slot=i; replacement=true; break;
        }
        bool same_sender=!memcmp(e->source,m->source,16);
        if(same_sender) { ++same; sender_known=true; }
        bool first=true, first_unknown=true;
        for(size_t j=0;j<i;++j) if(s->entries[j].used && !memcmp(s->entries[j].source,e->source,16)) {
            first=false; if(s->entries[j].signature!=LXMF_SIGNATURE_VERIFIED) first_unknown=false;
        }
        if(first) ++senders;
        if(e->signature!=LXMF_SIGNATURE_VERIFIED) {
            if(first_unknown) ++unknown_senders;
            if(same_sender) { ++unknown_same; unknown_sender_known=true; }
        }
    }
    if(!replacement && m->signature==LXMF_SIGNATURE_FAILED) return RNS_ERROR_INVALID_ARGUMENT;
    if(!replacement && (slot==64 || same>=8 || (!sender_known && senders>=8))) return RNS_ERROR_OVERFLOW;
    if(!replacement && m->signature==LXMF_SIGNATURE_UNVERIFIED &&
        (unknown_same>=4 || (!unknown_sender_known && unknown_senders>=2))) return RNS_ERROR_OVERFLOW;
    char key[8]; key_for(slot,key); encode(s,m);
    rns_status_t result=rns_storage_write_atomic(s->storage,key,s->wire,sizeof(s->wire));
    if(result==RNS_OK) {
        s->entries[slot]=*m; s->entries[slot].used=true;
        if(m->signature==LXMF_SIGNATURE_FAILED) { rns_hal_secure_zero(s->entries[slot].text,sizeof(m->text)); s->entries[slot].text_length=0; }
    }
    return result;
}
