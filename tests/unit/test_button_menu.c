/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "button_menu.h"
#include <assert.h>
#include <string.h>
static heltec_menu_action press(heltec_button_menu *m, uint64_t t, uint64_t duration) {
    assert(heltec_button_menu_poll(m, true, t) == HELTEC_MENU_NONE);
    assert(heltec_button_menu_poll(m, true, t + 50) == HELTEC_MENU_NONE);
    assert(heltec_button_menu_poll(m, false, t + duration) == HELTEC_MENU_NONE);
    return heltec_button_menu_poll(m, false, t + duration + 50);
}
int main(void) {
    heltec_button_menu m = {0};
    assert(press(&m, 0, 1000) == HELTEC_MENU_NONE && !m.open);
    assert(press(&m, 2000, 100) == HELTEC_MENU_NONE && m.open);
    assert(!strcmp(heltec_button_menu_label(&m), "STATUS"));
    assert(press(&m, 3000, 100) == HELTEC_MENU_NONE && m.selected == 1);
    assert(press(&m, 4000, 800) == HELTEC_MENU_MESSAGE && !m.open);
    assert(press(&m, 5000, 100) == HELTEC_MENU_NONE);
    assert(press(&m, 6000, 100) == HELTEC_MENU_NONE);
    assert(press(&m, 7000, 100) == HELTEC_MENU_NONE && m.selected == 2);
    assert(press(&m, 8000, 800) == HELTEC_MENU_ANNOUNCE);
    m.open = true; m.selected = 2;
    assert(press(&m, 10000, 800) == HELTEC_MENU_NONE);
    m.open = true; m.selected = 3;
    assert(press(&m, 12000, 800) == HELTEC_MENU_CLEAR);
    m.open = true; m.selected = 2;
    assert(press(&m, 70000, 800) == HELTEC_MENU_ANNOUNCE);
    return 0;
}
