/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef HELTEC_BUTTON_MENU_H
#define HELTEC_BUTTON_MENU_H
#include <stdbool.h>
#include <stdint.h>
typedef enum { HELTEC_MENU_NONE, HELTEC_MENU_ANNOUNCE, HELTEC_MENU_MESSAGE, HELTEC_MENU_CLEAR } heltec_menu_action;
typedef struct {
    bool raw, stable, armed, open, announced, consumed;
    uint8_t selected;
    uint64_t changed, pressed_at, last_announce;
} heltec_button_menu;
/* Short release opens/cycles; >=700 ms stable hold selects immediately. Boot-held
 * input must be released first. No repeat while held; 60 s announce cooldown. */
heltec_menu_action heltec_button_menu_poll(heltec_button_menu *menu, bool pressed, uint64_t now);
const char *heltec_button_menu_label(const heltec_button_menu *menu);
#endif
