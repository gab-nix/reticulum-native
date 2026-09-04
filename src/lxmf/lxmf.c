#include "reticulum/lxmf.h"

#include "reticulum/crypto.h"
#include "reticulum/hal.h"
#include <float.h>
#include <stdlib.h>
#include <string.h>

#define LXMF_HEADER_LENGTH                                                     \
  (LXMF_DESTINATION_LENGTH + LXMF_SOURCE_LENGTH + LXMF_SIGNATURE_LENGTH)
#define LXMF_MAX_NESTING 32u

typedef struct {
  uint8_t *p;
  size_t left;
} writer_t;
typedef struct {
  const uint8_t *p;
  const uint8_t *end;
} reader_t;

static uint32_t rotr32(uint32_t x, unsigned n) {
  return (x >> n) | (x << (32u - n));
}
static uint32_t load32be(const uint8_t *p) {
  return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
         ((uint32_t)p[2] << 8) | p[3];
}
static void store32be(uint8_t *p, uint32_t x) {
  p[0] = (uint8_t)(x >> 24);
  p[1] = (uint8_t)(x >> 16);
  p[2] = (uint8_t)(x >> 8);
  p[3] = (uint8_t)x;
}
static void store64be(uint8_t *p, uint64_t x) {
  for (unsigned i = 0; i < 8; i++)
    p[7 - i] = (uint8_t)(x >> (8 * i));
}

static const uint32_t sha_k[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1,
    0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
    0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
    0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
    0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
    0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
    0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
    0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
typedef struct {
  uint32_t h[8];
  uint8_t block[64];
  size_t used;
  uint64_t bits;
} sha_ctx;
static void sha_block(sha_ctx *c, const uint8_t *b) {
  uint32_t w[64], a, bv, d, e, f, g, h;
  uint32_t cc;
  for (unsigned i = 0; i < 16; i++)
    w[i] = load32be(b + 4 * i);
  for (unsigned i = 16; i < 64; i++) {
    uint32_t s0 =
        rotr32(w[i - 15], 7) ^ rotr32(w[i - 15], 18) ^ (w[i - 15] >> 3);
    uint32_t s1 =
        rotr32(w[i - 2], 17) ^ rotr32(w[i - 2], 19) ^ (w[i - 2] >> 10);
    w[i] = w[i - 16] + s0 + w[i - 7] + s1;
  }
  a = c->h[0];
  bv = c->h[1];
  cc = c->h[2];
  d = c->h[3];
  e = c->h[4];
  f = c->h[5];
  g = c->h[6];
  h = c->h[7];
  for (unsigned i = 0; i < 64; i++) {
    uint32_t s1 = rotr32(e, 6) ^ rotr32(e, 11) ^ rotr32(e, 25);
    uint32_t ch = (e & f) ^ ((~e) & g);
    uint32_t t1 = h + s1 + ch + sha_k[i] + w[i];
    uint32_t s0 = rotr32(a, 2) ^ rotr32(a, 13) ^ rotr32(a, 22);
    uint32_t maj = (a & bv) ^ (a & cc) ^ (bv & cc);
    uint32_t t2 = s0 + maj;
    h = g;
    g = f;
    f = e;
    e = d + t1;
    d = cc;
    cc = bv;
    bv = a;
    a = t1 + t2;
  }
  c->h[0] += a;
  c->h[1] += bv;
  c->h[2] += cc;
  c->h[3] += d;
  c->h[4] += e;
  c->h[5] += f;
  c->h[6] += g;
  c->h[7] += h;
}
static void sha_init(sha_ctx *c) {
  static const uint32_t iv[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372,
                                 0xa54ff53a, 0x510e527f, 0x9b05688c,
                                 0x1f83d9ab, 0x5be0cd19};
  memcpy(c->h, iv, sizeof iv);
  c->used = 0;
  c->bits = 0;
}
static void sha_update(sha_ctx *c, const uint8_t *p, size_t n) {
  while (n) {
    size_t take = 64 - c->used;
    if (take > n)
      take = n;
    memcpy(c->block + c->used, p, take);
    c->used += take;
    p += take;
    n -= take;
    c->bits += (uint64_t)take * 8;
    if (c->used == 64) {
      sha_block(c, c->block);
      c->used = 0;
    }
  }
}
static void sha_final(sha_ctx *c, uint8_t out[32]) {
  c->block[c->used++] = 0x80;
  if (c->used > 56) {
    memset(c->block + c->used, 0, 64 - c->used);
    sha_block(c, c->block);
    c->used = 0;
  }
  memset(c->block + c->used, 0, 56 - c->used);
  store64be(c->block + 56, c->bits);
  sha_block(c, c->block);
  for (unsigned i = 0; i < 8; i++)
    store32be(out + 4 * i, c->h[i]);
}
void lxmf_sha256(const uint8_t *data, size_t len, uint8_t digest[32]) {
  sha_ctx c;
  sha_init(&c);
  if (len)
    sha_update(&c, data, len);
  sha_final(&c, digest);
}

static bool put(writer_t *w, const void *p, size_t n) {
  if (n > w->left)
    return false;
  if (n != 0U)
    memcpy(w->p, p, n);
  w->p += n;
  w->left -= n;
  return true;
}
static bool put_u8(writer_t *w, uint8_t x) { return put(w, &x, 1); }
static bool put_bin(writer_t *w, lxmf_slice_t s) {
  if (s.len && !s.data)
    return false;
  if (s.len <= UINT8_MAX) {
    uint8_t h[2] = {0xc4, (uint8_t)s.len};
    if (!put(w, h, 2))
      return false;
  } else if (s.len <= UINT16_MAX) {
    uint8_t h[3] = {0xc5, (uint8_t)(s.len >> 8), (uint8_t)s.len};
    if (!put(w, h, 3))
      return false;
  } else if (s.len <= UINT32_MAX) {
    uint8_t h[5] = {0xc6, (uint8_t)(s.len >> 24), (uint8_t)(s.len >> 16),
                    (uint8_t)(s.len >> 8), (uint8_t)s.len};
    if (!put(w, h, 5))
      return false;
  } else
    return false;
  return put(w, s.data, s.len);
}
static bool put_double(writer_t *w, double d) {
  uint64_t u;
  uint8_t b[9] = {0xcb};
  memcpy(&u, &d, 8);
  store64be(b + 1, u);
  return put(w, b, 9);
}

static bool skip_obj(reader_t *r, unsigned depth) {
  if (depth > LXMF_MAX_NESTING || r->p >= r->end)
    return false;
  uint8_t c = *r->p++;
  uint64_t n = 0;
  size_t items = 0;
  if (c <= 0x7f || c >= 0xe0 || c == 0xc0 || c == 0xc2 || c == 0xc3)
    return true;
  if ((c & 0xe0) == 0xa0)
    n = c & 31;
  else if ((c & 0xf0) == 0x90)
    items = c & 15;
  else if ((c & 0xf0) == 0x80)
    items = 2 * (c & 15);
  else
    switch (c) {
    case 0xc4:
    case 0xd9:
      if (r->p + 1 > r->end)
        return false;
      n = *r->p++;
      break;
    case 0xc5:
    case 0xda:
      if (r->p + 2 > r->end)
        return false;
      n = ((uint64_t)r->p[0] << 8) | r->p[1];
      r->p += 2;
      break;
    case 0xc6:
    case 0xdb:
      if (r->p + 4 > r->end)
        return false;
      n = load32be(r->p);
      r->p += 4;
      break;
    case 0xca:
      n = 4;
      break;
    case 0xcb:
      n = 8;
      break;
    case 0xcc:
      n = 1;
      break;
    case 0xcd:
      n = 2;
      break;
    case 0xce:
      n = 4;
      break;
    case 0xcf:
      n = 8;
      break;
    case 0xd0:
      n = 1;
      break;
    case 0xd1:
      n = 2;
      break;
    case 0xd2:
      n = 4;
      break;
    case 0xd3:
      n = 8;
      break;
    case 0xd4:
      n = 2;
      break;
    case 0xd5:
      n = 3;
      break;
    case 0xd6:
      n = 5;
      break;
    case 0xd7:
      n = 9;
      break;
    case 0xd8:
      n = 17;
      break;
    case 0xdc:
      if (r->p + 2 > r->end)
        return false;
      items = ((size_t)r->p[0] << 8) | r->p[1];
      r->p += 2;
      break;
    case 0xdd:
      if (r->p + 4 > r->end)
        return false;
      items = load32be(r->p);
      r->p += 4;
      break;
    case 0xde:
      if (r->p + 2 > r->end)
        return false;
      items = 2u * (((size_t)r->p[0] << 8) | r->p[1]);
      r->p += 2;
      break;
    case 0xdf:
      if (r->p + 4 > r->end)
        return false;
      {
        uint32_t q = load32be(r->p);
        if (q > UINT32_MAX / 2)
          return false;
        items = 2u * (size_t)q;
      }
      r->p += 4;
      break;
    default:
      return false;
    }
  if (n) {
    if (n > (uint64_t)(r->end - r->p))
      return false;
    r->p += (size_t)n;
  }
  for (size_t i = 0; i < items; i++)
    if (!skip_obj(r, depth + 1))
      return false;
  return true;
}
static bool get_bin(reader_t *r, lxmf_slice_t *s) {
  const uint8_t *start = r->p;
  if (!skip_obj(r, 0))
    return false;
  uint8_t c = *start;
  size_t h, n;
  if (c >= 0xa0 && c <= 0xbf) {
    h = 1;
    n = c & 31;
  } else if (c == 0xc4 || c == 0xd9) {
    h = 2;
    n = start[1];
  } else if (c == 0xc5 || c == 0xda) {
    h = 3;
    n = ((size_t)start[1] << 8) | start[2];
  } else if (c == 0xc6 || c == 0xdb) {
    h = 5;
    n = load32be(start + 1);
  } else
    return false;
  s->data = start + h;
  s->len = n;
  return true;
}
static bool get_double(reader_t *r, double *d) {
  if (r->end - r->p < 9 || *r->p++ != 0xcb)
    return false;
  uint64_t u = ((uint64_t)load32be(r->p) << 32) | load32be(r->p + 4);
  r->p += 8;
  memcpy(d, &u, 8);
  return true;
}

static bool get_container_count(reader_t *r, uint8_t fix_mask, uint8_t fix_tag,
                                uint8_t code16, uint8_t code32, size_t *count) {
  if (r->p >= r->end)
    return false;
  uint8_t code = *r->p++;
  if ((code & fix_mask) == fix_tag) {
    *count = code & (uint8_t)~fix_mask;
    return true;
  }
  if (code == code16) {
    if ((size_t)(r->end - r->p) < 2u)
      return false;
    *count = ((size_t)r->p[0] << 8u) | r->p[1];
    r->p += 2;
    return true;
  }
  if (code == code32) {
    if ((size_t)(r->end - r->p) < 4u)
      return false;
    uint32_t value = load32be(r->p);
    r->p += 4;
    *count = (size_t)value;
    if ((uint32_t)*count != value)
      return false;
    return true;
  }
  return false;
}

static bool get_unsigned(reader_t *r, uint64_t *value) {
  if (r->p >= r->end)
    return false;
  uint8_t code = *r->p++;
  if (code <= 0x7fu) {
    *value = code;
    return true;
  }
  size_t bytes;
  switch (code) {
  case 0xcc:
    bytes = 1u;
    break;
  case 0xcd:
    bytes = 2u;
    break;
  case 0xce:
    bytes = 4u;
    break;
  case 0xcf:
    bytes = 8u;
    break;
  default:
    return false;
  }
  if ((size_t)(r->end - r->p) < bytes)
    return false;
  uint64_t result = 0u;
  for (size_t i = 0u; i < bytes; ++i)
    result = (result << 8u) | r->p[i];
  r->p += bytes;
  *value = result;
  return true;
}

static bool get_expiry(reader_t *r, uint64_t *expiry) {
  if (r->p >= r->end)
    return false;
  if (*r->p != 0xcau && *r->p != 0xcbu)
    return get_unsigned(r, expiry);
  uint8_t code = *r->p++;
  double value;
  if (code == 0xcau) {
    if ((size_t)(r->end - r->p) < 4u)
      return false;
    uint32_t bits = load32be(r->p);
    float decoded;
    r->p += 4;
    memcpy(&decoded, &bits, sizeof decoded);
    value = decoded;
  } else {
    if ((size_t)(r->end - r->p) < 8u)
      return false;
    uint64_t bits = ((uint64_t)load32be(r->p) << 32u) | load32be(r->p + 4u);
    r->p += 8;
    memcpy(&value, &bits, sizeof value);
  }
  /* 2^64 is exactly representable as a double; requiring a smaller value
   * makes the conversion defined even though UINT64_MAX itself rounds up. */
  if (!(value >= 0.0) || value >= 18446744073709551616.0)
    return false;
  /* Python generates ticket expiry with time.time(), including fractions.
   * The durable store uses seconds: round down, never extend validity. */
  *expiry = (uint64_t)value;
  return true;
}

lxmf_status_t lxmf_fields_parse_ticket(const uint8_t *fields,
                                       size_t fields_length,
                                       lxmf_ticket_field_t *ticket) {
  if (fields == NULL || fields_length == 0u || ticket == NULL)
    return LXMF_ERR_ARGUMENT;
  memset(ticket, 0, sizeof *ticket);
  reader_t reader = {fields, fields + fields_length};
  size_t count = 0u;
  if (!get_container_count(&reader, 0xf0u, 0x80u, 0xdeu, 0xdfu, &count))
    return LXMF_ERR_FORMAT;
  for (size_t i = 0u; i < count; ++i) {
    reader_t key_reader = reader;
    uint64_t key = UINT64_MAX;
    bool integer_key = get_unsigned(&key_reader, &key);
    if (!skip_obj(&reader, 0u))
      return LXMF_ERR_FORMAT;
    if (!integer_key || key != LXMF_FIELD_TICKET) {
      if (!skip_obj(&reader, 0u))
        return LXMF_ERR_FORMAT;
      continue;
    }
    if (ticket->present)
      return LXMF_ERR_FORMAT;
    size_t values = 0u;
    if (!get_container_count(&reader, 0xf0u, 0x90u, 0xdcu, 0xddu, &values) ||
        values != 2u || !get_expiry(&reader, &ticket->expires_at))
      return LXMF_ERR_FORMAT;
    lxmf_slice_t bytes;
    if (!get_bin(&reader, &bytes) || bytes.len != LXMF_TICKET_LENGTH)
      return LXMF_ERR_FORMAT;
    memcpy(ticket->ticket, bytes.data, LXMF_TICKET_LENGTH);
    ticket->present = true;
  }
  return reader.p == reader.end ? LXMF_OK : LXMF_ERR_FORMAT;
}

static size_t bin_bound(size_t n) {
  return n <= UINT32_MAX && n <= SIZE_MAX - 5u ? n + 5u : 0u;
}
static bool add_bound(size_t *total, size_t value) {
  if (value > SIZE_MAX - *total)
    return false;
  *total += value;
  return *total <= LXMF_MAX_MESSAGE_SIZE;
}
size_t lxmf_pack_bound(const lxmf_message_t *m) {
  if (!m)
    return 0;
  size_t title = bin_bound(m->title.len), content = bin_bound(m->content.len);
  size_t stamp_len = m->stamp_len ? m->stamp_len : LXMF_STAMP_LENGTH;
  size_t stamp = m->has_stamp ? bin_bound(stamp_len) : 0u;
  if (title == 0u || content == 0u || (m->has_stamp && stamp == 0u))
    return 0u;
  size_t fields = m->fields_msgpack.len ? m->fields_msgpack.len : 1u;
  size_t bound = LXMF_HEADER_LENGTH;
  return add_bound(&bound, 10u) && add_bound(&bound, fields) &&
                 add_bound(&bound, title) && add_bound(&bound, content) &&
                 add_bound(&bound, stamp)
             ? bound
             : 0u;
}

static lxmf_status_t encode_payload(const lxmf_message_t *m, bool stamp,
                                    writer_t *w) {
  if (!put_u8(w, (uint8_t)(stamp ? 0x95 : 0x94)) ||
      !put_double(w, m->timestamp) || !put_bin(w, m->title) ||
      !put_bin(w, m->content))
    return LXMF_ERR_BOUNDS;
  if (m->fields_msgpack.len) {
    reader_t rr = {m->fields_msgpack.data,
                   m->fields_msgpack.data + m->fields_msgpack.len};
    const uint8_t *p = rr.p;
    if (!skip_obj(&rr, 0) || rr.p != rr.end ||
        ((*p & 0xf0) != 0x80 && *p != 0xde && *p != 0xdf))
      return LXMF_ERR_FORMAT;
    if (!put(w, p, m->fields_msgpack.len))
      return LXMF_ERR_BOUNDS;
  } else if (!put_u8(w, 0x80))
    return LXMF_ERR_BOUNDS;
  if (stamp) {
    size_t n = m->stamp_len ? m->stamp_len : LXMF_STAMP_LENGTH;
    if (n != LXMF_STAMP_LENGTH && n != LXMF_POW_STAMP_LENGTH)
      return LXMF_ERR_FORMAT;
    lxmf_slice_t s = {m->stamp, n};
    if (!put_bin(w, s))
      return LXMF_ERR_BOUNDS;
  }
  return LXMF_OK;
}

lxmf_status_t lxmf_pack(const lxmf_message_t *m, lxmf_sign_fn signer, void *ctx,
                        uint8_t *out, size_t cap, size_t *out_len) {
  if (!m || !signer || !out || !out_len)
    return LXMF_ERR_ARGUMENT;
  *out_len = 0u;
  size_t bound = lxmf_pack_bound(m);
  if (bound == 0u || cap < bound)
    return LXMF_ERR_BOUNDS;
  size_t payload_cap = bound - LXMF_HEADER_LENGTH;
  uint8_t *payload = malloc(payload_cap);
  if (!payload)
    return LXMF_ERR_BOUNDS;
  writer_t pw = {payload, payload_cap};
  lxmf_status_t st = encode_payload(m, false, &pw);
  if (st != LXMF_OK) {
    free(payload);
    return st;
  }
  size_t plen = payload_cap - pw.left;
  sha_ctx hc;
  sha_init(&hc);
  sha_update(&hc, m->destination, 16);
  sha_update(&hc, m->source, 16);
  sha_update(&hc, payload, plen);
  uint8_t id[32];
  sha_final(&hc, id);
  if (plen > SIZE_MAX - 64u) {
    rns_hal_secure_zero(payload, payload_cap);
    free(payload);
    return LXMF_ERR_BOUNDS;
  }
  size_t prelen = plen + 64u;
  uint8_t *preimage = malloc(prelen);
  if (!preimage) {
    rns_hal_secure_zero(payload, payload_cap);
    free(payload);
    return LXMF_ERR_BOUNDS;
  }
  size_t offset = 0u;
  memcpy(preimage + offset, m->destination, 16);
  offset += 16u;
  memcpy(preimage + offset, m->source, 16);
  offset += 16u;
  memcpy(preimage + offset, payload, plen);
  offset += plen;
  memcpy(preimage + offset, id, 32);
  uint8_t sig[64];
  st = signer(ctx, preimage, prelen, sig);
  rns_hal_secure_zero(preimage, prelen);
  free(preimage);
  rns_hal_secure_zero(payload, payload_cap);
  free(payload);
  if (st != LXMF_OK)
    return LXMF_ERR_CRYPTO;
  writer_t w = {out, cap};
  if (!put(&w, m->destination, 16) || !put(&w, m->source, 16) ||
      !put(&w, sig, 64))
    return LXMF_ERR_BOUNDS;
  st = encode_payload(m, m->has_stamp, &w);
  if (st != LXMF_OK)
    return st;
  *out_len = cap - w.left;
  return LXMF_OK;
}

lxmf_status_t lxmf_unpack(const uint8_t *in, size_t len, lxmf_verify_fn verify,
                          void *ctx, lxmf_message_t *m) {
  if (!in || !m)
    return LXMF_ERR_ARGUMENT;
  if (len < LXMF_HEADER_LENGTH + 1 || len > LXMF_MAX_MESSAGE_SIZE)
    return LXMF_ERR_FORMAT;
  memset(m, 0, sizeof *m);
  memcpy(m->destination, in, 16);
  memcpy(m->source, in + 16, 16);
  memcpy(m->signature, in + 32, 64);
  reader_t r = {in + LXMF_HEADER_LENGTH, in + len};
  uint8_t a = *r.p++;
  if (a != 0x94 && a != 0x95)
    return LXMF_ERR_FORMAT;
  if (!get_double(&r, &m->timestamp) || !get_bin(&r, &m->title) ||
      !get_bin(&r, &m->content))
    return LXMF_ERR_FORMAT;
  const uint8_t *fs = r.p;
  if (!skip_obj(&r, 0) || ((*fs & 0xf0) != 0x80 && *fs != 0xde && *fs != 0xdf))
    return LXMF_ERR_FORMAT;
  m->fields_msgpack.data = fs;
  m->fields_msgpack.len = (size_t)(r.p - fs);
  if (a == 0x95) {
    lxmf_slice_t s;
    if (!get_bin(&r, &s) ||
        (s.len != LXMF_STAMP_LENGTH && s.len != LXMF_POW_STAMP_LENGTH))
      return LXMF_ERR_FORMAT;
    m->has_stamp = true;
    m->stamp_len = s.len;
    memcpy(m->stamp, s.data, s.len);
  }
  if (r.p != r.end)
    return LXMF_ERR_FORMAT;
  lxmf_message_t base = *m;
  base.has_stamp = false;
  size_t payload_capacity = len - LXMF_HEADER_LENGTH;
  uint8_t *payload = malloc(payload_capacity);
  if (!payload)
    return LXMF_ERR_BOUNDS;
  writer_t pw = {payload, payload_capacity};
  lxmf_status_t st = encode_payload(&base, false, &pw);
  if (st != LXMF_OK) {
    free(payload);
    return st;
  }
  size_t plen = payload_capacity - pw.left;
  sha_ctx hc;
  sha_init(&hc);
  sha_update(&hc, m->destination, 16);
  sha_update(&hc, m->source, 16);
  sha_update(&hc, payload, plen);
  sha_final(&hc, m->message_id);
  if (verify) {
    if (plen > SIZE_MAX - 64u) {
      rns_hal_secure_zero(payload, payload_capacity);
      free(payload);
      return LXMF_ERR_BOUNDS;
    }
    size_t n = plen + 64u;
    uint8_t *pre = malloc(n);
    if (!pre) {
      rns_hal_secure_zero(payload, payload_capacity);
      free(payload);
      return LXMF_ERR_BOUNDS;
    }
    size_t offset = 0u;
    memcpy(pre + offset, m->destination, 16);
    offset += 16u;
    memcpy(pre + offset, m->source, 16);
    offset += 16u;
    memcpy(pre + offset, payload, plen);
    offset += plen;
    memcpy(pre + offset, m->message_id, 32);
    st = verify(ctx, m->source, pre, n, m->signature);
    rns_hal_secure_zero(pre, n);
    free(pre);
    rns_hal_secure_zero(payload, payload_capacity);
    free(payload);
    if (st == LXMF_ERR_UNKNOWN_SIGNER)
      return LXMF_ERR_UNKNOWN_SIGNER;
    if (st != LXMF_OK)
      return LXMF_ERR_SIGNATURE;
  } else {
    rns_hal_secure_zero(payload, payload_capacity);
    free(payload);
  }
  return LXMF_OK;
}

void lxmf_ticket_stamp(const uint8_t ticket[16], const uint8_t id[32],
                       uint8_t stamp[16]) {
  sha_ctx c;
  uint8_t d[32];
  sha_init(&c);
  sha_update(&c, ticket, 16);
  sha_update(&c, id, 32);
  sha_final(&c, d);
  memcpy(stamp, d, 16);
}
bool lxmf_ticket_stamp_valid(const uint8_t stamp[16], const uint8_t ticket[16],
                             const uint8_t id[32]) {
  uint8_t want[16], difference = 0;
  lxmf_ticket_stamp(ticket, id, want);
  for (unsigned i = 0; i < 16; i++)
    difference |= (uint8_t)(want[i] ^ stamp[i]);
  return difference == 0;
}

lxmf_status_t lxmf_identity_signer(void *identity, const uint8_t *data,
                                   size_t len, uint8_t signature[64]) {
  if (!identity || (!data && len) || !signature)
    return LXMF_ERR_ARGUMENT;
  return rns_identity_sign((const rns_identity *)identity, data, len, signature)
             ? LXMF_OK
             : LXMF_ERR_CRYPTO;
}
lxmf_status_t lxmf_identity_verifier(void *context, const uint8_t source[16],
                                     const uint8_t *data, size_t len,
                                     const uint8_t signature[64]) {
  lxmf_identity_verifier_context_t *c =
      (lxmf_identity_verifier_context_t *)context;
  if (!c || !c->resolve || !source || (!data && len) || !signature)
    return LXMF_ERR_ARGUMENT;
  const rns_identity *identity = c->resolve(c->resolve_context, source);
  if (!identity)
    return LXMF_ERR_UNKNOWN_SIGNER;
  return rns_identity_verify(identity, data, len, signature)
             ? LXMF_OK
             : LXMF_ERR_SIGNATURE;
}

static size_t msgpack_uint(uint32_t n, uint8_t out[5]) {
  if (n <= 0x7f) {
    out[0] = (uint8_t)n;
    return 1;
  }
  if (n <= 0xff) {
    out[0] = 0xcc;
    out[1] = (uint8_t)n;
    return 2;
  }
  if (n <= 0xffff) {
    out[0] = 0xcd;
    out[1] = (uint8_t)(n >> 8);
    out[2] = (uint8_t)n;
    return 3;
  }
  out[0] = 0xce;
  store32be(out + 1, n);
  return 5;
}
static lxmf_status_t stamp_workblock(const uint8_t id[32], uint32_t rounds,
                                     uint8_t **out) {
  if (rounds == 0 || rounds > LXMF_STAMP_WORKBLOCK_ROUNDS)
    return LXMF_ERR_ARGUMENT;
  size_t size = (size_t)rounds * 256u;
  uint8_t *wb = (uint8_t *)malloc(size);
  if (!wb)
    return LXMF_ERR_BOUNDS;
  for (uint32_t n = 0; n < rounds; n++) {
    uint8_t packed[5], salt_input[37], salt[32];
    size_t pn = msgpack_uint(n, packed);
    memcpy(salt_input, id, 32);
    memcpy(salt_input + 32, packed, pn);
    lxmf_sha256(salt_input, 32 + pn, salt);
    if (!rns_hkdf_sha256(id, 32, salt, 32, NULL, 0, wb + (size_t)n * 256u,
                         256u)) {
      free(wb);
      return LXMF_ERR_CRYPTO;
    }
  }
  *out = wb;
  return LXMF_OK;
}
static uint8_t digest_value(const uint8_t d[32]) {
  uint8_t v = 0;
  for (size_t i = 0; i < 32; i++) {
    if (d[i] == 0) {
      v = (uint8_t)(v + 8);
      continue;
    }
    uint8_t b = d[i];
    while ((b & 0x80) == 0) {
      v++;
      b <<= 1;
    }
    break;
  }
  return v;
}
static bool digest_meets(const uint8_t d[32], uint8_t cost) {
  uint8_t target[32] = {0};
  unsigned bit = 256u - cost;
  target[31u - bit / 8u] = (uint8_t)(1u << (bit % 8u));
  return memcmp(d, target, 32) <= 0;
}
static void hash_work_stamp(const uint8_t *wb, uint32_t rounds,
                            const uint8_t stamp[32], uint8_t digest[32]) {
  sha_ctx c;
  sha_init(&c);
  sha_update(&c, wb, (size_t)rounds * 256u);
  sha_update(&c, stamp, 32);
  sha_final(&c, digest);
}
struct lxmf_stamp_job {
  uint8_t id[32], nonce[32], cost, value;
  uint32_t rounds;
  sha_ctx prefix;
  lxmf_stamp_job_progress_t progress;
};

lxmf_status_t lxmf_stamp_job_create(const uint8_t id[32], uint8_t cost,
                                    const uint8_t nonce[32],
                                    lxmf_stamp_job_t **out) {
  return lxmf_stamp_job_create_expanded(id, cost, LXMF_STAMP_WORKBLOCK_ROUNDS,
                                        nonce, out);
}

lxmf_status_t lxmf_stamp_job_create_expanded(const uint8_t id[32], uint8_t cost,
                                             uint32_t rounds,
                                             const uint8_t nonce[32],
                                             lxmf_stamp_job_t **out) {
  if (!out)
    return LXMF_ERR_ARGUMENT;
  *out = NULL;
  if (!id || cost == 0 || rounds == 0 || rounds > LXMF_STAMP_WORKBLOCK_ROUNDS)
    return LXMF_ERR_ARGUMENT;
  lxmf_stamp_job_t *job = calloc(1, sizeof *job);
  if (!job)
    return LXMF_ERR_BOUNDS;
  memcpy(job->id, id, 32);
  job->cost = cost;
  job->rounds = rounds;
  if (nonce)
    memcpy(job->nonce, nonce, 32);
  else if (!rns_random_bytes(job->nonce, 32)) {
    free(job);
    return LXMF_ERR_CRYPTO;
  }
  sha_init(&job->prefix);
  job->progress.state = LXMF_STAMP_PREPARING;
  job->progress.result = LXMF_ERR_PENDING;
  *out = job;
  return LXMF_OK;
}

lxmf_status_t lxmf_stamp_job_poll(lxmf_stamp_job_t *job, uint32_t budget) {
  if (!job || budget > LXMF_STAMP_POLL_MAX_UNITS)
    return LXMF_ERR_ARGUMENT;
  while (budget-- && job->progress.result == LXMF_ERR_PENDING) {
    if (job->progress.state == LXMF_STAMP_PREPARING) {
      uint8_t packed[5], input[37], salt[32], block[256];
      size_t length = msgpack_uint(job->progress.prepared_rounds, packed);
      memcpy(input, job->id, 32);
      memcpy(input + 32, packed, length);
      lxmf_sha256(input, 32 + length, salt);
      if (!rns_hkdf_sha256(job->id, 32, salt, 32, NULL, 0, block,
                           sizeof block)) {
        job->progress.state = LXMF_STAMP_FAILED;
        job->progress.result = LXMF_ERR_CRYPTO;
        break;
      }
      /* Hashing the prefix incrementally gives exactly the same SHA-256
       * state as hashing the entire materialized upstream workblock. */
      sha_update(&job->prefix, block, sizeof block);
      if (++job->progress.prepared_rounds == job->rounds)
        job->progress.state = LXMF_STAMP_SEARCHING;
    } else {
      sha_ctx candidate = job->prefix;
      uint8_t digest[32];
      sha_update(&candidate, job->nonce, 32);
      sha_final(&candidate, digest);
      if (job->progress.attempts == UINT64_MAX) {
        job->progress.state = LXMF_STAMP_FAILED;
        job->progress.result = LXMF_ERR_BOUNDS;
        break;
      }
      job->progress.attempts++;
      if (digest_meets(digest, job->cost)) {
        job->value = digest_value(digest);
        job->progress.state = LXMF_STAMP_COMPLETE;
        job->progress.result = LXMF_OK;
      } else {
        for (int i = 31; i >= 0; --i) {
          if (++job->nonce[i])
            break;
        }
      }
    }
  }
  return job->progress.result;
}

void lxmf_stamp_job_cancel(lxmf_stamp_job_t *job) {
  if (job && job->progress.result == LXMF_ERR_PENDING) {
    job->progress.state = LXMF_STAMP_CANCELLED;
    job->progress.result = LXMF_ERR_CANCELLED;
  }
}
void lxmf_stamp_job_destroy(lxmf_stamp_job_t *job) { free(job); }
lxmf_status_t lxmf_stamp_job_progress(const lxmf_stamp_job_t *job,
                                      lxmf_stamp_job_progress_t *progress) {
  if (!job || !progress)
    return LXMF_ERR_ARGUMENT;
  *progress = job->progress;
  return LXMF_OK;
}
lxmf_status_t lxmf_stamp_job_result(const lxmf_stamp_job_t *job,
                                    uint8_t stamp[32], uint8_t *value) {
  if (!job || !stamp)
    return LXMF_ERR_ARGUMENT;
  if (job->progress.result != LXMF_OK)
    return job->progress.result;
  memcpy(stamp, job->nonce, 32);
  if (value)
    *value = job->value;
  return LXMF_OK;
}
lxmf_status_t lxmf_pow_stamp_validate_expanded(const uint8_t id[32],
                                               uint8_t cost, uint32_t rounds,
                                               const uint8_t stamp[32],
                                               uint8_t *value) {
  if (!id || !stamp || cost < 1)
    return LXMF_ERR_ARGUMENT;
  uint8_t *wb = NULL;
  lxmf_status_t st = stamp_workblock(id, rounds, &wb);
  if (st != LXMF_OK)
    return st;
  uint8_t d[32];
  hash_work_stamp(wb, rounds, stamp, d);
  free(wb);
  if (value)
    *value = digest_value(d);
  return digest_meets(d, cost) ? LXMF_OK : LXMF_ERR_FORMAT;
}
lxmf_status_t lxmf_pow_stamp_validate(const uint8_t id[32], uint8_t cost,
                                      const uint8_t stamp[32], uint8_t *value) {
  return lxmf_pow_stamp_validate_expanded(id, cost, LXMF_STAMP_WORKBLOCK_ROUNDS,
                                          stamp, value);
}
lxmf_status_t lxmf_pow_stamp_generate(const uint8_t id[32], uint8_t cost,
                                      lxmf_stamp_progress_fn progress,
                                      void *ctx, uint8_t stamp[32],
                                      uint8_t *value, uint64_t *attempts) {
  if (!id || !stamp || cost < 1)
    return LXMF_ERR_ARGUMENT;
  if (progress && !progress(ctx, 0))
    return LXMF_ERR_CANCELLED;
  uint8_t *wb = NULL;
  lxmf_status_t st = stamp_workblock(id, LXMF_STAMP_WORKBLOCK_ROUNDS, &wb);
  if (st != LXMF_OK)
    return st;
  if (!rns_random_bytes(stamp, 32)) {
    free(wb);
    return LXMF_ERR_CRYPTO;
  }
  uint64_t rounds = 0;
  for (;;) {
    uint8_t d[32];
    hash_work_stamp(wb, LXMF_STAMP_WORKBLOCK_ROUNDS, stamp, d);
    rounds++;
    if (digest_meets(d, cost)) {
      if (value)
        *value = digest_value(d);
      if (attempts)
        *attempts = rounds;
      free(wb);
      return LXMF_OK;
    }
    for (int i = 31; i >= 0; i--) {
      stamp[i]++;
      if (stamp[i])
        break;
    }
    if ((rounds & 1023u) == 0 && progress && !progress(ctx, rounds)) {
      if (attempts)
        *attempts = rounds;
      free(wb);
      return LXMF_ERR_CANCELLED;
    }
  }
}

const char *lxmf_status_string(lxmf_status_t s) {
  switch (s) {
  case LXMF_OK:
    return "ok";
  case LXMF_ERR_ARGUMENT:
    return "invalid argument";
  case LXMF_ERR_BOUNDS:
    return "buffer bounds";
  case LXMF_ERR_FORMAT:
    return "invalid LXMF/MessagePack format";
  case LXMF_ERR_CRYPTO:
    return "cryptographic operation failed";
  case LXMF_ERR_SIGNATURE:
    return "signature verification failed";
  case LXMF_ERR_CANCELLED:
    return "operation cancelled";
  case LXMF_ERR_TIMEOUT:
    return "delivery timed out";
  case LXMF_ERR_PENDING:
    return "waiting for delivery prerequisite";
  case LXMF_ERR_UNKNOWN_SIGNER:
    return "signer identity not held locally";
  case LXMF_ERR_STAMP:
    return "stamp validation failed";
  case LXMF_ERR_BLOCKED:
    return "source blocked by inbound policy";
  default:
    return "unknown LXMF error";
  }
}
const char *lxmf_signature_state_string(lxmf_signature_state_t s) {
  switch (s) {
  case LXMF_SIGNATURE_VERIFIED:
    return "verified";
  case LXMF_SIGNATURE_UNVERIFIED:
    return "unverified sender";
  case LXMF_SIGNATURE_FAILED:
    return "signature invalid";
  default:
    return "unknown signature state";
  }
}
