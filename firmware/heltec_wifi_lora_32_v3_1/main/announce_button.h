/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef HELTEC_ANNOUNCE_BUTTON_H
#define HELTEC_ANNOUNCE_BUTTON_H
#include <stdbool.h>
#include <stdint.h>
typedef struct {
    bool raw_pressed, stable_pressed, armed, sent;
    uint64_t changed_ms, last_announce_ms;
} heltec_announce_button;
/* Zero-initialize; boot-held button must be released before it can trigger.
 * 50 ms debounce, one event on release, 60 s cooldown, no held repeat. */
bool heltec_announce_button_poll(heltec_announce_button *button, bool pressed, uint64_t now_ms);
#endif
