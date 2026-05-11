/*
 * WS2812 temporary indicator widget for ZMK.
 *
 * Driver-only version:
 * - does not patch ZMK core;
 * - does not implement persistent layer colors;
 * - uses temporary overlay indications;
 * - guards layer-state subscription so peripheral split builds link correctly.
 */

#include <string.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/led_strip.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

#include <zmk/activity.h>
#include <zmk/events/activity_state_changed.h>

#if IS_ENABLED(CONFIG_ZMK_BATTERY_REPORTING)
#include <zmk/battery.h>
#include <zmk/events/battery_state_changed.h>
#endif

#if IS_ENABLED(CONFIG_ZMK_RGB_UNDERGLOW)
#include <zmk/rgb_underglow.h>
#endif

#if IS_ENABLED(CONFIG_WS2812_WIDGET_SHOW_LAYER_CHANGE) &&                                      \
    (!IS_ENABLED(CONFIG_ZMK_SPLIT) || IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL))
#include <zmk/events/layer_state_changed.h>
#include <zmk/keymap.h>
#endif

#include <zmk_ws2812_widget/widget.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#define WS2812_STRIP_NODE DT_CHOSEN(zmk_ws2812_widget)

#if !DT_NODE_EXISTS(WS2812_STRIP_NODE)
#error "WS2812 widget chosen node zmk,ws2812-widget not found"
#endif

#define WS2812_NUM_PIXELS DT_PROP(WS2812_STRIP_NODE, chain_length)

BUILD_ASSERT(CONFIG_WS2812_WIDGET_FADE_STEP_MS > 0,
             "CONFIG_WS2812_WIDGET_FADE_STEP_MS must be greater than zero");

enum indicator_kind {
    INDICATOR_KIND_MANUAL_LAYER,
    INDICATOR_KIND_LAYER_ON,
    INDICATOR_KIND_LAYER_OFF,
    INDICATOR_KIND_BATTERY_MANUAL,
    INDICATOR_KIND_BATTERY_CRITICAL,
    INDICATOR_KIND_CONNECTIVITY,
};

struct indicator_request {
    enum indicator_kind kind;
    struct led_rgb color;
    uint16_t fade_in_ms;
    uint16_t hold_ms;
    uint16_t fade_out_ms;
    uint16_t gap_ms;
    uint8_t repeat_count;
};

static const struct device *const led_strip = DEVICE_DT_GET(WS2812_STRIP_NODE);
static struct led_rgb pixels[WS2812_NUM_PIXELS];

static bool initialized;
static bool widget_enabled = IS_ENABLED(CONFIG_WS2812_WIDGET_ENABLED_ON_START);
static bool activity_active = true;
static int64_t last_activity_ms;
static int64_t last_layer_indication_ms;

K_MSGQ_DEFINE(indicator_msgq, sizeof(struct indicator_request), 12, 4);

static struct led_rgb hex_to_rgb(uint32_t hex_color) {
    return (struct led_rgb){
        .r = (hex_color >> 16) & 0xFF,
        .g = (hex_color >> 8) & 0xFF,
        .b = hex_color & 0xFF,
    };
}

static int set_all_pixels(struct led_rgb color) {
    for (int i = 0; i < WS2812_NUM_PIXELS; i++) {
        pixels[i] = color;
    }

    return led_strip_update_rgb(led_strip, pixels, WS2812_NUM_PIXELS);
}

static struct led_rgb scale_rgb(struct led_rgb color, uint16_t numerator, uint16_t denominator) {
    if (denominator == 0) {
        return color;
    }

    return (struct led_rgb){
        .r = (uint8_t)(((uint16_t)color.r * numerator) / denominator),
        .g = (uint8_t)(((uint16_t)color.g * numerator) / denominator),
        .b = (uint8_t)(((uint16_t)color.b * numerator) / denominator),
    };
}

static uint16_t fade_step_count(uint16_t duration_ms) {
    if (duration_ms == 0) {
        return 0;
    }

    return MAX(1, duration_ms / CONFIG_WS2812_WIDGET_FADE_STEP_MS);
}

static void fade_from_black_to_color(struct led_rgb color, uint16_t duration_ms) {
    uint16_t steps = fade_step_count(duration_ms);

    if (steps == 0) {
        set_all_pixels(color);
        return;
    }

    uint16_t delay_ms = MAX(1, duration_ms / steps);

    for (uint16_t step = 0; step <= steps; step++) {
        set_all_pixels(scale_rgb(color, step, steps));
        k_sleep(K_MSEC(delay_ms));
    }
}

static void fade_from_color_to_black(struct led_rgb color, uint16_t duration_ms) {
    uint16_t steps = fade_step_count(duration_ms);

    if (steps == 0) {
        set_all_pixels((struct led_rgb){0, 0, 0});
        return;
    }

    uint16_t delay_ms = MAX(1, duration_ms / steps);

    for (uint16_t step = 0; step <= steps; step++) {
        set_all_pixels(scale_rgb(color, steps - step, steps));
        k_sleep(K_MSEC(delay_ms));
    }
}

static bool periodic_indication_allowed(void) {
#if IS_ENABLED(CONFIG_WS2812_WIDGET_AUTO_DISABLE_AFTER_INACTIVITY)
    int64_t now = k_uptime_get();

    if (last_activity_ms > 0 &&
        now - last_activity_ms > CONFIG_WS2812_WIDGET_INACTIVITY_DISABLE_MS) {
        return false;
    }
#endif

    return true;
}

static bool indication_allowed(bool periodic) {
    if (!initialized || !widget_enabled || !activity_active) {
        return false;
    }

    if (periodic && !periodic_indication_allowed()) {
        return false;
    }

    return true;
}

void ws2812_note_activity(void) {
    last_activity_ms = k_uptime_get();
}

void ws2812_set_indication_enabled(bool enabled) {
    widget_enabled = enabled;
    ws2812_note_activity();

    if (!enabled && initialized) {
        set_all_pixels((struct led_rgb){0, 0, 0});
    }

    LOG_INF("WS2812 indications %s", enabled ? "enabled" : "disabled");
}

void ws2812_toggle_indication_enabled(void) {
    ws2812_set_indication_enabled(!widget_enabled);
}

static bool pause_underglow_if_needed(void) {
    bool was_on = false;

#if IS_ENABLED(CONFIG_WS2812_WIDGET_PAUSE_RGB_UNDERGLOW) && IS_ENABLED(CONFIG_ZMK_RGB_UNDERGLOW)
    if (zmk_rgb_underglow_get_state(&was_on) == 0 && was_on) {
        zmk_rgb_underglow_off();
        k_sleep(K_MSEC(CONFIG_WS2812_WIDGET_UNDERGLOW_OFF_DELAY_MS));
    }
#endif

    return was_on;
}

static void restore_underglow_if_needed(bool was_on) {
#if IS_ENABLED(CONFIG_WS2812_WIDGET_PAUSE_RGB_UNDERGLOW) && IS_ENABLED(CONFIG_ZMK_RGB_UNDERGLOW)
    if (was_on) {
        k_sleep(K_MSEC(CONFIG_WS2812_WIDGET_UNDERGLOW_RESTORE_DELAY_MS));
        zmk_rgb_underglow_on();
        return;
    }
#else
    ARG_UNUSED(was_on);
#endif

    set_all_pixels((struct led_rgb){0, 0, 0});
}

static void execute_indicator_request(const struct indicator_request *request) {
    bool underglow_was_on = pause_underglow_if_needed();

    for (uint8_t i = 0; i < request->repeat_count; i++) {
        fade_from_black_to_color(request->color, request->fade_in_ms);

        if (request->hold_ms > 0) {
            set_all_pixels(request->color);
            k_sleep(K_MSEC(request->hold_ms));
        }

        fade_from_color_to_black(request->color, request->fade_out_ms);
        set_all_pixels((struct led_rgb){0, 0, 0});

        if (i + 1 < request->repeat_count && request->gap_ms > 0) {
            k_sleep(K_MSEC(request->gap_ms));
        }
    }

    restore_underglow_if_needed(underglow_was_on);
}

static void enqueue_indicator(struct indicator_request request, bool periodic) {
    if (!periodic) {
        ws2812_note_activity();
    }

    if (!indication_allowed(periodic)) {
        return;
    }

    int rc = k_msgq_put(&indicator_msgq, &request, K_NO_WAIT);
    if (rc != 0) {
        LOG_WRN("WS2812 indicator queue full, dropping request kind %d", request.kind);
    }
}

static struct indicator_request make_layer_request(bool enabled) {
    return (struct indicator_request){
        .kind = enabled ? INDICATOR_KIND_LAYER_ON : INDICATOR_KIND_LAYER_OFF,
        .color = hex_to_rgb(enabled ? CONFIG_WS2812_WIDGET_LAYER_COLOR_ON
                                    : CONFIG_WS2812_WIDGET_LAYER_COLOR_OFF),
        .fade_in_ms = CONFIG_WS2812_WIDGET_LAYER_FADE_IN_MS,
        .hold_ms = CONFIG_WS2812_WIDGET_LAYER_HOLD_MS,
        .fade_out_ms = CONFIG_WS2812_WIDGET_LAYER_FADE_OUT_MS,
        .gap_ms = CONFIG_WS2812_WIDGET_LAYER_BLINK_PAUSE_MS,
        .repeat_count = CONFIG_WS2812_WIDGET_LAYER_REPEAT_COUNT,
    };
}

static struct indicator_request make_manual_layer_request(void) {
    return (struct indicator_request){
        .kind = INDICATOR_KIND_MANUAL_LAYER,
        .color = hex_to_rgb(CONFIG_WS2812_WIDGET_LAYER_COLOR_MANUAL),
        .fade_in_ms = CONFIG_WS2812_WIDGET_LAYER_FADE_IN_MS,
        .hold_ms = CONFIG_WS2812_WIDGET_LAYER_HOLD_MS,
        .fade_out_ms = CONFIG_WS2812_WIDGET_LAYER_FADE_OUT_MS,
        .gap_ms = CONFIG_WS2812_WIDGET_LAYER_BLINK_PAUSE_MS,
        .repeat_count = CONFIG_WS2812_WIDGET_LAYER_REPEAT_COUNT,
    };
}

void ws2812_indicate_layer(void) {
#if IS_ENABLED(CONFIG_WS2812_WIDGET_SHOW_LAYER_CHANGE)
    last_layer_indication_ms = k_uptime_get();
    enqueue_indicator(make_manual_layer_request(), false);
#endif
}

#if IS_ENABLED(CONFIG_WS2812_WIDGET_SHOW_LAYER_CHANGE) &&                                      \
    (!IS_ENABLED(CONFIG_ZMK_SPLIT) || IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL))

static bool layer_is_explicit_trigger(uint8_t layer) {
    return (CONFIG_WS2812_WIDGET_LAYER_TRIGGER_0 >= 0 &&
            layer == CONFIG_WS2812_WIDGET_LAYER_TRIGGER_0) ||
           (CONFIG_WS2812_WIDGET_LAYER_TRIGGER_1 >= 0 &&
            layer == CONFIG_WS2812_WIDGET_LAYER_TRIGGER_1) ||
           (CONFIG_WS2812_WIDGET_LAYER_TRIGGER_2 >= 0 &&
            layer == CONFIG_WS2812_WIDGET_LAYER_TRIGGER_2) ||
           (CONFIG_WS2812_WIDGET_LAYER_TRIGGER_3 >= 0 &&
            layer == CONFIG_WS2812_WIDGET_LAYER_TRIGGER_3);
}

static bool any_explicit_layer_triggers_configured(void) {
    return CONFIG_WS2812_WIDGET_LAYER_TRIGGER_0 >= 0 || CONFIG_WS2812_WIDGET_LAYER_TRIGGER_1 >= 0 ||
           CONFIG_WS2812_WIDGET_LAYER_TRIGGER_2 >= 0 || CONFIG_WS2812_WIDGET_LAYER_TRIGGER_3 >= 0;
}

static bool layer_should_trigger(uint8_t layer) {
    if (layer == zmk_keymap_layer_default()) {
        return false;
    }

    if (any_explicit_layer_triggers_configured()) {
        return layer_is_explicit_trigger(layer);
    }

    if (layer >= 32) {
        return false;
    }

    return (CONFIG_WS2812_WIDGET_LAYER_INDICATOR_MASK & BIT(layer)) != 0;
}

static struct k_work_delayable layer_indicator_work;
static bool pending_layer_state;
static bool pending_layer_valid;

static void layer_indicator_work_cb(struct k_work *work) {
    ARG_UNUSED(work);

    if (!pending_layer_valid) {
        return;
    }

    bool state = pending_layer_state;
    pending_layer_valid = false;

    last_layer_indication_ms = k_uptime_get();
    enqueue_indicator(make_layer_request(state), false);
}

static int layer_listener_cb(const zmk_event_t *eh) {
    const struct zmk_layer_state_changed *ev = as_zmk_layer_state_changed(eh);

    if (!initialized || ev == NULL || !layer_should_trigger(ev->layer)) {
        return 0;
    }

    pending_layer_state = ev->state;
    pending_layer_valid = true;

    k_work_reschedule(&layer_indicator_work, K_MSEC(CONFIG_WS2812_WIDGET_LAYER_DEBOUNCE_MS));

    return 0;
}

ZMK_LISTENER(ws2812_layer_listener, layer_listener_cb);
ZMK_SUBSCRIPTION(ws2812_layer_listener, zmk_layer_state_changed);
#endif

#if IS_ENABLED(CONFIG_WS2812_WIDGET_SHOW_BATTERY)

static struct led_rgb get_battery_status_color(uint8_t battery_level) {
    if (battery_level == 0) {
        return hex_to_rgb(CONFIG_WS2812_WIDGET_COLOR_OFF);
    }

    if (battery_level <= CONFIG_WS2812_WIDGET_BATTERY_LEVEL_CRITICAL) {
        return hex_to_rgb(CONFIG_WS2812_WIDGET_BATTERY_COLOR_CRITICAL);
    }

    if (battery_level <= CONFIG_WS2812_WIDGET_BATTERY_LEVEL_LOW) {
        return hex_to_rgb(CONFIG_WS2812_WIDGET_BATTERY_COLOR_LOW);
    }

    if (battery_level >= CONFIG_WS2812_WIDGET_BATTERY_LEVEL_HIGH) {
        return hex_to_rgb(CONFIG_WS2812_WIDGET_BATTERY_COLOR_HIGH);
    }

    return hex_to_rgb(CONFIG_WS2812_WIDGET_BATTERY_COLOR_MEDIUM);
}

static struct indicator_request make_battery_request(struct led_rgb color, uint8_t repeat_count,
                                                     enum indicator_kind kind) {
    return (struct indicator_request){
        .kind = kind,
        .color = color,
        .fade_in_ms = CONFIG_WS2812_WIDGET_BATTERY_FADE_IN_MS,
        .hold_ms = CONFIG_WS2812_WIDGET_BATTERY_HOLD_MS,
        .fade_out_ms = CONFIG_WS2812_WIDGET_BATTERY_FADE_OUT_MS,
        .gap_ms = CONFIG_WS2812_WIDGET_BATTERY_BLINK_PAUSE_MS,
        .repeat_count = repeat_count,
    };
}

void ws2812_indicate_battery(void) {
    uint8_t battery_level = zmk_battery_state_of_charge();
    int retry = 0;

    while (battery_level == 0 && retry++ < 10) {
        k_sleep(K_MSEC(100));
        battery_level = zmk_battery_state_of_charge();
    }

    enqueue_indicator(make_battery_request(get_battery_status_color(battery_level),
                                           CONFIG_WS2812_WIDGET_BATTERY_BLINK_REPEAT,
                                           INDICATOR_KIND_BATTERY_MANUAL),
                      false);
}

static struct k_work_delayable battery_reminder_work;

static void schedule_next_battery_reminder(void) {
    k_work_reschedule(&battery_reminder_work,
                      K_MSEC(CONFIG_WS2812_WIDGET_BATTERY_REMINDER_INTERVAL_MS));
}

static void battery_reminder_work_cb(struct k_work *work) {
    ARG_UNUSED(work);

#if IS_ENABLED(CONFIG_WS2812_WIDGET_BATTERY_REMINDER)
    int64_t now = k_uptime_get();

    if (last_layer_indication_ms > 0 &&
        now - last_layer_indication_ms < CONFIG_WS2812_WIDGET_BATTERY_COOLDOWN_AFTER_LAYER_MS) {
        schedule_next_battery_reminder();
        return;
    }

    if (indication_allowed(true)) {
        uint8_t battery_level = zmk_battery_state_of_charge();

        if (battery_level > 0 && battery_level <= CONFIG_WS2812_WIDGET_BATTERY_LEVEL_CRITICAL) {
            enqueue_indicator(make_battery_request(hex_to_rgb(CONFIG_WS2812_WIDGET_BATTERY_REMINDER_COLOR),
                                                   CONFIG_WS2812_WIDGET_BATTERY_REMINDER_REPEAT_COUNT,
                                                   INDICATOR_KIND_BATTERY_CRITICAL),
                              true);
        }
    }
#endif

    schedule_next_battery_reminder();
}

#if IS_ENABLED(CONFIG_ZMK_BATTERY_REPORTING)
static int battery_listener_cb(const zmk_event_t *eh) {
    const struct zmk_battery_state_changed *ev = as_zmk_battery_state_changed(eh);

    if (!initialized || ev == NULL) {
        return 0;
    }

    if (ev->state_of_charge > 0 &&
        ev->state_of_charge <= CONFIG_WS2812_WIDGET_BATTERY_LEVEL_CRITICAL) {
        enqueue_indicator(make_battery_request(hex_to_rgb(CONFIG_WS2812_WIDGET_BATTERY_COLOR_CRITICAL),
                                               CONFIG_WS2812_WIDGET_BATTERY_CRITICAL_REPEAT_COUNT,
                                               INDICATOR_KIND_BATTERY_CRITICAL),
                          false);
    }

    return 0;
}

ZMK_LISTENER(ws2812_battery_listener, battery_listener_cb);
ZMK_SUBSCRIPTION(ws2812_battery_listener, zmk_battery_state_changed);
#endif
#else
void ws2812_indicate_battery(void) {}
#endif

void ws2812_indicate_connectivity(void) {
#if IS_ENABLED(CONFIG_WS2812_WIDGET_SHOW_CONNECTIVITY)
    struct indicator_request request = {
        .kind = INDICATOR_KIND_CONNECTIVITY,
        .color = hex_to_rgb(CONFIG_WS2812_WIDGET_CONN_COLOR_CONNECTED),
        .fade_in_ms = CONFIG_WS2812_WIDGET_PRE_FADE_MS,
        .hold_ms = CONFIG_WS2812_WIDGET_CONN_BLINK_MS,
        .fade_out_ms = CONFIG_WS2812_WIDGET_POST_FADE_MS,
        .gap_ms = CONFIG_WS2812_WIDGET_INTERVAL_MS,
        .repeat_count = 1,
    };

    enqueue_indicator(request, false);
#endif
}

static int activity_listener_cb(const zmk_event_t *eh) {
    const struct zmk_activity_state_changed *ev = as_zmk_activity_state_changed(eh);

    if (ev == NULL) {
        return 0;
    }

    if (ev->state == ZMK_ACTIVITY_ACTIVE) {
        activity_active = true;
        ws2812_note_activity();
    } else if (ev->state == ZMK_ACTIVITY_SLEEP) {
        activity_active = false;
        set_all_pixels((struct led_rgb){0, 0, 0});
    }

    return 0;
}

ZMK_LISTENER(ws2812_activity_listener, activity_listener_cb);
ZMK_SUBSCRIPTION(ws2812_activity_listener, zmk_activity_state_changed);

static void indicator_process_thread(void *d0, void *d1, void *d2) {
    ARG_UNUSED(d0);
    ARG_UNUSED(d1);
    ARG_UNUSED(d2);

#if IS_ENABLED(CONFIG_WS2812_WIDGET_SHOW_LAYER_CHANGE) &&                                      \
    (!IS_ENABLED(CONFIG_ZMK_SPLIT) || IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL))
    k_work_init_delayable(&layer_indicator_work, layer_indicator_work_cb);
#endif

#if IS_ENABLED(CONFIG_WS2812_WIDGET_SHOW_BATTERY)
    k_work_init_delayable(&battery_reminder_work, battery_reminder_work_cb);
#endif

    while (true) {
        struct indicator_request request;

        k_msgq_get(&indicator_msgq, &request, K_FOREVER);
        execute_indicator_request(&request);

        if (CONFIG_WS2812_WIDGET_INTERVAL_MS > 0) {
            k_sleep(K_MSEC(CONFIG_WS2812_WIDGET_INTERVAL_MS));
        }
    }
}

K_THREAD_DEFINE(ws2812_indicator_process_tid, 1536, indicator_process_thread, NULL, NULL, NULL,
                K_LOWEST_APPLICATION_THREAD_PRIO, 0, 100);

static void indicator_init_thread(void *d0, void *d1, void *d2) {
    ARG_UNUSED(d0);
    ARG_UNUSED(d1);
    ARG_UNUSED(d2);

    if (!device_is_ready(led_strip)) {
        LOG_ERR("WS2812 LED strip device is not ready");
        return;
    }

    initialized = true;
    ws2812_note_activity();

    set_all_pixels((struct led_rgb){0, 0, 0});

    LOG_INF("WS2812 temporary indicator initialized with %d pixels", WS2812_NUM_PIXELS);

#if IS_ENABLED(CONFIG_WS2812_WIDGET_SHOW_BATTERY)
#if IS_ENABLED(CONFIG_WS2812_WIDGET_SHOW_BATTERY_ON_START)
    ws2812_indicate_battery();
#endif

#if IS_ENABLED(CONFIG_WS2812_WIDGET_BATTERY_REMINDER)
    schedule_next_battery_reminder();
#endif
#endif
}

K_THREAD_DEFINE(ws2812_indicator_init_tid, 1024, indicator_init_thread, NULL, NULL, NULL,
                K_LOWEST_APPLICATION_THREAD_PRIO, 0, 200);
