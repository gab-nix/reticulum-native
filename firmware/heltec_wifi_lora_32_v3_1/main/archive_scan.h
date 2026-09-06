/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef HELTEC_ARCHIVE_SCAN_H
#define HELTEC_ARCHIVE_SCAN_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
/* Initialize cursor=64. One polling owner calls next then complete for every
 * admitted slot. Announcements cannot bypass storage-failure cooldowns. */
typedef struct {
    size_t cursor;
    uint64_t next_ms;
    bool rescan, clock_exhausted;
} heltec_archive_scan;
static inline void heltec_archive_scan_request(heltec_archive_scan *s) {
    if(s->cursor==64U) s->cursor=0;
    else s->rescan=true;
}
static inline void heltec_archive_scan_delay(heltec_archive_scan *s,uint64_t now,uint64_t delay) {
    if(now>UINT64_MAX-delay) { s->next_ms=UINT64_MAX; s->clock_exhausted=true; }
    else s->next_ms=now+delay;
}
static inline bool heltec_archive_scan_next(heltec_archive_scan *s,uint64_t now,size_t *slot) {
    if(!slot || s->clock_exhausted || s->cursor>=64U || now<s->next_ms) return false;
    *slot=s->cursor++;
    heltec_archive_scan_delay(s,now,250U);
    return true;
}
static inline void heltec_archive_scan_complete(heltec_archive_scan *s,uint64_t now,bool retry) {
    if(retry) { s->rescan=true; heltec_archive_scan_delay(s,now,5000U); }
    if(s->cursor==64U && s->rescan) { s->cursor=0; s->rescan=false; }
}
#endif
