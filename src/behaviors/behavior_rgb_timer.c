#define DT_DRV_COMPAT zmk_behavior_rgb_timer


#include <zephyr/device.h>

#include <drivers/behavior.h>

#include <zmk/behavior.h>


#include <zmk_ws2812_widget/widget.h>



static int rgb_timer_pressed(
    struct zmk_behavior_binding *binding,
    struct zmk_behavior_binding_event event
)
{

    switch(binding->param1)
    {

    case 0:

        ws2812_idle_timer_toggle();

        break;


    case 1:

        ws2812_idle_timeout_change(1);

        break;


    case 2:

        ws2812_idle_timeout_change(-1);

        break;

    }


    return ZMK_BEHAVIOR_OPAQUE;
}



static int rgb_timer_released(
    struct zmk_behavior_binding *binding,
    struct zmk_behavior_binding_event event
)
{
    return ZMK_BEHAVIOR_OPAQUE;
}



static const struct behavior_driver_api rgb_timer_api = {

    .binding_pressed = rgb_timer_pressed,

    .binding_released = rgb_timer_released,

};



#define RGB_TIMER_INST(n) \
BEHAVIOR_DT_INST_DEFINE( \
n, \
NULL, \
NULL, \
NULL, \
NULL, \
POST_KERNEL, \
CONFIG_KERNEL_INIT_PRIORITY_DEFAULT, \
&rgb_timer_api \
);



DT_INST_FOREACH_STATUS_OKAY(RGB_TIMER_INST)