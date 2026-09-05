/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef HELTEC_CHAT_ADMISSION_H
#define HELTEC_CHAT_ADMISSION_H
#include "chat_store.h"
#include "message_archive.h"
#include <string.h>
/* Shared by firmware and host tests; never mutates either store. */
static inline bool heltec_chat_admission_available(const heltec_chat_store *chats,
    const heltec_message_archive *archive, const lxmf_message_t *message, bool verified) {
    if(!chats || !archive || !message) return false;
    uint8_t senders[8][16]; size_t count=0,same=0;
    bool known=false,evictable_chat=false,archived=false,rotate=false;
    size_t retained=0;
    for(size_t i=0;i<8;++i) {
        const heltec_chat *c=heltec_chat_store_get(chats,i);
        if(!c) continue;
        memcpy(senders[count++],c->sender,16);
        bool pending=false,completed=false,matching=!memcmp(c->sender,message->source,16);
        for(size_t j=0;j<c->count;++j) {
            if(!memcmp(c->messages[j].id,message->message_id,32)) return true;
            if(c->messages[j].state==1 || c->messages[j].state==2) pending=true;
            else completed=true;
        }
        if(!pending) evictable_chat=true;
        if(matching) { known=true; same=c->count; retained=c->count; rotate=completed; }
    }
    for(size_t i=0;i<64;++i) {
        const heltec_archived_message *m=heltec_message_archive_get(archive,i);
        if(!m || m->signature==LXMF_SIGNATURE_VERIFIED) continue;
        archived=true;
        if(!memcmp(m->id,message->message_id,32)) return true;
        bool found=false;
        for(size_t j=0;j<count;++j) if(!memcmp(senders[j],m->source,16)) found=true;
        if(!found) { if(count==8) return false; memcpy(senders[count++],m->source,16); }
        if(!memcmp(m->source,message->source,16)) { known=true; ++same; }
    }
    if(same<8 && (known || count<8)) return true;
    if(!verified) return false;
    if(known && same==8 && retained==8 && rotate) return true;
    /* Only the chat store can evict here. With archive-only identities mixed
     * in, replacing its oldest chat may not free a combined sender slot. */
    return !known && count==8 && !archived && evictable_chat;
}
#endif
