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
    assert(!strcmp(heltec_button_menu_label(&m), "HOME"));
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
    m.open = true; m.selected = 1;
    assert(heltec_button_menu_poll(&m, true, 72000) == HELTEC_MENU_NONE);
    assert(heltec_button_menu_poll(&m, true, 72050) == HELTEC_MENU_NONE);
    assert(heltec_button_menu_poll(&m, true, 72549) == HELTEC_MENU_NONE);
    assert(heltec_button_menu_poll(&m, true, 72550) == HELTEC_MENU_MESSAGE);
    assert(!m.open);
    assert(heltec_button_menu_poll(&m, true, 74000) == HELTEC_MENU_NONE);
    assert(heltec_button_menu_poll(&m, false, 74100) == HELTEC_MENU_NONE);
    assert(heltec_button_menu_poll(&m, false, 74150) == HELTEC_MENU_NONE);
    assert(!m.open); /* Release must not reopen or advance the menu. */
    m.browsing = true;
    assert(press(&m, 75000, 100) == HELTEC_MENU_NEXT && !m.open);
    assert(heltec_button_menu_poll(&m, true, 76000) == HELTEC_MENU_NONE);
    assert(heltec_button_menu_poll(&m, true, 76050) == HELTEC_MENU_NONE);
    assert(heltec_button_menu_poll(&m, true, 76750) == HELTEC_MENU_NONE && m.open);
    assert(heltec_button_menu_poll(&m, false, 77000) == HELTEC_MENU_NONE);
    assert(heltec_button_menu_poll(&m, false, 77050) == HELTEC_MENU_NONE && m.selected == 0);
    m.selected = 4;
    assert(press(&m, 78000, 800) == HELTEC_MENU_NODES);
    m.open = true; m.selected = 5;
    assert(!strcmp(heltec_button_menu_label(&m), "CHANNEL"));
    assert(press(&m, 80000, 800) == HELTEC_MENU_CHANNEL);
    m.open = true; m.selected = 5;
    assert(press(&m, 82000, 100) == HELTEC_MENU_NONE && m.selected == 0);
    m.open=false; m.browsing=true; m.hold_action=true;
    assert(heltec_button_menu_poll(&m,true,84000)==HELTEC_MENU_NONE);
    assert(heltec_button_menu_poll(&m,true,84050)==HELTEC_MENU_NONE);
    assert(heltec_button_menu_poll(&m,true,84550)==HELTEC_MENU_SELECT);
    assert(heltec_button_menu_poll(&m,true,85000)==HELTEC_MENU_NONE);
    assert(heltec_button_menu_poll(&m,false,85100)==HELTEC_MENU_NONE);
    assert(heltec_button_menu_poll(&m,false,85150)==HELTEC_MENU_NONE && !m.open);
    return 0;
}
