/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "archive_view.h"
#include <stdio.h>
#include <string.h>
bool heltec_archive_view_poll(heltec_archive_view *v, heltec_message_archive *archive,
    bool next, bool select, char lines[8][22]) {
    size_t slots[64], count=0, index=0;
    memset(lines,0,8U*22U);
    for(size_t i=0;i<64;++i) {
        const heltec_archived_message *m=heltec_message_archive_get(archive,i);
        if(!m || m->signature==LXMF_SIGNATURE_VERIFIED) continue;
        if(v->selected && !memcmp(v->id,m->id,32)) index=count;
        slots[count++]=i;
    }
    if(!count) {
        memcpy(lines[0],"NO UNVERIFIED RECORDS",21);
        memcpy(lines[7],"HOLD TO RETURN",15);
        v->selected=false; v->confirm=false;
        return select;
    }
    if(next) {
        v->error=RNS_OK;
        if(v->confirm) v->remove=!v->remove;
        else index=(index+1U)%count;
    }
    const heltec_archived_message *m=heltec_message_archive_get(archive,slots[index]);
    if(v->selected && memcmp(v->id,m->id,32)) v->confirm=false;
    memcpy(v->id,m->id,32); v->selected=true;
    if(select) {
        if(!v->confirm) { v->confirm=true; v->remove=false; }
        else if(!v->remove) { v->confirm=false; return true; }
        else {
            v->error=heltec_message_archive_remove(archive,slots[index]);
            if(v->error==RNS_OK) { memcpy(v->deleted_id,v->id,32); v->deleted=true; }
            v->confirm=false; v->selected=v->error!=RNS_OK;
            return heltec_archive_view_poll(v,archive,false,false,lines);
        }
    }
    if(v->confirm) {
        (void)snprintf(lines[0],22,"DELETE THIS MESSAGE?");
        (void)snprintf(lines[2],22,"%c CANCEL / BACK",v->remove?' ':'*');
        (void)snprintf(lines[3],22,"%c DELETE",v->remove?'*':' ');
    } else {
        (void)snprintf(lines[0],22,"CLAIMED %02X%02X%02X%02X",m->source[12],m->source[13],m->source[14],m->source[15]);
        (void)snprintf(lines[1],22,"%s",m->signature==LXMF_SIGNATURE_UNVERIFIED?"UNVERIFIED SENDER":"INVALID SIGNATURE");
        if(m->signature==LXMF_SIGNATURE_FAILED) memcpy(lines[2],"CONTENT HIDDEN",15);
        else {
            size_t out=0;
            for(size_t i=0;i<m->text_length && out<84U;++i) {
                uint8_t c=m->text[i]; if((c&0xc0U)==0x80U) continue;
                lines[2U+out/21U][out%21U]=c>=32 && c<127?(char)c:c<32?' ':'?'; ++out;
            }
            if(m->text_length>84U) memcpy(lines[5]+18,"...",3);
        }
        (void)snprintf(lines[6],22,"RECORD %u/%u",(unsigned)(index+1U),(unsigned)count);
    }
    if(v->error!=RNS_OK) (void)snprintf(lines[6],22,"STORAGE ERROR %d",(int)v->error);
    memcpy(lines[7],"TAP NEXT HOLD OPEN",19);
    return false;
}
