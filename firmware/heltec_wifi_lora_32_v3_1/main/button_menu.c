/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "button_menu.h"
const char *heltec_button_menu_label(const heltec_button_menu *m) {
    static const char *items[] = {"HOME", "CHATS", "ANNOUNCE", "CLEAR VIEW", "NODES", "CHANNEL"};
    return m ? items[m->selected % 6U] : "HOME";
}
heltec_menu_action heltec_button_menu_poll(heltec_button_menu *m, bool pressed, uint64_t now) {
    if (!m) return HELTEC_MENU_NONE;
    if (m->raw != pressed) { m->raw = pressed; m->changed = now; }
    if (now < m->changed || now - m->changed < 50U) return HELTEC_MENU_NONE;
    if (!pressed && !m->stable) { m->armed = true; return HELTEC_MENU_NONE; }
    if (m->stable == pressed) {
        if (!pressed || !m->armed || m->consumed || now < m->pressed_at ||
            now - m->pressed_at < 500U) return HELTEC_MENU_NONE;
        m->consumed = true;
        if (!m->open && m->hold_action) return HELTEC_MENU_SELECT;
        if (!m->open) { m->open = true; m->selected = 0; return HELTEC_MENU_NONE; }
        goto select_item;
    }
    m->stable = pressed;
    if (pressed) { m->pressed_at = now; m->consumed = false; return HELTEC_MENU_NONE; }
    if (!m->armed) { m->armed = true; return HELTEC_MENU_NONE; }
    if (m->consumed) return HELTEC_MENU_NONE;
    if (!m->open) {
        if (m->hold_action && now >= m->pressed_at && now-m->pressed_at >= 500U) return HELTEC_MENU_SELECT;
        if (m->browsing && now >= m->pressed_at && now-m->pressed_at < 500U) return HELTEC_MENU_NEXT;
        m->open = true; m->selected = 0; return HELTEC_MENU_NONE;
    }
    if (now < m->pressed_at || now - m->pressed_at < 500U) {
        m->selected = (uint8_t)((m->selected + 1U) % 6U); return HELTEC_MENU_NONE;
    }
select_item:
    m->open = false;
    if (m->selected == 1) return HELTEC_MENU_MESSAGE;
    if (m->selected == 3) return HELTEC_MENU_CLEAR;
    if (m->selected == 4) return HELTEC_MENU_NODES;
    if (m->selected == 5) return HELTEC_MENU_CHANNEL;
    if (m->selected == 2 && (!m->announced || (now >= m->last_announce && now - m->last_announce >= 60000U))) {
        m->announced = true; m->last_announce = now; return HELTEC_MENU_ANNOUNCE;
    }
    return HELTEC_MENU_NONE;
}
