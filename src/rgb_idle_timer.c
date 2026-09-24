#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <zmk_ws2812_widget/widget.h>

#include <zmk_ws2812_widget/widget.h>


LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);


static struct k_timer rgb_idle_timer;


static bool timer_enabled = true;

static uint32_t timeout_minutes = 15;



static void rgb_idle_timer_handler(struct k_timer *timer)
{
    ARG_UNUSED(timer);

    if (!timer_enabled) {
        return;
    }


    ws2812_set_indication_enabled(false);
}



void ws2812_idle_timer_init(void)
{
    k_timer_init(
        &rgb_idle_timer,
        rgb_idle_timer_handler,
        NULL
    );


    k_timer_start(
        &rgb_idle_timer,
        K_MINUTES(timeout_minutes),
        K_FOREVER
    );
}



void ws2812_idle_timer_reset(void)
{
    if (!timer_enabled) {
        return;
    }


    ws2812_set_indication_enabled(true);


    k_timer_start(
        &rgb_idle_timer,
        K_MINUTES(timeout_minutes),
        K_FOREVER
    );
}



void ws2812_idle_timer_toggle(void)
{
    timer_enabled = !timer_enabled;


    if (timer_enabled) {

        ws2812_idle_timer_reset();

    }
    else {

        k_timer_stop(&rgb_idle_timer);

        zmk_rgb_underglow_on();

    }
}



void ws2812_idle_timeout_change(int8_t direction)
{

    if (direction > 0) {

        timeout_minutes++;

    }
    else {

        if (timeout_minutes > 1) {
            timeout_minutes--;
        }

    }


    if (timer_enabled) {
        ws2812_idle_timer_reset();
    }
}



bool ws2812_idle_timer_enabled(void)
{
    return timer_enabled;
}