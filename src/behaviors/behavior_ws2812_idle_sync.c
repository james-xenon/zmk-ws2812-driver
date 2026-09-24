#define DT_DRV_COMPAT zmk_behavior_ws2812_idle_sync

#include <zephyr/device.h>
#include <zephyr/logging/log.h>

#include <drivers/behavior.h>
#include <zmk/behavior.h>

#include <zmk_ws2812_widget/widget.h>


LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);



static int behavior_ws2812_idle_sync_init(const struct device *dev)
{
    return 0;
}



static int on_pressed(
    struct zmk_behavior_binding *binding,
    struct zmk_behavior_binding_event event
)
{

    if (binding->param1 == 0) {

        ws2812_idle_sync_off();

    }
    else {

        ws2812_idle_sync_on();

    }


    return ZMK_BEHAVIOR_OPAQUE;
}



static int on_released(
    struct zmk_behavior_binding *binding,
    struct zmk_behavior_binding_event event
)
{
    return ZMK_BEHAVIOR_OPAQUE;
}



static const struct behavior_driver_api behavior_ws2812_idle_sync_api = {

    .locality = BEHAVIOR_LOCALITY_GLOBAL,

    .binding_pressed = on_pressed,

    .binding_released = on_released,

#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_METADATA)
    .get_parameter_metadata = zmk_behavior_get_empty_param_metadata,
#endif

};



#define WS2812_IDLE_SYNC_INST(n) \
BEHAVIOR_DT_INST_DEFINE( \
    n, \
    behavior_ws2812_idle_sync_init, \
    NULL, \
    NULL, \
    NULL, \
    POST_KERNEL, \
    CONFIG_KERNEL_INIT_PRIORITY_DEFAULT, \
    &behavior_ws2812_idle_sync_api \
);



DT_INST_FOREACH_STATUS_OKAY(WS2812_IDLE_SYNC_INST)