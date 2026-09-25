#include <zephyr/kernel.h>

#include <zmk/event_manager.h>
#include <zmk/events/activity_state_changed.h>

#include <zmk/behavior_queue.h>
#include <zmk/behavior.h>

#include <zmk_ws2812_widget/widget.h>


static int ws2812_send_activity_sync(void)
{
    struct zmk_behavior_binding binding = {
        .behavior_dev = DEVICE_DT_NAME(DT_NODELABEL(ws2812_async)),
        .param1 = 0,
        .param2 = 0,
    };


    struct zmk_behavior_binding_event event = {
        .position = 0,
        .timestamp = k_uptime_get(),
    };


    zmk_behavior_queue_add(
        &event,
        binding,
        true,
        0
    );


    zmk_behavior_queue_add(
        &event,
        binding,
        false,
        10
    );


    return 0;
}



static int activity_listener_cb(const zmk_event_t *eh)
{
    const struct zmk_activity_state_changed *ev =
        as_zmk_activity_state_changed(eh);


    if (ev == NULL) {
        return 0;
    }


    if (ev->state == ZMK_ACTIVITY_ACTIVE) {

        ws2812_note_activity();

        ws2812_send_activity_sync();

    }


    return 0;
}


ZMK_LISTENER(ws2812_activity_listener,
             activity_listener_cb);

ZMK_SUBSCRIPTION(ws2812_activity_listener,
                 zmk_activity_state_changed);