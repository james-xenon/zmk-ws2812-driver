#include <zephyr/kernel.h>

#include <zmk/event_manager.h>
#include <zmk/events/activity_state_changed.h>

#include "widget.h"


static bool ws2812_activity_listener(const zmk_event_t *eh)
{
    const struct zmk_activity_state_changed *ev =
        as_zmk_activity_state_changed(eh);


    if (ev != NULL)
    {
        ws2812_note_activity();
    }


    return false;
}


ZMK_LISTENER(ws2812_activity_listener,
             ws2812_activity_listener);

ZMK_SUBSCRIPTION(ws2812_activity_listener,
                 zmk_activity_state_changed);