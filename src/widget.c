#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/led_strip.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

#if IS_ENABLED(CONFIG_ZMK_EXT_POWER)
#include <drivers/ext_power.h>
#endif

#include <zmk/event_manager.h>
#include <zmk/keymap.h>
#include <zmk/events/layer_state_changed.h>

#if IS_ENABLED(CONFIG_ZMK_BATTERY_REPORTING)
#include <zmk/battery.h>
#endif

#include <zmk_ws2812_widget/widget.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#define WS2812_STRIP_NODE DT_CHOSEN(zmk_ws2812_widget)

#if !DT_NODE_EXISTS(WS2812_STRIP_NODE)
#error "WS2812 widget chosen node zmk,ws2812-widget not found"
#endif

static const struct device *led_strip = DEVICE_DT_GET(WS2812_STRIP_NODE);
static const uint32_t num_pixels = DT_PROP(WS2812_STRIP_NODE, chain_length);

struct blink_pattern {
    struct led_rgb color;
    uint16_t duration_ms;
    uint16_t pause_ms;
    uint8_t repeat_count;
};

static bool initialized = false;

static struct led_rgb hex_to_rgb(uint32_t hex_color) {
    return (struct led_rgb){
        .r = (hex_color >> 16) & 0xFF,
        .g = (hex_color >> 8) & 0xFF,
        .b = hex_color & 0xFF,
    };
}

static int set_leds_color(struct led_rgb color) {
    struct led_rgb pixels[num_pixels];

    for (int i = 0; i < num_pixels; i++) {
        pixels[i] = color;
    }

    return led_strip_update_rgb(led_strip, pixels, num_pixels);
}

static void turn_leds_off(void) {
    set_leds_color((struct led_rgb){0, 0, 0});
}

#if IS_ENABLED(CONFIG_WS2812_WIDGET_USE_EXT_POWER)
static const struct device *get_ext_power_device(void) {
    const struct device *ext_power = device_get_binding("EXT_POWER");

    if (ext_power == NULL) {
        LOG_WRN("WS2812 widget: EXT_POWER device not found");
    }

    return ext_power;
}

static bool prepare_ext_power_for_blink(void) {
    const struct device *ext_power = get_ext_power_device();

    if (ext_power == NULL) {
        return false;
    }

    int state = ext_power_get(ext_power);
    bool was_off = state <= 0;

    if (was_off) {
        int rc = ext_power_enable(ext_power);

        if (rc < 0) {
            LOG_WRN("WS2812 widget: failed to enable EXT_POWER: %d", rc);
            return false;
        }

        k_sleep(K_MSEC(CONFIG_WS2812_WIDGET_EXT_POWER_STARTUP_DELAY_MS));
    }

    return was_off;
}

static void restore_ext_power_after_blink(bool was_off_before_blink) {
#if IS_ENABLED(CONFIG_WS2812_WIDGET_RESTORE_EXT_POWER_OFF)
    if (!was_off_before_blink) {
        return;
    }

    const struct device *ext_power = get_ext_power_device();

    if (ext_power == NULL) {
        return;
    }

    int rc = ext_power_disable(ext_power);

    if (rc < 0) {
        LOG_WRN("WS2812 widget: failed to restore EXT_POWER off: %d", rc);
    }
#else
    ARG_UNUSED(was_off_before_blink);
#endif
}
#else
static bool prepare_ext_power_for_blink(void) {
    return false;
}

static void restore_ext_power_after_blink(bool was_off_before_blink) {
    ARG_UNUSED(was_off_before_blink);
}
#endif

K_MSGQ_DEFINE(led_msgq, sizeof(struct blink_pattern), 16, 1);

static void queue_blink(struct led_rgb color, uint16_t duration_ms, uint16_t pause_ms,
                        uint8_t repeat_count) {
    struct blink_pattern pattern = {
        .color = color,
        .duration_ms = duration_ms,
        .pause_ms = pause_ms,
        .repeat_count = repeat_count,
    };

    int rc = k_msgq_put(&led_msgq, &pattern, K_NO_WAIT);

    if (rc < 0) {
        LOG_WRN("WS2812 blink queue full, dropping blink");
    }
}

static void execute_blink_pattern(struct blink_pattern pattern) {
    bool ext_power_was_off = prepare_ext_power_for_blink();

    for (int i = 0; i < pattern.repeat_count; i++) {
        set_leds_color(pattern.color);
        k_sleep(K_MSEC(pattern.duration_ms));

        turn_leds_off();

        if (pattern.pause_ms > 0 && i < pattern.repeat_count - 1) {
            k_sleep(K_MSEC(pattern.pause_ms));
        }
    }

    turn_leds_off();
    restore_ext_power_after_blink(ext_power_was_off);
}

static bool watched_layer(uint8_t layer) {
    return layer == CONFIG_WS2812_WIDGET_LAYER_TRIGGER_0 ||
           layer == CONFIG_WS2812_WIDGET_LAYER_TRIGGER_1 ||
           layer == CONFIG_WS2812_WIDGET_LAYER_TRIGGER_2;
}

void ws2812_indicate_layer(void) {
#if IS_ENABLED(CONFIG_WS2812_WIDGET_SHOW_LAYER_CHANGE)
    queue_blink(hex_to_rgb(CONFIG_WS2812_WIDGET_LAYER_COLOR_MANUAL),
                CONFIG_WS2812_WIDGET_LAYER_BLINK_MS,
                CONFIG_WS2812_WIDGET_LAYER_BLINK_PAUSE_MS,
                CONFIG_WS2812_WIDGET_LAYER_BLINK_REPEAT);
#endif
}

#if IS_ENABLED(CONFIG_WS2812_WIDGET_SHOW_LAYER_CHANGE)
#if !IS_ENABLED(CONFIG_ZMK_SPLIT) || IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)

static int led_layer_listener_cb(const zmk_event_t *eh) {
    const struct zmk_layer_state_changed *ev = as_zmk_layer_state_changed(eh);

    if (ev == NULL || !initialized) {
        return 0;
    }

    if (!watched_layer(ev->layer)) {
        return 0;
    }

    k_sleep(K_MSEC(CONFIG_WS2812_WIDGET_LAYER_DEBOUNCE_MS));

    struct led_rgb color = ev->state ? hex_to_rgb(CONFIG_WS2812_WIDGET_LAYER_COLOR_ON)
                                     : hex_to_rgb(CONFIG_WS2812_WIDGET_LAYER_COLOR_OFF);

    queue_blink(color,
                CONFIG_WS2812_WIDGET_LAYER_BLINK_MS,
                CONFIG_WS2812_WIDGET_LAYER_BLINK_PAUSE_MS,
                CONFIG_WS2812_WIDGET_LAYER_BLINK_REPEAT);

    LOG_INF("WS2812 layer blink: layer=%d state=%d", ev->layer, ev->state);

    return 0;
}

ZMK_LISTENER(led_layer_listener, led_layer_listener_cb);
ZMK_SUBSCRIPTION(led_layer_listener, zmk_layer_state_changed);

#endif
#endif

#if IS_ENABLED(CONFIG_ZMK_BATTERY_REPORTING) && IS_ENABLED(CONFIG_WS2812_WIDGET_SHOW_BATTERY)

static struct k_work_delayable battery_periodic_work;

static uint8_t read_battery_level_with_retry(void) {
    uint8_t battery_level = zmk_battery_state_of_charge();

    for (int retry = 0; battery_level == 0 && retry < 10; retry++) {
        k_sleep(K_MSEC(100));
        battery_level = zmk_battery_state_of_charge();
    }

    return battery_level;
}

static void ws2812_warn_if_battery_below_threshold(void) {
    uint8_t battery_level = read_battery_level_with_retry();

    if (battery_level == 0) {
        LOG_WRN("WS2812 battery warning skipped: battery level unavailable");
        return;
    }

    if (battery_level <= CONFIG_WS2812_WIDGET_BATTERY_LEVEL_CRITICAL) {
        queue_blink(hex_to_rgb(CONFIG_WS2812_WIDGET_BATTERY_COLOR_CRITICAL),
                    CONFIG_WS2812_WIDGET_BATTERY_BLINK_MS,
                    CONFIG_WS2812_WIDGET_BATTERY_BLINK_PAUSE_MS,
                    CONFIG_WS2812_WIDGET_BATTERY_BLINK_REPEAT);

        LOG_INF("WS2812 battery warning: level=%d threshold=%d",
                battery_level,
                CONFIG_WS2812_WIDGET_BATTERY_LEVEL_CRITICAL);
    }
}

void ws2812_indicate_battery(void) {
    ws2812_warn_if_battery_below_threshold();
}

static void battery_periodic_cb(struct k_work *work) {
    ARG_UNUSED(work);

    if (initialized) {
        ws2812_warn_if_battery_below_threshold();
    }

    k_work_reschedule(&battery_periodic_work,
                      K_MSEC(CONFIG_WS2812_WIDGET_BATTERY_PERIODIC_MS));
}

#else

void ws2812_indicate_battery(void) {
}

#endif

void ws2812_indicate_connectivity(void) {
}

static void led_process_thread(void *d0, void *d1, void *d2) {
    ARG_UNUSED(d0);
    ARG_UNUSED(d1);
    ARG_UNUSED(d2);

    while (true) {
        struct blink_pattern pattern;

        k_msgq_get(&led_msgq, &pattern, K_FOREVER);
        execute_blink_pattern(pattern);

        k_sleep(K_MSEC(CONFIG_WS2812_WIDGET_INTERVAL_MS));
    }
}

K_THREAD_DEFINE(led_process_tid, 1024, led_process_thread, NULL, NULL, NULL,
                K_LOWEST_APPLICATION_THREAD_PRIO, 0, 100);

static void led_init_thread(void *d0, void *d1, void *d2) {
    ARG_UNUSED(d0);
    ARG_UNUSED(d1);
    ARG_UNUSED(d2);

    if (!device_is_ready(led_strip)) {
        LOG_ERR("WS2812 LED strip device not ready");
        return;
    }

    turn_leds_off();

    initialized = true;

#if IS_ENABLED(CONFIG_ZMK_BATTERY_REPORTING) && IS_ENABLED(CONFIG_WS2812_WIDGET_SHOW_BATTERY)
    k_work_init_delayable(&battery_periodic_work, battery_periodic_cb);

    k_work_reschedule(&battery_periodic_work,
                      K_MSEC(CONFIG_WS2812_WIDGET_BATTERY_PERIODIC_MS));
#endif

    LOG_INF("WS2812 blink-off widget initialized with %d pixels", num_pixels);
}

K_THREAD_DEFINE(led_init_tid, 1024, led_init_thread, NULL, NULL, NULL,
                K_LOWEST_APPLICATION_THREAD_PRIO, 0, 200);
