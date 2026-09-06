/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "chat_view.h"
#include <stdio.h>
#include <string.h>
static const char *replies[]={"I'm okay","SOS","I'll be there","Almost there","Yes","No","Thank you","Cancel"};
/* Bounded ASCII display projection. Every retained byte is visited; UTF-8
 * continuation bytes are consumed and unsupported glyphs become '?'. */
static size_t wrapped(const heltec_chat_message *m,unsigned page,char rows[4][22]) {
    char text[HELTEC_CHAT_TEXT+1]; size_t n=0,row=0,pos=0;
    memset(rows,0,4U*22U);
    for(size_t i=0;i<m->length;++i) {
        uint8_t c=m->text[i]; if((c&0xc0U)==0x80U) continue;
        text[n++]=c>=32U && c<127U?(char)c:c=='\n'?'\n':c<32U?' ':'?';
    }
    text[n]=0;
    while(pos<n && row<HELTEC_CHAT_TEXT) {
        size_t end=pos; while(end<n && end-pos<21U && text[end]!='\n') ++end;
        size_t take=end-pos,next=end;
        if(end<n && text[end]=='\n') next=end+1U;
        else if(end<n && take==21U) {
            size_t space=end; while(space>pos && text[space-1U]!=' ') --space;
            if(space>pos) { take=space-pos-1U; next=space; }
        }
        if(row/4U==page) memcpy(rows[row%4U],text+pos,take);
        ++row; pos=next;
    }
    return row?row:1U;
}
bool heltec_chat_view_open_unread(heltec_chat_view *v,const heltec_chat_store *s) {
    const heltec_chat_message *latest=NULL; const heltec_chat *owner=NULL;
    for(size_t i=0;i<HELTEC_CHAT_COUNT;++i) {
        const heltec_chat *c=heltec_chat_store_get(s,i); if(!c) continue;
        for(size_t j=0;j<c->count;++j) if(c->messages[j].unread &&
            (!latest || c->messages[j].timestamp>latest->timestamp)) { latest=&c->messages[j]; owner=c; }
    }
    if(!latest) return false;
    memcpy(v->sender,owner->sender,16); memcpy(v->message,latest->id,32);
    v->selected=true; v->message_selected=true; v->back=false; v->screen=1; v->page=0;
    return true;
}
bool heltec_chat_view_poll(heltec_chat_view *v,heltec_chat_store *store,
                          bool next,bool select,char lines[8][22]) {
    size_t slots[8],count=0,index=0,mi=0;
    memset(lines,0,8U*22U);
    if(next) { v->error=RNS_OK; v->reply_queued=false; }
    for(size_t i=0;i<8;++i) if(heltec_chat_store_get(store,i)) slots[count++]=i;
    bool found=false;
    for(size_t i=0;i<count;++i) if(v->selected && !memcmp(v->sender,heltec_chat_store_get(store,slots[i])->sender,16)) { index=i; found=true; }
    if(v->selected && !found) { v->screen=0; v->message_selected=false; }
    if(!count) { v->screen=0; v->selected=false; v->back=true; }
    if(v->screen==0 && next) {
        if(v->back) { v->back=false; index=0; }
        else if(index+1U<count) ++index;
        else v->back=true;
        v->message_selected=false;
    }
    const heltec_chat *chat=count?heltec_chat_store_get(store,slots[index]):NULL;
    if(chat) { memcpy(v->sender,chat->sender,16); v->selected=true; }
    if(chat && v->message_selected)
        for(size_t i=0;i<chat->count;++i) if(!memcmp(v->message,chat->messages[i].id,32)) mi=i;
    char rows[4][22];
    if(v->screen==1 && next && chat && chat->count) {
        size_t pages=(wrapped(&chat->messages[mi],v->page,rows)+3U)/4U;
        if(v->page+1U<pages) ++v->page;
        else { mi=(mi+1U)%chat->count; v->page=0; }
    }
    if(v->screen==2 && next) v->action=(v->action+1U)%5U;
    if(v->screen==3 && next) v->action=1U-v->action;
    if(v->screen==4 && next) v->reply=(v->reply+1U)%8U;
    if(v->screen==5 && next) v->action=1U-v->action;
    if(v->screen==6 && next) v->action=1U-v->action;
    if(select) {
        if(v->screen==0) { if(v->back || !chat) return true; v->screen=1; v->page=0; }
        else if(v->screen==1) { v->screen=2; v->action=0; }
        else if(v->screen==2) {
            if(v->action==0) { v->screen=0; v->back=false; }
            else if(v->action==1) v->screen=1;
            else if(v->action==2) {
                if(v->send_reply) { v->screen=4; v->reply=0; }
                else v->error=RNS_ERROR_UNSUPPORTED;
            }
            else if(v->action==3) { v->screen=3; v->action=0; }
            else if(v->cancel_reply && chat && chat->count &&
                (chat->messages[mi].state==1 || chat->messages[mi].state==2)) { v->screen=6; v->action=0; }
            else v->error=RNS_ERROR_INVALID_STATE;
        } else if(v->screen==3) {
            if(v->action && chat) {
                v->error=heltec_chat_store_delete(store,slots[index]);
                v->screen=0; v->selected=false; v->message_selected=false;
                return heltec_chat_view_poll(v,store,false,false,lines);
            }
            v->screen=2; v->action=0;
        } else if(v->screen==4) {
            if(v->reply==7) v->screen=2;
            else { v->screen=5; v->action=0; }
        } else if(v->screen==5) {
            if(v->action && chat && v->send_reply) {
                memset(v->reply_error,0,sizeof(v->reply_error));
                v->error=v->send_reply(v->reply_context,v->sender,replies[v->reply]);
                v->reply_queued=v->error==RNS_OK;
                v->screen=1;
            } else v->screen=4;
        } else if(v->screen==6) {
            if(v->action && v->cancel_reply) v->error=v->cancel_reply(v->reply_context,v->message);
            v->screen=1;
        }
    }
    if(v->screen==0) {
        (void)snprintf(lines[0],22,"CHATS %u",(unsigned)count);
        size_t selected=v->back?count:index, first=selected/5U*5U;
        for(size_t i=first;i<=count && i<first+5U;++i) {
            if(i==count) (void)snprintf(lines[1U+i-first],22,"%c BACK",selected==i?'*':' ');
            else {
                const heltec_chat *c=heltec_chat_store_get(store,slots[i]); bool unread=false;
                for(size_t j=0;j<c->count;++j) unread|=c->messages[j].unread;
                char label[22]; heltec_peer_label(v->peer_name,v->reply_context,c->sender,label);
                (void)snprintf(lines[1U+i-first],22,"%c%c%.19s",selected==i?'*':' ',unread?'!':' ',label);
            }
        }
    } else if(v->screen==1 && chat) {
        heltec_peer_label(v->peer_name,v->reply_context,chat->sender,lines[0]);
        if(chat->count) {
            const heltec_chat_message *m=&chat->messages[mi];
            memcpy(v->message,m->id,32); v->message_selected=true;
            static const char *states[]={"VERIFIED INBOUND","QUEUED","AWAITING PROOF","DELIVERED","FAILED","CANCELLED"};
            (void)snprintf(lines[1],22,"%s",m->state<6?states[m->state]:"UNKNOWN STATE");
            if(v->delivery_line) (void)v->delivery_line(v->reply_context,m->id,lines[1]);
            size_t count_rows=wrapped(m,v->page,rows), pages=(count_rows+3U)/4U;
            if(v->page>=pages) { v->page=0; (void)wrapped(m,0,rows); }
            for(size_t i=0;i<4;++i) memcpy(lines[2+i],rows[i],22);
            /* All real counts fit two digits (at most 8 messages and 96
             * pages). Explicit ranges also let GCC prove the OLED bound. */
            (void)snprintf(lines[6],22,"MSG %u/%u PAGE %u/%u",
                ((unsigned)mi+1U)%100U,(unsigned)chat->count%100U,
                (v->page+1U)%100U,(unsigned)pages%100U);
            rns_status_t read_status=heltec_chat_store_mark_read(store,chat->sender,m->id);
            if(read_status!=RNS_OK) v->error=read_status;
        }
    } else if(v->screen==2) {
        const char *actions[]={"CHAT LIST","READ MESSAGES",v->send_reply?"QUICK REPLY":"REPLY UNAVAILABLE","DELETE CHAT","CANCEL REPLY"};
        memcpy(lines[0],"CHAT ACTIONS",13);
        for(size_t i=0;i<5;++i) (void)snprintf(lines[i+1],22,"%c %s",v->action==i?'*':' ',actions[i]);
        (void)snprintf(lines[6],22,"ID %02X%02X%02X%02X",v->sender[12],v->sender[13],v->sender[14],v->sender[15]);
    } else if(v->screen==4) {
        (void)snprintf(lines[0],22,"QUICK REPLIES");
        size_t first=v->reply/5U*5U;
        for(size_t i=first;i<8 && i<first+5U;++i)
            (void)snprintf(lines[1+i-first],22,"%c %s",i==v->reply?'*':' ',replies[i]);
    } else if(v->screen==5) {
        (void)snprintf(lines[0],22,"SEND THIS REPLY?");
        (void)snprintf(lines[1],22,"%s",replies[v->reply]);
        (void)snprintf(lines[3],22,"%c CANCEL",v->action?' ':'*');
        (void)snprintf(lines[4],22,"%c SEND",v->action?'*':' ');
        if(v->reply==1) (void)snprintf(lines[5],22,"NOT EMERGENCY SERVICE");
    } else if(v->screen==6) {
        (void)snprintf(lines[0],22,"CANCEL THIS REPLY?");
        (void)snprintf(lines[2],22,"%c KEEP SENDING",v->action?' ':'*');
        (void)snprintf(lines[3],22,"%c CANCEL RETRIES",v->action?'*':' ');
        (void)snprintf(lines[5],22,"QUEUED RF MAY SEND");
    } else {
        (void)snprintf(lines[0],22,"DELETE THIS CHAT?");
        (void)snprintf(lines[2],22,"%c CANCEL",v->action==0?'*':' ');
        (void)snprintf(lines[3],22,"%c DELETE",v->action==1?'*':' ');
    }
    if(v->reply_queued) (void)snprintf(lines[6],22,"REPLY QUEUED");
    if(v->error==RNS_ERROR_NOT_FOUND) (void)snprintf(lines[6],22,"PEER INFO NEEDED");
    else if(v->error==RNS_ERROR_UNSUPPORTED) (void)snprintf(lines[6],22,"%.21s",v->reply_error[0]?v->reply_error:"REPLY UNSUPPORTED");
    else if(v->error!=RNS_OK) (void)snprintf(lines[6],22,"ACTION ERROR %d",(int)v->error);
    memcpy(lines[7],"TAP NEXT HOLD OPEN",19);
    return false;
}
