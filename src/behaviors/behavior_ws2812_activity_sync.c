#define DT_DRV_COMPAT zmk_behavior_ws2812_activity_sync

#include <zephyr/device.h>
#include <zephyr/sys/util.h>

#include <drivers/behavior.h>
#include <zmk/behavior.h>

#include <zmk_ws2812_widget/widget.h>


static int behavior_ws2812_activity_sync_init(const struct device *dev)
{
    return 0;
}


/* GLOBAL split display synchronization.
 * param1 = 0 wakes the WS2812 display; param1 = 1 blanks it for idle.
 * GLOBAL locality executes the command on central and every peripheral. */
static int on_keymap_binding_pressed(
    struct zmk_behavior_binding *binding,
    struct zmk_behavior_binding_event event)
{
    ARG_UNUSED(event);

    /* param1 = 0 is an activity heartbeat: wake this half and reset its local
     * idle timer.  param1 = 1 is kept as a compatibility path for an explicit
     * local blank request, although the fixed idle design no longer depends on
     * central-to-peripheral OFF delivery. */
    if (binding->param1 == 0) {
        ws2812_note_activity();
    } else {
        ws2812_apply_idle_sync(true);
    }

    return ZMK_BEHAVIOR_OPAQUE;
}


static int on_keymap_binding_released(
    struct zmk_behavior_binding *binding,
    struct zmk_behavior_binding_event event)
{
    ARG_UNUSED(binding);
    ARG_UNUSED(event);
    return ZMK_BEHAVIOR_OPAQUE;
}


static const struct behavior_driver_api behavior_ws2812_activity_sync_driver_api = {
    .locality = BEHAVIOR_LOCALITY_GLOBAL,

    .binding_pressed = on_keymap_binding_pressed,
    .binding_released = on_keymap_binding_released,

#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_METADATA)
    .get_parameter_metadata = zmk_behavior_get_empty_param_metadata,
#endif
};


#define WS2812_ACTIVITY_SYNC_INST(n) \
    BEHAVIOR_DT_INST_DEFINE(n, \
                            behavior_ws2812_activity_sync_init, \
                            NULL, \
                            NULL, \
                            NULL, \
                            POST_KERNEL, \
                            CONFIG_KERNEL_INIT_PRIORITY_DEFAULT, \
                            &behavior_ws2812_activity_sync_driver_api);


DT_INST_FOREACH_STATUS_OKAY(WS2812_ACTIVITY_SYNC_INST)