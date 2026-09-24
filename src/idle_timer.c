#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <zmk_ws2812_widget/widget.h>

#if IS_ENABLED(CONFIG_ZMK_RGB_UNDERGLOW)
#include <zmk/rgb_underglow.h>
#endif

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);


static struct k_timer rgb_idle_timer;

static bool timer_enabled = true;
static bool rgb_was_on = false;

static uint32_t timeout_minutes = 15;


static void rgb_idle_timeout_handler(struct k_timer *timer)
{
    if (!timer_enabled) {
        return;
    }

#if IS_ENABLED(CONFIG_ZMK_RGB_UNDERGLOW)

    bool state = false;

    if (zmk_rgb_underglow_get_state(&state) == 0) {

        rgb_was_on = state;

        if (state) {
            zmk_rgb_underglow_off();
        }
    }

#endif
}


void ws2812_idle_timer_init(void)
{
    k_timer_init(
        &rgb_idle_timer,
        rgb_idle_timeout_handler,
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

#if IS_ENABLED(CONFIG_ZMK_RGB_UNDERGLOW)

    zmk_rgb_underglow_on();

#endif

    k_timer_start(
        &rgb_idle_timer,
        K_MINUTES(timeout_minutes),
        K_FOREVER
    );
}


void ws2812_idle_timer_toggle(void)
{
    timer_enabled = !timer_enabled;


    if (!timer_enabled) {

        k_timer_stop(&rgb_idle_timer);

#if IS_ENABLED(CONFIG_ZMK_RGB_UNDERGLOW)

        zmk_rgb_underglow_on();

#endif

    } else {

        ws2812_idle_timer_reset();

    }
}


void ws2812_idle_timeout_change(int8_t direction)
{

    if (direction > 0) {

        timeout_minutes++;

    } else {

        if (timeout_minutes > 1) {
            timeout_minutes--;
        }

    }


    if (timer_enabled) {

        ws2812_idle_timer_reset();

    }
}


uint32_t ws2812_idle_timeout_get(void)
{
    return timeout_minutes;
}