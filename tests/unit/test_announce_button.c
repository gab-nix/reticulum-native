/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "announce_button.h"
#include <assert.h>
int main(void) {
    heltec_announce_button b = {0};
    assert(!heltec_announce_button_poll(&b, true, 0));
    assert(!heltec_announce_button_poll(&b, true, 100));
    assert(!heltec_announce_button_poll(&b, false, 200));
    assert(!heltec_announce_button_poll(&b, false, 300));
    assert(!heltec_announce_button_poll(&b, true, 400));
    assert(!heltec_announce_button_poll(&b, false, 410));
    assert(!heltec_announce_button_poll(&b, true, 420));
    assert(!heltec_announce_button_poll(&b, true, 500));
    assert(!heltec_announce_button_poll(&b, true, 1500));
    assert(!heltec_announce_button_poll(&b, false, 1600));
    assert(heltec_announce_button_poll(&b, false, 1650));
    assert(!heltec_announce_button_poll(&b, true, 1700));
    assert(!heltec_announce_button_poll(&b, true, 1750));
    assert(!heltec_announce_button_poll(&b, false, 1800));
    assert(!heltec_announce_button_poll(&b, false, 1850));
    assert(!heltec_announce_button_poll(&b, true, 62000));
    assert(!heltec_announce_button_poll(&b, true, 62100));
    assert(!heltec_announce_button_poll(&b, false, 62200));
    assert(heltec_announce_button_poll(&b, false, 62300));
    return 0;
}
