#include "reticulum/lxmf_tickets.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "reticulum/crypto.h"

#define TICKET_FILE_HEADER 16u
#define TICKET_FILE_RECORD 48u
#define TICKET_FILE_VERSION 1u

enum ticket_kind {
  TICKET_OUTBOUND = 1,
  TICKET_INBOUND = 2,
  TICKET_LAST_DELIVERY = 3
};

typedef struct stored_ticket {
  uint8_t kind;
  uint8_t peer[LXMF_DESTINATION_LENGTH];
  uint64_t time;
  uint8_t ticket[LXMF_TICKET_LENGTH];
} stored_ticket_t;

struct lxmf_ticket_store {
  char path[LXMF_TICKET_STORE_PATH_MAX + 1u];
  stored_ticket_t entries[LXMF_TICKET_STORE_MAX_ENTRIES];
  size_t count;
};

static void put32(uint8_t *p, uint32_t v) {
  p[0] = (uint8_t)(v >> 24u);
  p[1] = (uint8_t)(v >> 16u);
  p[2] = (uint8_t)(v >> 8u);
  p[3] = (uint8_t)v;
}
static uint32_t get32(const uint8_t *p) {
  return ((uint32_t)p[0] << 24u) | ((uint32_t)p[1] << 16u) |
         ((uint32_t)p[2] << 8u) | (uint32_t)p[3];
}
static void put64(uint8_t *p, uint64_t v) {
  for (size_t i = 0u; i < 8u; ++i)
    p[7u - i] = (uint8_t)(v >> (8u * i));
}
static uint64_t get64(const uint8_t *p) {
  uint64_t v = 0u;
  for (size_t i = 0u; i < 8u; ++i)
    v = (v << 8u) | p[i];
  return v;
}
static uint32_t crc32_bytes(const uint8_t *p, size_t n) {
  uint32_t c = UINT32_C(0xffffffff);
  while (n-- != 0u) {
    c ^= *p++;
    for (unsigned i = 0u; i < 8u; ++i)
      c = (c >> 1u) ^ (UINT32_C(0xedb88320) & (~(c & 1u) + 1u));
  }
  return ~c;
}
static bool valid_kind(uint8_t kind) {
  return kind >= TICKET_OUTBOUND && kind <= TICKET_LAST_DELIVERY;
}
static bool same_peer(const stored_ticket_t *entry, uint8_t kind,
                      const uint8_t peer[LXMF_DESTINATION_LENGTH]) {
  return entry->kind == kind &&
         memcmp(entry->peer, peer, LXMF_DESTINATION_LENGTH) == 0;
}

static lxmf_status_t save(lxmf_ticket_store_t *store) {
  uint8_t payload[LXMF_TICKET_STORE_MAX_ENTRIES * TICKET_FILE_RECORD];
  for (size_t i = 0u; i < store->count; ++i) {
    uint8_t *record = payload + i * TICKET_FILE_RECORD;
    memset(record, 0, TICKET_FILE_RECORD);
    record[0] = store->entries[i].kind;
    memcpy(record + 8u, store->entries[i].peer, LXMF_DESTINATION_LENGTH);
    put64(record + 24u, store->entries[i].time);
    memcpy(record + 32u, store->entries[i].ticket, LXMF_TICKET_LENGTH);
  }
  size_t payload_length = store->count * TICKET_FILE_RECORD;
  uint8_t header[TICKET_FILE_HEADER] = {'L', 'X', 'T', 'K', TICKET_FILE_VERSION,
                                        0,   0,   0};
  put32(header + 8u, (uint32_t)store->count);
  put32(header + 12u, crc32_bytes(payload, payload_length));
  char temporary[LXMF_TICKET_STORE_PATH_MAX + 5u];
  int length = snprintf(temporary, sizeof temporary, "%s.tmp", store->path);
  if (length < 0 || (size_t)length >= sizeof temporary)
    return LXMF_ERR_BOUNDS;
  FILE *file = fopen(temporary, "w+b");
  if (file == NULL)
    return LXMF_ERR_CRYPTO;
  lxmf_status_t status = LXMF_OK;
  if (fwrite(header, 1u, sizeof header, file) != sizeof header ||
      (payload_length != 0u &&
       fwrite(payload, 1u, payload_length, file) != payload_length) ||
      fflush(file) != 0 || fsync(fileno(file)) != 0)
    status = LXMF_ERR_CRYPTO;
  if (fclose(file) != 0 && status == LXMF_OK)
    status = LXMF_ERR_CRYPTO;
  if (status == LXMF_OK && rename(temporary, store->path) != 0)
    status = LXMF_ERR_CRYPTO;
  if (status != LXMF_OK)
    (void)unlink(temporary);
  return status;
}

static lxmf_status_t load(lxmf_ticket_store_t *store) {
  FILE *file = fopen(store->path, "rb");
  if (file == NULL)
    return errno == ENOENT ? LXMF_OK : LXMF_ERR_CRYPTO;
  uint8_t header[TICKET_FILE_HEADER];
  lxmf_status_t status = LXMF_ERR_FORMAT;
  if (fread(header, 1u, sizeof header, file) != sizeof header ||
      memcmp(header, "LXTK", 4u) != 0 || header[4] != TICKET_FILE_VERSION)
    goto done;
  uint32_t count = get32(header + 8u);
  if (count > LXMF_TICKET_STORE_MAX_ENTRIES)
    goto done;
  size_t payload_length = (size_t)count * TICKET_FILE_RECORD;
  uint8_t payload[LXMF_TICKET_STORE_MAX_ENTRIES * TICKET_FILE_RECORD];
  if (payload_length != 0u &&
      fread(payload, 1u, payload_length, file) != payload_length)
    goto done;
  if (fgetc(file) != EOF ||
      crc32_bytes(payload, payload_length) != get32(header + 12u))
    goto done;
  for (uint32_t i = 0u; i < count; ++i) {
    const uint8_t *record = payload + (size_t)i * TICKET_FILE_RECORD;
    if (!valid_kind(record[0]))
      goto done;
    stored_ticket_t *entry = &store->entries[i];
    memset(entry, 0, sizeof *entry);
    entry->kind = record[0];
    memcpy(entry->peer, record + 8u, LXMF_DESTINATION_LENGTH);
    entry->time = get64(record + 24u);
    memcpy(entry->ticket, record + 32u, LXMF_TICKET_LENGTH);
  }
  store->count = count;
  status = LXMF_OK;
done:
  if (fclose(file) != 0 && status == LXMF_OK)
    status = LXMF_ERR_CRYPTO;
  return status;
}

lxmf_status_t lxmf_ticket_store_open(lxmf_ticket_store_t **output,
                                     const char *path) {
  if (output == NULL || path == NULL || *path == '\0' ||
      strlen(path) > LXMF_TICKET_STORE_PATH_MAX)
    return LXMF_ERR_ARGUMENT;
  *output = NULL;
  lxmf_ticket_store_t *store = calloc(1u, sizeof *store);
  if (store == NULL)
    return LXMF_ERR_BOUNDS;
  memcpy(store->path, path, strlen(path) + 1u);
  lxmf_status_t status = load(store);
  if (status != LXMF_OK) {
    free(store);
    return status;
  }
  *output = store;
  return LXMF_OK;
}

void lxmf_ticket_store_close(lxmf_ticket_store_t *store) { free(store); }

static stored_ticket_t *find_one(lxmf_ticket_store_t *store, uint8_t kind,
                                 const uint8_t peer[16]) {
  for (size_t i = 0u; i < store->count; ++i)
    if (same_peer(&store->entries[i], kind, peer))
      return &store->entries[i];
  return NULL;
}
static stored_ticket_t *append_entry(lxmf_ticket_store_t *store) {
  if (store->count == LXMF_TICKET_STORE_MAX_ENTRIES)
    return NULL;
  stored_ticket_t *entry = &store->entries[store->count++];
  memset(entry, 0, sizeof *entry);
  return entry;
}
static void remove_at(lxmf_ticket_store_t *store, size_t index) {
  if (index + 1u < store->count)
    memmove(&store->entries[index], &store->entries[index + 1u],
            (store->count - index - 1u) * sizeof store->entries[0]);
  --store->count;
  memset(&store->entries[store->count], 0, sizeof store->entries[0]);
}

lxmf_status_t lxmf_ticket_store_issue(lxmf_ticket_store_t *store,
                                      const uint8_t peer[16], uint64_t now,
                                      lxmf_ticket_entry_t *ticket,
                                      bool *created) {
  if (store == NULL || peer == NULL || ticket == NULL)
    return LXMF_ERR_ARGUMENT;
  if (created != NULL)
    *created = false;
  stored_ticket_t *last = find_one(store, TICKET_LAST_DELIVERY, peer);
  if (last != NULL &&
      (last->time > now || now - last->time < LXMF_TICKET_INTERVAL_SECONDS))
    return LXMF_ERR_PENDING;
  for (size_t i = 0u; i < store->count; ++i) {
    stored_ticket_t *entry = &store->entries[i];
    if (same_peer(entry, TICKET_INBOUND, peer) && entry->time > now &&
        entry->time - now > LXMF_TICKET_RENEW_SECONDS) {
      ticket->expires_at = entry->time;
      memcpy(ticket->ticket, entry->ticket, LXMF_TICKET_LENGTH);
      return LXMF_OK;
    }
  }
  stored_ticket_t *entry = append_entry(store);
  if (entry == NULL)
    return LXMF_ERR_BOUNDS;
  entry->kind = TICKET_INBOUND;
  memcpy(entry->peer, peer, 16u);
  if (UINT64_MAX - now < LXMF_TICKET_EXPIRY_SECONDS) {
    --store->count;
    return LXMF_ERR_BOUNDS;
  }
  entry->time = now + LXMF_TICKET_EXPIRY_SECONDS;
  if (!rns_random_bytes(entry->ticket, LXMF_TICKET_LENGTH)) {
    --store->count;
    return LXMF_ERR_CRYPTO;
  }
  lxmf_status_t status = save(store);
  if (status != LXMF_OK) {
    --store->count;
    memset(&store->entries[store->count], 0, sizeof store->entries[0]);
    return status;
  }
  ticket->expires_at = entry->time;
  memcpy(ticket->ticket, entry->ticket, LXMF_TICKET_LENGTH);
  if (created != NULL)
    *created = true;
  return LXMF_OK;
}

lxmf_status_t lxmf_ticket_store_remember_outbound(
    lxmf_ticket_store_t *store, const uint8_t peer[16],
    const lxmf_ticket_entry_t *ticket, uint64_t now) {
  if (store == NULL || peer == NULL || ticket == NULL ||
      ticket->expires_at <= now)
    return LXMF_ERR_ARGUMENT;
  stored_ticket_t *entry = find_one(store, TICKET_OUTBOUND, peer);
  bool appended = false;
  if (entry == NULL) {
    entry = append_entry(store);
    appended = true;
  }
  if (entry == NULL)
    return LXMF_ERR_BOUNDS;
  stored_ticket_t previous = *entry;
  entry->kind = TICKET_OUTBOUND;
  memcpy(entry->peer, peer, 16u);
  entry->time = ticket->expires_at;
  memcpy(entry->ticket, ticket->ticket, LXMF_TICKET_LENGTH);
  lxmf_status_t status = save(store);
  if (status != LXMF_OK) {
    if (appended)
      --store->count;
    else
      *entry = previous;
  }
  return status;
}

lxmf_status_t lxmf_ticket_store_get_outbound(lxmf_ticket_store_t *store,
                                             const uint8_t peer[16],
                                             uint64_t now,
                                             lxmf_ticket_entry_t *ticket) {
  if (store == NULL || peer == NULL || ticket == NULL)
    return LXMF_ERR_ARGUMENT;
  stored_ticket_t *entry = find_one(store, TICKET_OUTBOUND, peer);
  if (entry == NULL || entry->time <= now)
    return LXMF_ERR_PENDING;
  ticket->expires_at = entry->time;
  memcpy(ticket->ticket, entry->ticket, LXMF_TICKET_LENGTH);
  return LXMF_OK;
}

lxmf_status_t lxmf_ticket_store_get_inbound(
    lxmf_ticket_store_t *store, const uint8_t peer[16], uint64_t now,
    lxmf_ticket_entry_t *tickets, size_t capacity, size_t *ticket_count) {
  if (store == NULL || peer == NULL || ticket_count == NULL ||
      (capacity != 0u && tickets == NULL))
    return LXMF_ERR_ARGUMENT;
  *ticket_count = 0u;
  for (size_t i = 0u; i < store->count; ++i) {
    stored_ticket_t *entry = &store->entries[i];
    if (!same_peer(entry, TICKET_INBOUND, peer) || entry->time <= now)
      continue;
    if (*ticket_count == capacity)
      return LXMF_ERR_BOUNDS;
    tickets[*ticket_count].expires_at = entry->time;
    memcpy(tickets[*ticket_count].ticket, entry->ticket, LXMF_TICKET_LENGTH);
    ++*ticket_count;
  }
  return LXMF_OK;
}

lxmf_status_t lxmf_ticket_store_stamp_outbound(lxmf_ticket_store_t *store,
                                               const uint8_t peer[16],
                                               uint64_t now,
                                               const uint8_t message_id[32],
                                               uint8_t stamp[16]) {
  if (message_id == NULL || stamp == NULL)
    return LXMF_ERR_ARGUMENT;
  lxmf_ticket_entry_t ticket;
  lxmf_status_t status =
      lxmf_ticket_store_get_outbound(store, peer, now, &ticket);
  if (status != LXMF_OK)
    return status;
  lxmf_ticket_stamp(ticket.ticket, message_id, stamp);
  return LXMF_OK;
}

lxmf_status_t lxmf_ticket_store_validate_inbound(lxmf_ticket_store_t *store,
                                                 const uint8_t peer[16],
                                                 uint64_t now,
                                                 const uint8_t message_id[32],
                                                 const uint8_t stamp[16]) {
  if (store == NULL || peer == NULL || message_id == NULL || stamp == NULL)
    return LXMF_ERR_ARGUMENT;
  for (size_t i = 0u; i < store->count; ++i) {
    stored_ticket_t *entry = &store->entries[i];
    if (same_peer(entry, TICKET_INBOUND, peer) && entry->time > now &&
        lxmf_ticket_stamp_valid(stamp, entry->ticket, message_id))
      return LXMF_OK;
  }
  return LXMF_ERR_FORMAT;
}

lxmf_status_t lxmf_ticket_store_mark_delivered(lxmf_ticket_store_t *store,
                                               const uint8_t peer[16],
                                               uint64_t now) {
  if (store == NULL || peer == NULL)
    return LXMF_ERR_ARGUMENT;
  stored_ticket_t *entry = find_one(store, TICKET_LAST_DELIVERY, peer);
  bool appended = false;
  if (entry == NULL) {
    entry = append_entry(store);
    appended = true;
  }
  if (entry == NULL)
    return LXMF_ERR_BOUNDS;
  stored_ticket_t previous = *entry;
  entry->kind = TICKET_LAST_DELIVERY;
  memcpy(entry->peer, peer, 16u);
  entry->time = now;
  memset(entry->ticket, 0, sizeof entry->ticket);
  lxmf_status_t status = save(store);
  if (status != LXMF_OK) {
    if (appended)
      --store->count;
    else
      *entry = previous;
  }
  return status;
}

lxmf_status_t lxmf_ticket_store_cleanup(lxmf_ticket_store_t *store,
                                        uint64_t now) {
  if (store == NULL)
    return LXMF_ERR_ARGUMENT;
  stored_ticket_t *backup = NULL;
  size_t old_count = store->count;
  if (old_count != 0u) {
    backup = malloc(old_count * sizeof *backup);
    if (backup == NULL)
      return LXMF_ERR_BOUNDS;
    memcpy(backup, store->entries, old_count * sizeof *backup);
  }
  bool changed = false;
  for (size_t i = store->count; i > 0u; --i) {
    stored_ticket_t *entry = &store->entries[i - 1u];
    bool expired = entry->kind == TICKET_OUTBOUND
                       ? entry->time <= now
                       : entry->kind == TICKET_INBOUND &&
                             (entry->time <= now &&
                              now - entry->time > LXMF_TICKET_GRACE_SECONDS);
    if (expired) {
      remove_at(store, i - 1u);
      changed = true;
    }
  }
  lxmf_status_t status = changed ? save(store) : LXMF_OK;
  if (status != LXMF_OK) {
    memcpy(store->entries, backup, old_count * sizeof *backup);
    store->count = old_count;
  }
  free(backup);
  return status;
}

size_t lxmf_ticket_store_count(const lxmf_ticket_store_t *store) {
  return store != NULL ? store->count : 0u;
}
