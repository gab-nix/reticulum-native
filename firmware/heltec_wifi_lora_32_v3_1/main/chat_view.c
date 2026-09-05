/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "chat_view.h"
#include <stdio.h>
#include <string.h>
static const char *replies[]={"I'm okay","SOS","I'll be there","Almost there","Yes","No","Thank you","Cancel"};
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
    if(v->screen==1 && next && chat && chat->count) mi=(mi+1U)%chat->count;
    if(v->screen==2 && next) v->action=(v->action+1U)%5U;
    if(v->screen==3 && next) v->action=1U-v->action;
    if(v->screen==4 && next) v->reply=(v->reply+1U)%8U;
    if(v->screen==5 && next) v->action=1U-v->action;
    if(v->screen==6 && next) v->action=1U-v->action;
    if(select) {
        if(v->screen==0) { if(v->back || !chat) return true; v->screen=1; }
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
                const uint8_t *a=heltec_chat_store_get(store,slots[i])->sender;
                (void)snprintf(lines[1U+i-first],22,"%c %02X%02X%02X%02X",selected==i?'*':' ',a[12],a[13],a[14],a[15]);
            }
        }
    } else if(v->screen==1 && chat) {
        (void)snprintf(lines[0],22,"MESSAGE %u OF %u",(unsigned)(chat->count?mi+1U:0U),chat->count);
        if(chat->count) {
            const heltec_chat_message *m=&chat->messages[mi];
            memcpy(v->message,m->id,32); v->message_selected=true;
            static const char *states[]={"VERIFIED INBOUND","QUEUED","AWAITING PROOF","DELIVERED","FAILED","CANCELLED"};
            (void)snprintf(lines[1],22,"%s",m->state<6?states[m->state]:"UNKNOWN STATE");
            if(v->delivery_line) (void)v->delivery_line(v->reply_context,m->id,lines[1]);
            size_t out=0;
            for(size_t i=0;i<m->length && out<105U;++i) {
                uint8_t c=m->text[i]; if((c&0xc0U)==0x80U) continue;
                lines[2U+out/21U][out%21U]=c>=32 && c<127?(char)c:c<32?' ':'?'; ++out;
            }
            if(m->length>105U) memcpy(lines[6]+18,"...",3);
        }
    } else if(v->screen==2) {
        const char *actions[]={"CHAT LIST","READ MESSAGES",v->send_reply?"QUICK REPLY":"REPLY UNAVAILABLE","DELETE CHAT","CANCEL REPLY"};
        memcpy(lines[0],"CHAT ACTIONS",13);
        for(size_t i=0;i<5;++i) (void)snprintf(lines[i+1],22,"%c %s",v->action==i?'*':' ',actions[i]);
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
    if(v->error==RNS_ERROR_NOT_FOUND) (void)snprintf(lines[6],22,"PEER MUST ANNOUNCE");
    else if(v->error==RNS_ERROR_UNSUPPORTED) (void)snprintf(lines[6],22,"%.21s",v->reply_error[0]?v->reply_error:"REPLY UNSUPPORTED");
    else if(v->error!=RNS_OK) (void)snprintf(lines[6],22,"ACTION ERROR %d",(int)v->error);
    memcpy(lines[7],"TAP NEXT HOLD OPEN",19);
    return false;
}
