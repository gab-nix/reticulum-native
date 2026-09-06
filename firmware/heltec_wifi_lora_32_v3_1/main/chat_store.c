/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "chat_store.h"
#include "reticulum/hal.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#define RECORD_SIZE (18U+HELTEC_CHAT_MESSAGES*(43U+HELTEC_CHAT_TEXT))
struct heltec_chat_store {
    rns_storage_t *storage;
    heltec_chat chats[HELTEC_CHAT_COUNT], scratch;
    uint8_t record[RECORD_SIZE];
    bool quarantined[HELTEC_CHAT_COUNT];
};
static void key_for(size_t slot, char key[8]) { (void)snprintf(key, 8, "chat%u", (unsigned)slot); }
static rns_status_t save(heltec_chat_store *s, size_t slot, const heltec_chat *c) {
    memset(s->record, 0, sizeof(s->record));
    s->record[0] = 2; s->record[1] = c->count;
    memcpy(s->record+2, c->sender, 16);
    size_t p = 18;
    for (size_t i = 0; i < c->count; ++i) {
        const heltec_chat_message *m = &c->messages[i];
        memcpy(s->record+p, m->id, 32); p += 32;
        for (unsigned b = 0; b < 8; ++b) s->record[p++] = (uint8_t)(m->timestamp >> (56U-b*8U));
        s->record[p++] = (uint8_t)(m->state | (m->unread ? 0x80U : 0U));
        s->record[p++] = (uint8_t)(m->length >> 8); s->record[p++] = (uint8_t)m->length;
        memcpy(s->record+p, m->text, m->length); p += HELTEC_CHAT_TEXT;
    }
    char key[8]; key_for(slot, key);
    return rns_storage_write_atomic(s->storage, key, s->record, sizeof(s->record));
}
static bool decode(heltec_chat_store *s, heltec_chat *c) {
    if ((s->record[0] != 1 && s->record[0] != 2) || s->record[1] > HELTEC_CHAT_MESSAGES) return false;
    c->used = true; c->count = s->record[1]; memcpy(c->sender, s->record+2, 16);
    size_t p = 18;
    for (size_t i = 0; i < c->count; ++i) {
        heltec_chat_message *m = &c->messages[i];
        memcpy(m->id, s->record+p, 32); p += 32;
        for (unsigned b = 0; b < 8; ++b) m->timestamp = (m->timestamp << 8)|s->record[p++];
        m->state = s->record[p++];
        if (s->record[0] == 2) { m->unread=(m->state&0x80U)!=0; m->state&=0x7fU; }
        m->length = (uint16_t)((unsigned)s->record[p]*256U+s->record[p+1]); p += 2;
        if (m->length > HELTEC_CHAT_TEXT || m->state > 5 || (m->unread && m->state)) return false;
        memcpy(m->text, s->record+p, m->length); p += HELTEC_CHAT_TEXT;
    }
    return true;
}
rns_status_t heltec_chat_store_open(rns_storage_t *storage, heltec_chat_store **out) {
    if (!storage || !out) return RNS_ERROR_INVALID_ARGUMENT;
    *out = NULL;
    heltec_chat_store *s = calloc(1, sizeof(*s));
    if (!s) return RNS_ERROR_NO_MEMORY;
    s->storage = storage;
    for (size_t i = 0; i < HELTEC_CHAT_COUNT; ++i) {
        char key[8]; size_t n = 0; key_for(i, key);
        rns_status_t status = rns_storage_read(storage, key, s->record, sizeof(s->record), &n);
        if (status == RNS_ERROR_NOT_FOUND) continue;
        if (status == RNS_ERROR_QUARANTINED) { s->quarantined[i]=true; continue; }
        if (status != RNS_OK || n != sizeof(s->record) || !decode(s, &s->chats[i])) {
            heltec_chat_store_close(s); return status == RNS_OK ? RNS_ERROR_PROTOCOL : status;
        }
        for (size_t j = 0; j < i; ++j) if (s->chats[j].used && !memcmp(s->chats[j].sender, s->chats[i].sender, 16)) {
            heltec_chat_store_close(s); return RNS_ERROR_PROTOCOL;
        }
    }
    *out = s; return RNS_OK;
}
void heltec_chat_store_close(heltec_chat_store *s) {
    if (s) { rns_hal_secure_zero(s, sizeof(*s)); free(s); }
}
const heltec_chat *heltec_chat_store_get(const heltec_chat_store *s, size_t slot) {
    return s && slot < HELTEC_CHAT_COUNT && s->chats[slot].used ? &s->chats[slot] : NULL;
}
static bool pending(const heltec_chat *c) {
    for (size_t i = 0; i < c->count; ++i) if (c->messages[i].state == 1 || c->messages[i].state == 2) return true;
    return false;
}
bool heltec_chat_can_rotate(const heltec_chat *c, size_t combined_count, bool verified) {
    if (!verified || !c || !c->used || c->count != HELTEC_CHAT_MESSAGES ||
        combined_count != c->count) return false;
    for (size_t i = 0; i < c->count; ++i)
        if (c->messages[i].state != 1 && c->messages[i].state != 2) return true;
    return false;
}
rns_status_t heltec_chat_store_add(heltec_chat_store *s, const uint8_t sender[16], const heltec_chat_message *m) {
    if (!s || !sender || !m || m->length > HELTEC_CHAT_TEXT || m->state > 5 || (m->unread && m->state)) return RNS_ERROR_INVALID_ARGUMENT;
    size_t slot = HELTEC_CHAT_COUNT;
    for (size_t i = 0; i < HELTEC_CHAT_COUNT; ++i) if (s->chats[i].used && !memcmp(s->chats[i].sender, sender, 16)) { slot = i; break; }
    if (slot == HELTEC_CHAT_COUNT) {
        for (size_t i = 0; i < HELTEC_CHAT_COUNT; ++i) if (!s->quarantined[i] && !s->chats[i].used) { slot = i; break; }
    }
    if (slot == HELTEC_CHAT_COUNT) {
        for (size_t i = 0; i < HELTEC_CHAT_COUNT; ++i) if (!s->quarantined[i] && !pending(&s->chats[i]) &&
            (slot == HELTEC_CHAT_COUNT || s->chats[i].messages[0].timestamp < s->chats[slot].messages[0].timestamp)) slot = i;
    }
    if (slot == HELTEC_CHAT_COUNT) return RNS_ERROR_OVERFLOW;
    s->scratch = s->chats[slot];
    if (!s->scratch.used || memcmp(s->scratch.sender, sender, 16)) {
        memset(&s->scratch, 0, sizeof(s->scratch)); s->scratch.used = true; memcpy(s->scratch.sender, sender, 16);
    }
    for (size_t i = 0; i < s->scratch.count; ++i) if (!memcmp(s->scratch.messages[i].id, m->id, 32)) return RNS_OK;
    if (s->scratch.count == HELTEC_CHAT_MESSAGES) {
        size_t evict = HELTEC_CHAT_MESSAGES;
        for (size_t i = HELTEC_CHAT_MESSAGES; i-- > 0;) if (s->scratch.messages[i].state != 1 && s->scratch.messages[i].state != 2) { evict = i; break; }
        if (evict == HELTEC_CHAT_MESSAGES) return RNS_ERROR_OVERFLOW;
        memmove(&s->scratch.messages[evict], &s->scratch.messages[evict+1], (HELTEC_CHAT_MESSAGES-evict-1)*sizeof(*m));
        --s->scratch.count;
    }
    memmove(&s->scratch.messages[1], &s->scratch.messages[0], s->scratch.count*sizeof(*m));
    s->scratch.messages[0] = *m; ++s->scratch.count;
    rns_status_t result = save(s, slot, &s->scratch);
    if (result == RNS_OK) s->chats[slot] = s->scratch;
    return result;
}
rns_status_t heltec_chat_store_set_state(heltec_chat_store *s, const uint8_t sender[16], const uint8_t id[32], uint8_t state) {
    if (!s || !sender || !id || state < 1 || state > 5) return RNS_ERROR_INVALID_ARGUMENT;
    for (size_t slot = 0; slot < HELTEC_CHAT_COUNT; ++slot) {
        const heltec_chat *c = &s->chats[slot];
        if (!c->used || memcmp(c->sender, sender, 16)) continue;
        for (size_t i = 0; i < c->count; ++i) if (!memcmp(c->messages[i].id, id, 32)) {
            uint8_t old = c->messages[i].state;
            if (!old || (old >= 3 && old != state)) return RNS_ERROR_INVALID_STATE;
            if (old == state) return RNS_OK;
            s->scratch = *c; s->scratch.messages[i].state = state;
            rns_status_t result = save(s, slot, &s->scratch);
            if (result == RNS_OK) s->chats[slot] = s->scratch;
            return result;
        }
    }
    return RNS_ERROR_NOT_FOUND;
}
size_t heltec_chat_store_unread(const heltec_chat_store *s) {
    size_t count=0;
    if(s) for(size_t i=0;i<HELTEC_CHAT_COUNT;++i) if(s->chats[i].used)
        for(size_t j=0;j<s->chats[i].count;++j) count+=s->chats[i].messages[j].unread?1U:0U;
    return count;
}
rns_status_t heltec_chat_store_mark_read(heltec_chat_store *s,const uint8_t sender[16],const uint8_t id[32]) {
    if(!s || !sender || !id) return RNS_ERROR_INVALID_ARGUMENT;
    for(size_t slot=0;slot<HELTEC_CHAT_COUNT;++slot) {
        const heltec_chat *c=&s->chats[slot];
        if(!c->used || memcmp(c->sender,sender,16)) continue;
        for(size_t i=0;i<c->count;++i) if(!memcmp(c->messages[i].id,id,32)) {
            if(!c->messages[i].unread) return RNS_OK;
            s->scratch=*c; s->scratch.messages[i].unread=false;
            rns_status_t status=save(s,slot,&s->scratch);
            if(status==RNS_OK) s->chats[slot]=s->scratch;
            return status;
        }
    }
    return RNS_ERROR_NOT_FOUND;
}
rns_status_t heltec_chat_store_delete(heltec_chat_store *s, size_t slot) {
    if (!s || slot >= HELTEC_CHAT_COUNT) return RNS_ERROR_INVALID_ARGUMENT;
    if (pending(&s->chats[slot])) return RNS_ERROR_INVALID_STATE;
    char key[8]; key_for(slot, key);
    rns_status_t result = rns_storage_remove(s->storage, key);
    if (result == RNS_OK || result == RNS_ERROR_NOT_FOUND) { rns_hal_secure_zero(&s->chats[slot], sizeof(s->chats[slot])); return RNS_OK; }
    return result;
}
