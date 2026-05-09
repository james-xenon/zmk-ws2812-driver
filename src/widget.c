#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/led_strip.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

#if IS_ENABLED(CONFIG_ZMK_EXT_POWER)
#include <drivers/ext_power.h>
#endif

#if IS_ENABLED(CONFIG_ZMK_RGB_UNDERGLOW)
#include <zmk/rgb_underglow.h>
#endif

#include <zmk/event_manager.h>
#include <zmk/events/layer_state_changed.h>
#include <zmk/events/position_state_changed.h>
#include <zmk/events/sensor_event.h>

#if IS_ENABLED(CONFIG_ZMK_BATTERY_REPORTING)
#include <zmk/battery.h>
#endif

#include <zmk_ws2812_widget/widget.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#define WS2812_STRIP_NODE DT_CHOSEN(zmk_ws2812_widget)

#if !DT_NODE_EXISTS(WS2812_STRIP_NODE)
#error "WS2812 widget chosen node zmk,ws2812-widget not found"
#endif

#define NUM_PIXELS DT_PROP(WS2812_STRIP_NODE, chain_length)

static const struct device *led_strip = DEVICE_DT_GET(WS2812_STRIP_NODE);

struct blink_pattern {
    struct led_rgb color;
    uint16_t duration_ms;
    uint16_t pause_ms;
    uint8_t repeat_count;
    bool smooth;
};

static bool initialized = false;
static bool indication_enabled = IS_ENABLED(CONFIG_WS2812_WIDGET_ENABLED_ON_START);

static int64_t battery_quiet_until_ms = 0;
static int64_t last_activity_ms = 0;

static struct led_rgb hex_to_rgb(uint32_t hex_color) {
    return (struct led_rgb){
        .r = (hex_color >> 16) & 0xFF,
        .g = (hex_color >> 8) & 0xFF,
        .b = hex_color & 0xFF,
    };
}

static struct led_rgb scale_rgb(struct led_rgb color, uint8_t level, uint8_t max_level) {
    if (max_level == 0) {
        return (struct led_rgb){0, 0, 0};
    }

    return (struct led_rgb){
        .r = (color.r * level) / max_level,
        .g = (color.g * level) / max_level,
        .b = (color.b * level) / max_level,
    };
}

static int set_leds_color(struct led_rgb color) {
    struct led_rgb pixels[NUM_PIXELS];

    for (int i = 0; i < NUM_PIXELS; i++) {
        pixels[i] = color;
    }

    return led_strip_update_rgb(led_strip, pixels, NUM_PIXELS);
}

static void turn_leds_off(void) {
    set_leds_color((struct led_rgb){0, 0, 0});
}

static bool inactivity_suppresses_indication(void) {
#if IS_ENABLED(CONFIG_WS2812_WIDGET_AUTO_DISABLE_AFTER_INACTIVITY)
    if (last_activity_ms <= 0) {
        return false;
    }

    return (k_uptime_get() - last_activity_ms) >= CONFIG_WS2812_WIDGET_INACTIVITY_DISABLE_MS;
#else
    return false;
#endif
}

static bool indication_allowed(void) {
    return indication_enabled && !inactivity_suppresses_indication();
}

void ws2812_set_indication_enabled(bool enabled) {
    indication_enabled = enabled;

    if (!enabled) {
        k_msgq_purge(&led_msgq);
        battery_quiet_until_ms = INT64_MAX;
        LOG_INF("WS2812 indication disabled");
    } else {
        battery_quiet_until_ms = 0;
        last_activity_ms = k_uptime_get();
        LOG_INF("WS2812 indication enabled");
    }
}

void ws2812_toggle_indication_enabled(void) {
    ws2812_set_indication_enabled(!indication_enabled);
}

bool ws2812_get_indication_enabled(void) {
    return indication_enabled;
}

#if IS_ENABLED(CONFIG_WS2812_WIDGET_PAUSE_RGB_UNDERGLOW)

static bool pause_rgb_underglow_for_blink(void) {
    bool rgb_was_on = false;

    int rc = zmk_rgb_underglow_get_state(&rgb_was_on);

    if (rc < 0) {
        LOG_WRN("WS2812 widget: failed to read RGB underglow state: %d", rc);
        return false;
    }

    if (!rgb_was_on) {
        return false;
    }

    rc = zmk_rgb_underglow_off();

    if (rc < 0) {
        LOG_WRN("WS2812 widget: failed to pause RGB underglow: %d", rc);
        return false;
    }

    k_sleep(K_MSEC(CONFIG_WS2812_WIDGET_UNDERGLOW_OFF_DELAY_MS));

    return true;
}

static void restore_rgb_underglow_after_blink(bool rgb_was_on) {
    if (!rgb_was_on) {
        return;
    }

    int rc = zmk_rgb_underglow_on();

    if (rc < 0) {
        LOG_WRN("WS2812 widget: failed to restore RGB underglow: %d", rc);
        return;
    }

    k_sleep(K_MSEC(CONFIG_WS2812_WIDGET_UNDERGLOW_RESTORE_DELAY_MS));
}

#else

static bool pause_rgb_underglow_for_blink(void) {
    return false;
}

static void restore_rgb_underglow_after_blink(bool rgb_was_on) {
    ARG_UNUSED(rgb_was_on);
}

#endif

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
                        uint8_t repeat_count, bool smooth) {
    if (!indication_allowed()) {
        return;
    }

    struct blink_pattern pattern = {
        .color = color,
        .duration_ms = duration_ms,
        .pause_ms = pause_ms,
        .repeat_count = repeat_count,
        .smooth = smooth,
    };

    int rc = k_msgq_put(&led_msgq, &pattern, K_NO_WAIT);

    if (rc < 0) {
        LOG_WRN("WS2812 blink queue full, dropping blink");
    }
}

static void queue_layer_blink(struct led_rgb color) {
    if (!indication_allowed()) {
        return;
    }

    battery_quiet_until_ms =
        k_uptime_get() + CONFIG_WS2812_WIDGET_BATTERY_COOLDOWN_AFTER_LAYER_MS;

    k_msgq_purge(&led_msgq);

    queue_blink(color,
                CONFIG_WS2812_WIDGET_LAYER_BLINK_MS,
                CONFIG_WS2812_WIDGET_LAYER_BLINK_PAUSE_MS,
                CONFIG_WS2812_WIDGET_LAYER_BLINK_REPEAT,
                IS_ENABLED(CONFIG_WS2812_WIDGET_LAYER_FADE_ENABLE));
}

static void execute_simple_blink(struct blink_pattern pattern) {
    for (int i = 0; i < pattern.repeat_count; i++) {
        if (!indication_allowed()) {
            turn_leds_off();
            return;
        }

        set_leds_color(pattern.color);
        k_sleep(K_MSEC(pattern.duration_ms));

        turn_leds_off();

        if (pattern.pause_ms > 0 && i < pattern.repeat_count - 1) {
            k_sleep(K_MSEC(pattern.pause_ms));
        }
    }
}

static void execute_smooth_blink(struct blink_pattern pattern) {
    uint8_t steps = CONFIG_WS2812_WIDGET_LAYER_FADE_STEPS;

    if (steps < 2 || pattern.duration_ms < 600) {
        execute_simple_blink(pattern);
        return;
    }

    uint16_t fade_total_ms = pattern.duration_ms / 3;
    uint16_t fade_step_ms = fade_total_ms / steps;
    uint16_t hold_ms = pattern.duration_ms - (fade_step_ms * steps * 2);

    if (fade_step_ms < 10) {
        fade_step_ms = 10;
    }

    for (int repeat = 0; repeat < pattern.repeat_count; repeat++) {
        if (!indication_allowed()) {
            turn_leds_off();
            return;
        }

        for (uint8_t step = 1; step <= steps; step++) {
            set_leds_color(scale_rgb(pattern.color, step, steps));
            k_sleep(K_MSEC(fade_step_ms));
        }

        if (hold_ms > 0) {
            set_leds_color(pattern.color);
            k_sleep(K_MSEC(hold_ms));
        }

        for (int step = steps; step >= 0; step--) {
            set_leds_color(scale_rgb(pattern.color, step, steps));
            k_sleep(K_MSEC(fade_step_ms));
        }

        turn_leds_off();

        if (pattern.pause_ms > 0 && repeat < pattern.repeat_count - 1) {
            k_sleep(K_MSEC(pattern.pause_ms));
        }
    }
}

static void execute_blink_pattern(struct blink_pattern pattern) {
    if (!indication_allowed()) {
        return;
    }

    bool rgb_was_on = pause_rgb_underglow_for_blink();
    bool ext_power_was_off = prepare_ext_power_for_blink();

    if (pattern.smooth) {
        execute_smooth_blink(pattern);
    } else {
        execute_simple_blink(pattern);
    }

    turn_leds_off();

    if (rgb_was_on) {
        restore_rgb_underglow_after_blink(true);
    } else {
        restore_ext_power_after_blink(ext_power_was_off);
    }
}

static bool watched_layer(uint8_t layer) {
    return layer == CONFIG_WS2812_WIDGET_LAYER_TRIGGER_0 ||
           layer == CONFIG_WS2812_WIDGET_LAYER_TRIGGER_1 ||
           layer == CONFIG_WS2812_WIDGET_LAYER_TRIGGER_2;
}

void ws2812_indicate_layer(void) {
#if IS_ENABLED(CONFIG_WS2812_WIDGET_SHOW_LAYER_CHANGE)
    queue_layer_blink(hex_to_rgb(CONFIG_WS2812_WIDGET_LAYER_COLOR_MANUAL));
#endif
}

#if IS_ENABLED(CONFIG_WS2812_WIDGET_SHOW_LAYER_CHANGE)
#if !IS_ENABLED(CONFIG_ZMK_SPLIT) || IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)

static int led_layer_listener_cb(const zmk_event_t *eh) {
    const struct zmk_layer_state_changed *ev = as_zmk_layer_state_changed(eh);

    if (ev == NULL || !initialized || !indication_allowed()) {
        return 0;
    }

    if (!watched_layer(ev->layer)) {
        return 0;
    }

    k_sleep(K_MSEC(CONFIG_WS2812_WIDGET_LAYER_DEBOUNCE_MS));

    struct led_rgb color = ev->state ? hex_to_rgb(CONFIG_WS2812_WIDGET_LAYER_COLOR_ON)
                                     : hex_to_rgb(CONFIG_WS2812_WIDGET_LAYER_COLOR_OFF);

    queue_layer_blink(color);

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
    if (!indication_allowed()) {
        return;
    }

    if (k_uptime_get() < battery_quiet_until_ms) {
        return;
    }

    uint8_t battery_level = read_battery_level_with_retry();

    if (battery_level == 0) {
        LOG_WRN("WS2812 battery warning skipped: battery level unavailable");
        return;
    }

    if (battery_level <= CONFIG_WS2812_WIDGET_BATTERY_LEVEL_CRITICAL) {
        queue_blink(hex_to_rgb(CONFIG_WS2812_WIDGET_BATTERY_COLOR_CRITICAL),
                    CONFIG_WS2812_WIDGET_BATTERY_BLINK_MS,
                    CONFIG_WS2812_WIDGET_BATTERY_BLINK_PAUSE_MS,
                    CONFIG_WS2812_WIDGET_BATTERY_BLINK_REPEAT,
                    false);

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

static int ws2812_position_listener_cb(const zmk_event_t *eh) {
    const struct zmk_position_state_changed *ev = as_zmk_position_state_changed(eh);

    if (ev != NULL && ev->state) {
        last_activity_ms = k_uptime_get();

        if (indication_enabled) {
            battery_quiet_until_ms = 0;
        }
    }

    return 0;
}

ZMK_LISTENER(ws2812_position_listener, ws2812_position_listener_cb);
ZMK_SUBSCRIPTION(ws2812_position_listener, zmk_position_state_changed);

static int ws2812_sensor_listener_cb(const zmk_event_t *eh) {
    const struct zmk_sensor_event *ev = as_zmk_sensor_event(eh);

    if (ev != NULL) {
        last_activity_ms = k_uptime_get();

        if (indication_enabled) {
            battery_quiet_until_ms = 0;
        }
    }

    return 0;
}

ZMK_LISTENER(ws2812_sensor_listener, ws2812_sensor_listener_cb);
ZMK_SUBSCRIPTION(ws2812_sensor_listener, zmk_sensor_event);

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

    last_activity_ms = k_uptime_get();

    turn_leds_off();

    initialized = true;

#if IS_ENABLED(CONFIG_ZMK_BATTERY_REPORTING) && IS_ENABLED(CONFIG_WS2812_WIDGET_SHOW_BATTERY)
    k_work_init_delayable(&battery_periodic_work, battery_periodic_cb);

    k_work_reschedule(&battery_periodic_work,
                      K_MSEC(CONFIG_WS2812_WIDGET_BATTERY_PERIODIC_MS));
#endif

    LOG_INF("WS2812 blink/fade widget initialized with %d pixels", NUM_PIXELS);
}

K_THREAD_DEFINE(led_init_tid, 1024, led_init_thread, NULL, NULL, NULL,
                K_LOWEST_APPLICATION_THREAD_PRIO, 0, 200);
