#include <zephyr/kernel.h>
#include <zmk/event_manager.h>
#include <zmk/events/keycode_state_changed.h>
#include "widget.h"


static bool ws2812_activity_listener_cb(const zmk_event_t *eh)
{
    const struct zmk_keycode_state_changed *ev =
        as_zmk_keycode_state_changed(eh);

    if (ev != NULL && ev->state) {

        ws2812_note_activity();

    }

    return false;
}


ZMK_LISTENER(ws2812_activity_listener,
             ws2812_activity_listener_cb);

ZMK_SUBSCRIPTION(ws2812_activity_listener,
                 zmk_keycode_state_changed);