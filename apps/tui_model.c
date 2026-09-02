#include "tui_model.h"

#include <string.h>

static bool same(const uint8_t *a, const uint8_t *b) {
    return memcmp(a, b, LXMF_DESTINATION_LENGTH) == 0;
}

static bool utf8_sequence(const uint8_t *s, size_t n, size_t *width) {
    uint32_t cp;
    size_t w;
    if (!n) return false;
    if (s[0] < 0x80) { *width = 1; return true; }
    if (s[0] >= 0xc2 && s[0] <= 0xdf) { w = 2; cp = s[0] & 0x1fu; }
    else if (s[0] >= 0xe0 && s[0] <= 0xef) { w = 3; cp = s[0] & 0x0fu; }
    else if (s[0] >= 0xf0 && s[0] <= 0xf4) { w = 4; cp = s[0] & 0x07u; }
    else return false;
    if (n < w) return false;
    for (size_t i = 1; i < w; ++i) {
        if ((s[i] & 0xc0u) != 0x80u) return false;
        cp = (cp << 6) | (s[i] & 0x3fu);
    }
    if ((w == 2 && cp < 0x80) || (w == 3 && cp < 0x800) ||
        (w == 4 && cp < 0x10000) || cp > 0x10ffff ||
        (cp >= 0xd800 && cp <= 0xdfff)) return false;
    *width = w;
    return true;
}

static bool valid_utf8(const uint8_t *s, size_t n) {
    for (size_t p = 0, w; p < n; p += w)
        if (!utf8_sequence(s + p, n - p, &w)) return false;
    return true;
}

static size_t conversation_find(const tui_model_t *m, const uint8_t peer[16]) {
    for (size_t i = 0; i < m->conversation_count; ++i)
        if (same(m->conversations[i].peer, peer)) return i;
    return m->conversation_count;
}

static void sort_conversations(tui_model_t *m, const uint8_t selected_peer[16],
                               bool retain_selection) {
    for (size_t i = 1; i < m->conversation_count; ++i) {
        tui_conversation_t v = m->conversations[i];
        size_t j = i;
        while (j && (v.unread_count > m->conversations[j-1].unread_count ||
               (v.unread_count == m->conversations[j-1].unread_count &&
                v.latest_timestamp > m->conversations[j-1].latest_timestamp))) {
            m->conversations[j] = m->conversations[j-1]; --j;
        }
        m->conversations[j] = v;
    }
    if (retain_selection)
        m->selected = conversation_find(m, selected_peer);
    if (m->selected >= m->conversation_count) m->selected = 0;
}

static lxmf_status_t append(tui_model_t *m, const lxmf_store_message_t *src) {
    bool outgoing = same(src->source, m->local);
    bool incoming = same(src->destination, m->local);
    if (!outgoing && !incoming) return LXMF_OK;
    if (m->message_count >= TUI_MODEL_MAX_MESSAGES ||
        src->content.len > TUI_MODEL_MAX_CONTENT) return LXMF_ERR_BOUNDS;
    const uint8_t *peer = outgoing ? src->destination : src->source;
    size_t ci = conversation_find(m, peer);
    if (ci == m->conversation_count) {
        if (ci >= TUI_MODEL_MAX_CONVERSATIONS) return LXMF_ERR_BOUNDS;
        memset(&m->conversations[ci], 0, sizeof m->conversations[ci]);
        memcpy(m->conversations[ci].peer, peer, 16);
        ++m->conversation_count;
    }
    tui_message_t *dst = &m->messages[m->message_count++];
    memset(dst, 0, sizeof *dst);
    memcpy(dst->message_id, src->message_id, sizeof dst->message_id);
    memcpy(dst->peer, peer, sizeof dst->peer);
    dst->timestamp = src->timestamp;
    dst->status = src->status;
    dst->outgoing = outgoing;
    dst->unread = incoming;
    dst->content_len = src->content.len;
    if (src->content.len) memcpy(dst->content, src->content.data, src->content.len);
    tui_conversation_t *c = &m->conversations[ci];
    ++c->message_count;
    if (incoming) ++c->unread_count;
    if (c->message_count == 1 || src->timestamp > c->latest_timestamp) {
        c->latest_timestamp = src->timestamp;
        c->latest_status = src->status;
    }
    return LXMF_OK;
}

typedef struct { tui_model_t *model; lxmf_status_t status; } load_context;
static bool load_one(void *context, const lxmf_store_message_t *message) {
    load_context *c = context;
    c->status = append(c->model, message);
    return c->status == LXMF_OK;
}

void tui_model_init(tui_model_t *m, const uint8_t local[16]) {
    if (!m) return;
    memset(m, 0, sizeof *m);
    if (local) memcpy(m->local, local, 16);
}

lxmf_status_t tui_model_load(tui_model_t *m, lxmf_store_t *store) {
    if (!m || !store) return LXMF_ERR_ARGUMENT;
    uint8_t local[16]; memcpy(local, m->local, 16);
    tui_model_init(m, local);
    load_context c = {m, LXMF_OK};
    lxmf_status_t st = lxmf_store_list(store, load_one, &c);
    if (st != LXMF_OK) return st;
    if (c.status != LXMF_OK) return c.status;
    sort_conversations(m, NULL, false);
    return LXMF_OK;
}

size_t tui_model_conversation_count(const tui_model_t *m) { return m ? m->conversation_count : 0; }
const tui_conversation_t *tui_model_conversation(const tui_model_t *m, size_t i) { return m && i < m->conversation_count ? &m->conversations[i] : NULL; }
bool tui_model_select(tui_model_t *m, size_t i) { if (!m || i >= m->conversation_count) return false; m->selected=i; m->scroll=0; return true; }
size_t tui_model_selected(const tui_model_t *m) { return m ? m->selected : 0; }
bool tui_model_seed_conversation(tui_model_t *m,const uint8_t peer[16]) {
    if(!m||!peer)return false;
    size_t i=conversation_find(m,peer);
    if(i==m->conversation_count){if(i>=TUI_MODEL_MAX_CONVERSATIONS)return false;memset(&m->conversations[i],0,sizeof m->conversations[i]);memcpy(m->conversations[i].peer,peer,16);++m->conversation_count;}
    m->selected=i;m->scroll=0;return true;
}
const uint8_t *tui_model_selected_peer(const tui_model_t *m){return m&&m->selected<m->conversation_count?m->conversations[m->selected].peer:NULL;}

void tui_model_mark_selected_read(tui_model_t *m) {
    if (!m || m->selected >= m->conversation_count) return;
    uint8_t peer[16]; memcpy(peer, m->conversations[m->selected].peer, 16);
    for (size_t i=0;i<m->message_count;++i) if (same(m->messages[i].peer,peer)) m->messages[i].unread=false;
    m->conversations[m->selected].unread_count=0;
    sort_conversations(m, peer, true);
}

size_t tui_model_selected_message_count(const tui_model_t *m) {
    if (!m || m->selected >= m->conversation_count) return 0;
    return m->conversations[m->selected].message_count;
}

const tui_message_t *tui_model_selected_message(const tui_model_t *m, size_t index) {
    if (!m || m->selected >= m->conversation_count) return NULL;
    const uint8_t *peer=m->conversations[m->selected].peer;
    for(size_t i=0;i<m->message_count;++i) if(same(m->messages[i].peer,peer)) {
        if (!index--) return &m->messages[i];
    }
    return NULL;
}
void tui_model_set_scroll(tui_model_t *m,size_t v){if(m)m->scroll=v;}
size_t tui_model_scroll(const tui_model_t *m){return m?m->scroll:0;}
const char *tui_model_composer(const tui_model_t *m){return m?m->composer:"";}
size_t tui_model_composer_len(const tui_model_t *m){return m?m->composer_len:0;}
size_t tui_model_composer_cursor(const tui_model_t *m){return m?m->composer_cursor:0;}

bool tui_model_composer_insert(tui_model_t *m,const char *s,size_t n){
    if(!m||(!s&&n)||m->composer_len+n>TUI_MODEL_COMPOSER_CAPACITY||!valid_utf8((const uint8_t*)s,n))return false;
    memmove(m->composer+m->composer_cursor+n,m->composer+m->composer_cursor,m->composer_len-m->composer_cursor+1);
    if(n)memcpy(m->composer+m->composer_cursor,s,n);
    m->composer_cursor+=n;m->composer_len+=n;return true;
}
bool tui_model_composer_left(tui_model_t *m){if(!m||!m->composer_cursor)return false;do{--m->composer_cursor;}while(m->composer_cursor&&((uint8_t)m->composer[m->composer_cursor]&0xc0u)==0x80u);return true;}
bool tui_model_composer_right(tui_model_t *m){if(!m||m->composer_cursor>=m->composer_len)return false;size_t w;if(!utf8_sequence((const uint8_t*)m->composer+m->composer_cursor,m->composer_len-m->composer_cursor,&w))return false;m->composer_cursor+=w;return true;}
bool tui_model_composer_backspace(tui_model_t *m){if(!m||!m->composer_cursor)return false;size_t end=m->composer_cursor;tui_model_composer_left(m);memmove(m->composer+m->composer_cursor,m->composer+end,m->composer_len-end+1);m->composer_len-=end-m->composer_cursor;return true;}
void tui_model_composer_clear(tui_model_t *m){if(m){m->composer[0]=0;m->composer_len=0;m->composer_cursor=0;}}

lxmf_status_t tui_model_submit(tui_model_t *m,tui_model_queue_fn queue,void *ctx){
    if(!m||!queue||m->selected>=m->conversation_count||!m->composer_len)return LXMF_ERR_ARGUMENT;
    uint8_t peer[16];memcpy(peer,m->conversations[m->selected].peer,16);
    lxmf_store_message_t out;memset(&out,0,sizeof out);
    lxmf_status_t st=queue(ctx,peer,(const uint8_t*)m->composer,m->composer_len,&out);
    if(st!=LXMF_OK)return st;
    if(!same(out.source,m->local)||!same(out.destination,peer))return LXMF_ERR_FORMAT;
    out.status=LXMF_DELIVERY_QUEUED;
    out.content=(lxmf_slice_t){(const uint8_t*)m->composer,m->composer_len};
    st=append(m,&out);if(st!=LXMF_OK)return st;
    sort_conversations(m,peer,true);tui_model_composer_clear(m);return LXMF_OK;
}
