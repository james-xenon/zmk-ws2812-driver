#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <zmk/behavior.h>
#include <zmk_ws2812_widget/widget.h>


LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);



static int on_pressed(
    struct zmk_behavior_binding *binding,
    struct zmk_behavior_binding_event event
)
{

    uint32_t command = binding->param1;


    switch(command)
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


    return 0;
}



static int on_released(
    struct zmk_behavior_binding *binding,
    struct zmk_behavior_binding_event event
)
{
    return 0;
}



static const struct behavior_driver_api behavior_rgb_timer_api = {

    .binding_pressed = on_pressed,

    .binding_released = on_released,

};



BEHAVIOR_DT_DEFINE(
    rgb_timer,
    NULL,
    NULL,
    NULL,
    &behavior_rgb_timer_api,
    0,
    CONFIG_KERNEL_INIT_PRIORITY_DEFAULT
);