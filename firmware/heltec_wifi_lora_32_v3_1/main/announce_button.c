/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "announce_button.h"
bool heltec_announce_button_poll(heltec_announce_button *b, bool pressed, uint64_t now) {
    if (!b) return false;
    if (pressed != b->raw_pressed) { b->raw_pressed = pressed; b->changed_ms = now; }
    if (now < b->changed_ms || now - b->changed_ms < 50U) return false;
    if (!pressed && !b->stable_pressed) { b->armed = true; return false; }
    if (pressed == b->stable_pressed) return false;
    b->stable_pressed = pressed;
    if (pressed) return false;
    bool fire = b->armed && (!b->sent || (now >= b->last_announce_ms && now - b->last_announce_ms >= 60000U));
    b->armed = true;
    if (fire) { b->sent = true; b->last_announce_ms = now; }
    return fire;
}
