#include "reticulum/lxmf_propagation.h"

#include <math.h>
#include <string.h>

typedef struct {
  const uint8_t *p, *end;
  size_t budget;
} reader;
typedef struct {
  uint8_t *p;
  size_t left;
} writer;

static bool take(reader *r, size_t n, const uint8_t **p) {
  if (n > (size_t)(r->end - r->p))
    return false;
  *p = r->p;
  r->p += n;
  return true;
}
static bool unsigned_bytes(reader *r, size_t n, uint64_t *v) {
  const uint8_t *p;
  if (!take(r, n, &p))
    return false;
  *v = 0;
  for (size_t i = 0; i < n; i++)
    *v = (*v << 8) | p[i];
  return true;
}
static bool count(reader *r, bool map, size_t *n) {
  uint64_t c;
  if (!unsigned_bytes(r, 1, &c))
    return false;
  if ((c & 0xf0u) == (map ? 0x80u : 0x90u))
    *n = (size_t)(c & 15u);
  else if (c == (map ? 0xdeu : 0xdcu) || c == (map ? 0xdfu : 0xddu)) {
    if (!unsigned_bytes(r, c == (map ? 0xdeu : 0xdcu) ? 2 : 4, &c) ||
        c > LXMF_PN_MAX_WIRE)
      return false;
    *n = (size_t)c;
  } else
    return false;
  return true;
}
static bool number(reader *r, lxmf_pn_number_t *n) {
  uint64_t c, v;
  if (!unsigned_bytes(r, 1, &c))
    return false;
  memset(n, 0, sizeof *n);
  if (c <= 0x7fu) {
    n->integer = c;
    return true;
  }
  if (c >= 0xccu && c <= 0xcfu) {
    if (!unsigned_bytes(r, (size_t)1u << (c - 0xccu), &v))
      return false;
    n->integer = v;
    return true;
  }
  if (c >= 0xd0u && c <= 0xd3u) {
    size_t width = c == 0xd0u ? 1u : c == 0xd1u ? 2u : c == 0xd2u ? 4u : 8u;
    if (r->p == r->end || (*r->p & 0x80u) || !unsigned_bytes(r, width, &v))
      return false;
    n->integer = v;
    return true;
  }
  if (c == 0xcbu) {
    if (!unsigned_bytes(r, 8, &v))
      return false;
    memcpy(&n->real, &v, 8);
  } else if (c == 0xcau) {
    if (!unsigned_bytes(r, 4, &v))
      return false;
    uint32_t bits = (uint32_t)v;
    float f;
    memcpy(&f, &bits, 4);
    n->real = f;
  } else
    return false;
  n->is_float = true;
  return isfinite(n->real) && n->real >= 0.0;
}
static bool uint_value(reader *r, uint64_t *v) {
  lxmf_pn_number_t n;
  if (!number(r, &n) || n.is_float)
    return false;
  *v = n.integer;
  return true;
}
static bool boolean(reader *r, bool *v) {
  uint64_t c;
  if (!unsigned_bytes(r, 1, &c) || (c != 0xc2u && c != 0xc3u))
    return false;
  *v = c == 0xc3u;
  return true;
}
static bool binary(reader *r, lxmf_slice_t *s) {
  uint64_t c, n;
  if (!unsigned_bytes(r, 1, &c))
    return false;
  if (c < 0xc4u || c > 0xc6u ||
      !unsigned_bytes(r, (size_t)1u << (c - 0xc4u), &n) || n > LXMF_PN_MAX_WIRE)
    return false;
  s->len = (size_t)n;
  return take(r, s->len, &s->data);
}
/* Bounded generic object walk retains unknown metadata and outer extensions. */
static bool skip(reader *r, unsigned depth) {
  if (depth > 16u || !r->budget || r->p == r->end)
    return false;
  r->budget--;
  uint8_t c = *r->p++;
  uint64_t bytes = 0, children = 0, v;
  if (c <= 0x7fu || c >= 0xe0u || c == 0xc0u || c == 0xc2u || c == 0xc3u)
    return true;
  if ((c & 0xe0u) == 0xa0u)
    bytes = c & 31u;
  else if ((c & 0xf0u) == 0x90u)
    children = c & 15u;
  else if ((c & 0xf0u) == 0x80u)
    children = 2u * (c & 15u);
  else if (c == 0xc4u || c == 0xd9u || c == 0xc7u) {
    if (!unsigned_bytes(r, 1, &bytes))
      return false;
    if (c == 0xc7u)
      bytes++;
  } else if (c == 0xc5u || c == 0xdau || c == 0xc8u) {
    if (!unsigned_bytes(r, 2, &bytes))
      return false;
    if (c == 0xc8u)
      bytes++;
  } else if (c == 0xc6u || c == 0xdbu || c == 0xc9u) {
    if (!unsigned_bytes(r, 4, &bytes))
      return false;
    if (c == 0xc9u)
      bytes++;
  } else if (c == 0xdcu || c == 0xddu || c == 0xdeu || c == 0xdfu) {
    if (!unsigned_bytes(r, (c == 0xdcu || c == 0xdeu) ? 2 : 4, &v))
      return false;
    children = (c >= 0xdeu) ? 2u * v : v;
  } else if (c >= 0xccu && c <= 0xcfu)
    bytes = (uint64_t)1u << (c - 0xccu);
  else if (c >= 0xd0u && c <= 0xd3u)
    bytes = (uint64_t)1u << (c - 0xd0u);
  else if (c >= 0xd4u && c <= 0xd8u)
    bytes = ((uint64_t)1u << (c - 0xd4u)) + 1u;
  else if (c == 0xcau)
    bytes = 4;
  else if (c == 0xcbu)
    bytes = 8;
  else
    return false;
  if (bytes > (uint64_t)(r->end - r->p) || children > r->budget)
    return false;
  r->p += (size_t)bytes;
  for (uint64_t i = 0; i < children; i++)
    if (!skip(r, depth + 1u))
      return false;
  return true;
}
static bool objects(lxmf_slice_t s, size_t n, bool map) {
  if (!s.data || !s.len || s.len > LXMF_PN_MAX_ANNOUNCE)
    return false;
  reader r = {s.data, s.data + s.len, 4096};
  if (map) {
    reader head = r;
    size_t pairs;
    if (!count(&head, true, &pairs))
      return false;
  }
  for (size_t i = 0; i < n; i++)
    if (!skip(&r, 0))
      return false;
  return r.p == r.end;
}
static bool put(writer *w, const void *p, size_t n) {
  if (n > w->left)
    return false;
  if (n)
    memcpy(w->p, p, n);
  w->p += n;
  w->left -= n;
  return true;
}
static bool byte(writer *w, uint8_t v) { return put(w, &v, 1); }
static bool integer_bytes(writer *w, uint64_t v, size_t n) {
  uint8_t b[8];
  for (size_t i = 0; i < n; i++)
    b[n - i - 1u] = (uint8_t)(v >> (i * 8u));
  return put(w, b, n);
}
static bool integer(writer *w, uint64_t v) {
  if (v <= 127u)
    return byte(w, (uint8_t)v);
  size_t n = v <= UINT8_MAX    ? 1u
             : v <= UINT16_MAX ? 2u
             : v <= UINT32_MAX ? 4u
                               : 8u;
  uint8_t code = n == 1u ? 0xccu : n == 2u ? 0xcdu : n == 4u ? 0xceu : 0xcfu;
  return byte(w, code) && integer_bytes(w, v, n);
}
static bool real(writer *w, double v) {
  uint64_t bits;
  memcpy(&bits, &v, 8);
  return byte(w, 0xcbu) && integer_bytes(w, bits, 8);
}
static bool put_number(writer *w, lxmf_pn_number_t n) {
  return n.is_float ? real(w, n.real) : integer(w, n.integer);
}
static bool valid_number(lxmf_pn_number_t n) {
  return !n.is_float || (isfinite(n.real) && n.real >= 0.0);
}
static bool array(writer *w, size_t n) {
  return n < 16u ? byte(w, (uint8_t)(0x90u + n))
                 : byte(w, 0xdcu) && integer_bytes(w, n, 2);
}
static bool put_binary(writer *w, lxmf_slice_t s) {
  size_t n = s.len <= UINT8_MAX ? 1u : s.len <= UINT16_MAX ? 2u : 4u;
  return byte(w, n == 1u   ? 0xc4u
                 : n == 2u ? 0xc5u
                           : 0xc6u) &&
         integer_bytes(w, s.len, n) && put(w, s.data, s.len);
}
static bool slices_valid(const lxmf_slice_t *s, size_t n, bool ids) {
  if (n > LXMF_PN_MAX_ITEMS)
    return false;
  size_t total = 0;
  for (size_t i = 0; i < n; i++) {
    if (!s[i].data || !s[i].len || s[i].len > LXMF_PN_MAX_WIRE - total ||
        (ids && s[i].len != 32u))
      return false;
    total += s[i].len;
  }
  return true;
}
static bool put_slices(writer *w, const lxmf_slice_t *s, size_t n) {
  if (!array(w, n))
    return false;
  for (size_t i = 0; i < n; i++)
    if (!put_binary(w, s[i]))
      return false;
  return true;
}
static bool get_slices(reader *r, lxmf_slice_t *s, size_t *n, bool ids) {
  if (!count(r, false, n) || *n > LXMF_PN_MAX_ITEMS)
    return false;
  for (size_t i = 0; i < *n; i++)
    if (!binary(r, &s[i]) || !s[i].len || (ids && s[i].len != 32u))
      return false;
  return true;
}
static lxmf_status_t finish(writer w, size_t capacity, size_t *length, bool ok,
                            size_t max) {
  if (!ok || capacity - w.left > max)
    return LXMF_ERR_BOUNDS;
  *length = capacity - w.left;
  return LXMF_OK;
}

lxmf_status_t lxmf_pn_announce_encode(const lxmf_pn_announce_t *a, uint8_t *out,
                                      size_t cap, size_t *len) {
  if (!a || !out || !len)
    return LXMF_ERR_ARGUMENT;
  if (!valid_number(a->transfer_limit_kb) || !valid_number(a->sync_limit_kb) ||
      a->extension_count > LXMF_PN_MAX_ITEMS - 7u ||
      (a->metadata_msgpack.len && !objects(a->metadata_msgpack, 1, true)) ||
      (a->extension_count
           ? !objects(a->extensions_msgpack, a->extension_count, false)
           : a->extensions_msgpack.len != 0))
    return LXMF_ERR_FORMAT;
  writer w = {out, cap};
  bool ok = array(&w, 7u + a->extension_count) &&
            byte(&w, a->legacy_support ? 0xc3u : 0xc2u) &&
            integer(&w, a->timebase) && byte(&w, a->enabled ? 0xc3u : 0xc2u) &&
            put_number(&w, a->transfer_limit_kb) &&
            put_number(&w, a->sync_limit_kb) && array(&w, 3) &&
            integer(&w, a->stamp_cost) && integer(&w, a->stamp_flexibility) &&
            integer(&w, a->peering_cost) &&
            (a->metadata_msgpack.len
                 ? put(&w, a->metadata_msgpack.data, a->metadata_msgpack.len)
                 : byte(&w, 0x80)) &&
            put(&w, a->extensions_msgpack.data, a->extensions_msgpack.len);
  return finish(w, cap, len, ok, LXMF_PN_MAX_ANNOUNCE);
}
lxmf_status_t lxmf_pn_announce_decode(const uint8_t *in, size_t len,
                                      lxmf_pn_announce_t *a) {
  if (!in || !a)
    return LXMF_ERR_ARGUMENT;
  if (!len || len > LXMF_PN_MAX_ANNOUNCE)
    return LXMF_ERR_BOUNDS;
  reader r = {in, in + len, 4096};
  lxmf_pn_announce_t tmp = {0};
  size_t n, c;
  uint64_t costs[3];
  if (!count(&r, false, &n) || n < 7u || n > LXMF_PN_MAX_ITEMS ||
      !boolean(&r, &tmp.legacy_support) || !uint_value(&r, &tmp.timebase) ||
      !boolean(&r, &tmp.enabled) || !number(&r, &tmp.transfer_limit_kb) ||
      !number(&r, &tmp.sync_limit_kb) || !count(&r, false, &c) || c != 3u)
    return LXMF_ERR_FORMAT;
  for (size_t i = 0; i < 3; i++)
    if (!uint_value(&r, &costs[i]) || costs[i] > 255u)
      return LXMF_ERR_FORMAT;
  tmp.stamp_cost = (uint8_t)costs[0];
  tmp.stamp_flexibility = (uint8_t)costs[1];
  tmp.peering_cost = (uint8_t)costs[2];
  tmp.metadata_msgpack.data = r.p;
  reader head = r;
  if (!count(&head, true, &c) || !skip(&r, 0))
    return LXMF_ERR_FORMAT;
  tmp.metadata_msgpack.len = (size_t)(r.p - tmp.metadata_msgpack.data);
  tmp.extension_count = n - 7u;
  tmp.extensions_msgpack.data = r.p;
  for (size_t i = 0; i < tmp.extension_count; i++)
    if (!skip(&r, 0))
      return LXMF_ERR_FORMAT;
  tmp.extensions_msgpack.len = (size_t)(r.p - tmp.extensions_msgpack.data);
  if (r.p != r.end)
    return LXMF_ERR_FORMAT;
  *a = tmp;
  return LXMF_OK;
}

bool lxmf_pn_announce_name(const lxmf_pn_announce_t *announce,
                           lxmf_slice_t *name) {
  if (announce == NULL || name == NULL ||
      announce->metadata_msgpack.data == NULL ||
      announce->metadata_msgpack.len == 0U)
    return false;
  reader r = {announce->metadata_msgpack.data,
              announce->metadata_msgpack.data + announce->metadata_msgpack.len,
              4096U};
  size_t pairs = 0U;
  if (!count(&r, true, &pairs))
    return false;
  bool found = false;
  lxmf_slice_t result = {0};
  for (size_t i = 0U; i < pairs; ++i) {
    reader numeric = r;
    uint64_t key = UINT64_MAX;
    if (uint_value(&numeric, &key))
      r = numeric;
    else if (!skip(&r, 0U))
      return false;
    if (key == 1U) {
      if (found || !binary(&r, &result))
        return false;
      found = true;
    } else if (!skip(&r, 0U))
      return false;
  }
  if (!found || r.p != r.end)
    return false;
  *name = result;
  return true;
}
lxmf_status_t lxmf_pn_upload_encode(const lxmf_pn_upload_t *u, uint8_t *out,
                                    size_t cap, size_t *len) {
  if (!u || !out || !len)
    return LXMF_ERR_ARGUMENT;
  if (!isfinite(u->timebase) || u->timebase < 0.0 ||
      !slices_valid(u->messages, u->count, false))
    return LXMF_ERR_FORMAT;
  writer w = {out, cap};
  bool ok = array(&w, 2) && real(&w, u->timebase) &&
            put_slices(&w, u->messages, u->count);
  return finish(w, cap, len, ok, LXMF_PN_MAX_WIRE);
}
lxmf_status_t lxmf_pn_upload_decode(const uint8_t *in, size_t len,
                                    lxmf_pn_upload_t *u) {
  if (!in || !u)
    return LXMF_ERR_ARGUMENT;
  if (!len || len > LXMF_PN_MAX_WIRE)
    return LXMF_ERR_BOUNDS;
  reader r = {in, in + len, 4096};
  size_t n;
  lxmf_pn_number_t timestamp;
  lxmf_pn_upload_t tmp = {0};
  if (!count(&r, false, &n) || n != 2u || !number(&r, &timestamp) ||
      !get_slices(&r, tmp.messages, &tmp.count, false) || r.p != r.end)
    return LXMF_ERR_FORMAT;
  tmp.timebase =
      timestamp.is_float ? timestamp.real : (double)timestamp.integer;
  *u = tmp;
  return LXMF_OK;
}
lxmf_status_t lxmf_pn_upload_rejection_encode(uint8_t *out, size_t cap,
                                              size_t *len) {
  if (!out || !len)
    return LXMF_ERR_ARGUMENT;
  writer w = {out, cap};
  bool ok = array(&w, 1) && integer(&w, LXMF_PN_ERROR_INVALID_STAMP);
  return finish(w, cap, len, ok, LXMF_PN_MAX_WIRE);
}
lxmf_status_t lxmf_pn_upload_rejection_decode(const uint8_t *in, size_t len) {
  if (!in)
    return LXMF_ERR_ARGUMENT;
  if (!len || len > LXMF_PN_MAX_WIRE)
    return LXMF_ERR_BOUNDS;
  reader r = {in, in + len, 4096};
  size_t n;
  uint64_t v;
  return count(&r, false, &n) && n == 1u && uint_value(&r, &v) &&
                 v == LXMF_PN_ERROR_INVALID_STAMP && r.p == r.end
             ? LXMF_OK
             : LXMF_ERR_FORMAT;
}
lxmf_status_t lxmf_pn_get_request_encode(const lxmf_pn_get_request_t *q,
                                         uint8_t *out, size_t cap,
                                         size_t *len) {
  if (!q || !out || !len)
    return LXMF_ERR_ARGUMENT;
  if (!slices_valid(q->wants, q->wants_count, true) ||
      !slices_valid(q->haves, q->haves_count, true) ||
      (q->wants_null && q->wants_count) || (q->haves_null && q->haves_count) ||
      (q->has_limit && !valid_number(q->limit_kb)))
    return LXMF_ERR_FORMAT;
  writer w = {out, cap};
  bool ok = array(&w, q->has_limit ? 3 : 2) &&
            (q->wants_null ? byte(&w, 0xc0)
                           : put_slices(&w, q->wants, q->wants_count)) &&
            (q->haves_null ? byte(&w, 0xc0)
                           : put_slices(&w, q->haves, q->haves_count)) &&
            (!q->has_limit || put_number(&w, q->limit_kb));
  return finish(w, cap, len, ok, LXMF_PN_MAX_WIRE);
}
static bool nullable_slices(reader *r, bool *nil, lxmf_slice_t *s, size_t *n) {
  if (r->p == r->end)
    return false;
  *nil = *r->p == 0xc0u;
  if (*nil) {
    r->p++;
    *n = 0;
    return true;
  }
  return get_slices(r, s, n, true);
}
lxmf_status_t lxmf_pn_get_request_decode(const uint8_t *in, size_t len,
                                         lxmf_pn_get_request_t *q) {
  if (!in || !q)
    return LXMF_ERR_ARGUMENT;
  if (!len || len > LXMF_PN_MAX_WIRE)
    return LXMF_ERR_BOUNDS;
  reader r = {in, in + len, 4096};
  lxmf_pn_get_request_t tmp = {0};
  size_t n;
  if (!count(&r, false, &n) || (n != 2 && n != 3) ||
      !nullable_slices(&r, &tmp.wants_null, tmp.wants, &tmp.wants_count) ||
      !nullable_slices(&r, &tmp.haves_null, tmp.haves, &tmp.haves_count))
    return LXMF_ERR_FORMAT;
  tmp.has_limit = n == 3u;
  if ((tmp.has_limit && !number(&r, &tmp.limit_kb)) || r.p != r.end)
    return LXMF_ERR_FORMAT;
  *q = tmp;
  return LXMF_OK;
}
static bool error_valid(uint8_t e) {
  return e == 0xf0u || e == 0xf1u || e == 0xf3u || e == 0xf4u || e == 0xf5u ||
         e == 0xf6u || e == 0xfdu || e == 0xfeu;
}
lxmf_status_t lxmf_pn_get_response_encode(const lxmf_pn_get_response_t *p,
                                          bool ids, uint8_t *out, size_t cap,
                                          size_t *len) {
  if (!p || !out || !len)
    return LXMF_ERR_ARGUMENT;
  if ((p->kind != LXMF_PN_RESPONSE_ITEMS && p->kind != LXMF_PN_RESPONSE_NIL &&
       p->kind != LXMF_PN_RESPONSE_ERROR) ||
      (p->kind != LXMF_PN_RESPONSE_ITEMS && p->count) ||
      (p->kind == LXMF_PN_RESPONSE_ERROR && !error_valid(p->error)) ||
      (p->kind == LXMF_PN_RESPONSE_ITEMS &&
       !slices_valid(p->items, p->count, ids)))
    return LXMF_ERR_FORMAT;
  writer w = {out, cap};
  bool ok = p->kind == LXMF_PN_RESPONSE_NIL ? byte(&w, 0xc0)
            : p->kind == LXMF_PN_RESPONSE_ERROR
                ? integer(&w, p->error)
                : put_slices(&w, p->items, p->count);
  return finish(w, cap, len, ok, LXMF_PN_MAX_WIRE);
}
lxmf_status_t lxmf_pn_get_response_decode(const uint8_t *in, size_t len,
                                          bool ids, lxmf_pn_get_response_t *p) {
  if (!in || !p)
    return LXMF_ERR_ARGUMENT;
  if (!len || len > LXMF_PN_MAX_WIRE)
    return LXMF_ERR_BOUNDS;
  reader r = {in, in + len, 4096};
  lxmf_pn_get_response_t tmp = {0};
  if (*r.p == 0xc0u) {
    r.p++;
    tmp.kind = LXMF_PN_RESPONSE_NIL;
  } else if ((*r.p & 0xf0u) == 0x90u || *r.p == 0xdcu || *r.p == 0xddu) {
    if (!get_slices(&r, tmp.items, &tmp.count, ids))
      return LXMF_ERR_FORMAT;
  } else {
    uint64_t e;
    if (!uint_value(&r, &e) || e > 255u || !error_valid((uint8_t)e))
      return LXMF_ERR_FORMAT;
    tmp.kind = LXMF_PN_RESPONSE_ERROR;
    tmp.error = (uint8_t)e;
  }
  if (r.p != r.end)
    return LXMF_ERR_FORMAT;
  *p = tmp;
  return LXMF_OK;
}
