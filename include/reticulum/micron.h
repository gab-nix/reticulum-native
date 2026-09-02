#ifndef RETICULUM_MICRON_H
#define RETICULUM_MICRON_H
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#define RNS_MICRON_MAX_ITEMS 256u
#define RNS_MICRON_TEXT_MAX 512u
typedef enum { RNS_MICRON_TEXT, RNS_MICRON_LINK, RNS_MICRON_INPUT, RNS_MICRON_BUTTON, RNS_MICRON_MEDIA, RNS_MICRON_HEADING } rns_micron_kind;
typedef struct { rns_micron_kind kind; char text[RNS_MICRON_TEXT_MAX]; char target[RNS_MICRON_TEXT_MAX]; bool unsupported; } rns_micron_item;
typedef struct { rns_micron_item items[RNS_MICRON_MAX_ITEMS]; size_t count; } rns_micron_page;
typedef struct { char urls[32][RNS_MICRON_TEXT_MAX]; size_t count; size_t cursor; } rns_micron_history;
int rns_micron_parse(rns_micron_page *page, const uint8_t *data, size_t length);
int rns_micron_normalize_url(const char *base, const char *target, char *out, size_t capacity);
void rns_micron_history_init(rns_micron_history *history);
int rns_micron_history_push(rns_micron_history *history, const char *url);
const char *rns_micron_history_back(rns_micron_history *history);
const char *rns_micron_history_forward(rns_micron_history *history);
#endif
