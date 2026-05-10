#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/drivers/led_strip.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

#include <zmk/activity.h>
#include <zmk/battery.h>
#include <zmk/event_manager.h>
#include <zmk/events/activity_state_changed.h>
#include <zmk/events/battery_state_changed.h>
#include <zmk/events/layer_state_changed.h>
#include <zmk/keymap.h>

#if IS_ENABLED(CONFIG_ZMK_RGB_UNDERGLOW)
#include <zmk/rgb_underglow.h>
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

static const struct device *led_strip = DEVICE_DT_GET(WS2812_STRIP_NODE);

struct indicator_request {
    struct led_rgb color;
    uint16_t fade_in_ms;
    uint16_t hold_ms;
    uint16_t fade_out_ms;
    uint16_t gap_ms;
    uint8_t repeat_count;
};

static struct led_rgb work_pixels[WS2812_NUM_PIXELS];
static struct led_rgb restore_pixels[WS2812_NUM_PIXELS];
static bool initialized;
static bool activity_active = true;

K_MSGQ_DEFINE(indicator_msgq, sizeof(struct indicator_request), 12, 1);

static struct led_rgb hex_to_rgb(uint32_t hex_color) {
    return (struct led_rgb){
        .r = (hex_color >> 16) & 0xFF,
        .g = (hex_color >> 8) & 0xFF,
        .b = hex_color & 0xFF,
    };
}

static int update_pixels(const struct led_rgb *pixels) {
    return led_strip_update_rgb(led_strip, pixels, WS2812_NUM_PIXELS);
}

static int set_all_pixels(struct led_rgb color) {
    for (int i = 0; i < WS2812_NUM_PIXELS; i++) {
        work_pixels[i] = color;
    }

    return update_pixels(work_pixels);
}

static struct led_rgb lerp_rgb(struct led_rgb from, struct led_rgb to, uint16_t step,
                               uint16_t steps) {
    if (steps == 0) {
        return to;
    }

    return (struct led_rgb){
        .r = from.r + (((int)to.r - (int)from.r) * step) / steps,
        .g = from.g + (((int)to.g - (int)from.g) * step) / steps,
        .b = from.b + (((int)to.b - (int)from.b) * step) / steps,
    };
}

static uint16_t fade_steps(uint16_t fade_ms) {
    if (fade_ms == 0) {
        return 0;
    }

    return MAX(1, fade_ms / CONFIG_WS2812_WIDGET_FADE_STEP_MS);
}

static void fade_pixels_to_color(const struct led_rgb *from, struct led_rgb to, uint16_t fade_ms) {
    uint16_t steps = fade_steps(fade_ms);

    if (steps == 0) {
        set_all_pixels(to);
        return;
    }

    uint16_t delay_ms = MAX(1, fade_ms / steps);

    for (uint16_t step = 1; step <= steps; step++) {
        for (int i = 0; i < WS2812_NUM_PIXELS; i++) {
            work_pixels[i] = lerp_rgb(from[i], to, step, steps);
        }

        update_pixels(work_pixels);
        k_sleep(K_MSEC(delay_ms));
    }
}

static void fade_color_to_pixels(struct led_rgb from, const struct led_rgb *to, uint16_t fade_ms) {
    uint16_t steps = fade_steps(fade_ms);

    if (steps == 0) {
        update_pixels(to);
        return;
    }

    uint16_t delay_ms = MAX(1, fade_ms / steps);

    for (uint16_t step = 1; step <= steps; step++) {
        for (int i = 0; i < WS2812_NUM_PIXELS; i++) {
            work_pixels[i] = lerp_rgb(from, to[i], step, steps);
        }

        update_pixels(work_pixels);
        k_sleep(K_MSEC(delay_ms));
    }
}

static void fade_color_to_color(struct led_rgb from, struct led_rgb to, uint16_t fade_ms) {
    uint16_t steps = fade_steps(fade_ms);

    if (steps == 0) {
        set_all_pixels(to);
        return;
    }

    uint16_t delay_ms = MAX(1, fade_ms / steps);

    for (uint16_t step = 1; step <= steps; step++) {
        struct led_rgb color = lerp_rgb(from, to, step, steps);
        set_all_pixels(color);
        k_sleep(K_MSEC(delay_ms));
    }
}

static void fill_restore_pixels_black(void) {
    memset(restore_pixels, 0, sizeof(restore_pixels));
}

static bool suspend_underglow_and_snapshot(void) {
    fill_restore_pixels_black();

#if IS_ENABLED(CONFIG_ZMK_RGB_UNDERGLOW) &&                                                  \
    IS_ENABLED(CONFIG_WS2812_WIDGET_USE_RGB_UNDERGLOW_OVERLAY_API)
    bool was_on = false;
    size_t snapshot_count = 0;
    int rc = zmk_rgb_underglow_overlay_suspend(restore_pixels, WS2812_NUM_PIXELS,
                                               &snapshot_count, &was_on);
    if (rc == 0) {
        return was_on && snapshot_count > 0;
    }

    LOG_WRN("Failed to suspend RGB underglow overlay API: %d", rc);
#endif

    return false;
}

static void resume_underglow(void) {
#if IS_ENABLED(CONFIG_ZMK_RGB_UNDERGLOW) &&                                                  \
    IS_ENABLED(CONFIG_WS2812_WIDGET_USE_RGB_UNDERGLOW_OVERLAY_API)
    int rc = zmk_rgb_underglow_overlay_resume();
    if (rc != 0) {
        LOG_WRN("Failed to resume RGB underglow overlay API: %d", rc);
    }
#endif
}

static void execute_indicator_request(const struct indicator_request *request) {
    static const struct led_rgb black = {.r = 0, .g = 0, .b = 0};

    if (!activity_active) {
        return;
    }

    bool restore_underglow = suspend_underglow_and_snapshot();

    if (restore_underglow) {
        fade_pixels_to_color(restore_pixels, black, CONFIG_WS2812_WIDGET_PRE_FADE_MS);
    } else {
        set_all_pixels(black);
    }

    for (uint8_t i = 0; i < request->repeat_count; i++) {
        fade_color_to_color(black, request->color, request->fade_in_ms);
        k_sleep(K_MSEC(request->hold_ms));
        fade_color_to_color(request->color, black, request->fade_out_ms);

        if (request->gap_ms > 0 && i + 1 < request->repeat_count) {
            k_sleep(K_MSEC(request->gap_ms));
        }
    }

    if (restore_underglow) {
        fade_color_to_pixels(black, restore_pixels, CONFIG_WS2812_WIDGET_POST_FADE_MS);
    } else {
        set_all_pixels(black);
    }

    resume_underglow();
}

static void enqueue_indicator(struct indicator_request request) {
    if (!initialized || !activity_active) {
        return;
    }

    int rc = k_msgq_put(&indicator_msgq, &request, K_NO_WAIT);
    if (rc != 0) {
        LOG_WRN("WS2812 indicator queue full, dropping request: %d", rc);
    }
}

static void enqueue_layer_indicator(bool enabled) {
#if IS_ENABLED(CONFIG_WS2812_WIDGET_SHOW_LAYER_CHANGE)
    struct indicator_request request = {
        .color = hex_to_rgb(enabled ? CONFIG_WS2812_WIDGET_LAYER_ON_COLOR
                                    : CONFIG_WS2812_WIDGET_LAYER_OFF_COLOR),
        .fade_in_ms = CONFIG_WS2812_WIDGET_PRE_FADE_MS,
        .hold_ms = CONFIG_WS2812_WIDGET_LAYER_HOLD_MS,
        .fade_out_ms = CONFIG_WS2812_WIDGET_POST_FADE_MS,
        .gap_ms = CONFIG_WS2812_WIDGET_INTERVAL_MS,
        .repeat_count = CONFIG_WS2812_WIDGET_LAYER_REPEAT_COUNT,
    };

    enqueue_indicator(request);
#endif
}

#if IS_ENABLED(CONFIG_WS2812_WIDGET_SHOW_BATTERY)
static struct led_rgb get_battery_color(uint8_t battery_level) {
    if (battery_level == 0) {
        return hex_to_rgb(CONFIG_WS2812_WIDGET_COLOR_OFF);
    }

    if (battery_level >= CONFIG_WS2812_WIDGET_BATTERY_LEVEL_HIGH) {
        return hex_to_rgb(CONFIG_WS2812_WIDGET_BATTERY_COLOR_HIGH);
    }

    if (battery_level >= CONFIG_WS2812_WIDGET_BATTERY_LEVEL_LOW) {
        return hex_to_rgb(CONFIG_WS2812_WIDGET_BATTERY_COLOR_MEDIUM);
    }

    if (battery_level <= CONFIG_WS2812_WIDGET_BATTERY_LEVEL_CRITICAL) {
        return hex_to_rgb(CONFIG_WS2812_WIDGET_BATTERY_COLOR_CRITICAL);
    }

    return hex_to_rgb(CONFIG_WS2812_WIDGET_BATTERY_COLOR_LOW);
}
#endif

void ws2812_indicate_battery(void) {
#if IS_ENABLED(CONFIG_WS2812_WIDGET_SHOW_BATTERY)
    struct indicator_request request = {
        .color = hex_to_rgb(CONFIG_WS2812_WIDGET_BATTERY_REMINDER_COLOR),
        .fade_in_ms = CONFIG_WS2812_WIDGET_PRE_FADE_MS,
        .hold_ms = CONFIG_WS2812_WIDGET_BATTERY_HOLD_MS,
        .fade_out_ms = CONFIG_WS2812_WIDGET_POST_FADE_MS,
        .gap_ms = CONFIG_WS2812_WIDGET_INTERVAL_MS,
        .repeat_count = CONFIG_WS2812_WIDGET_BATTERY_REMINDER_REPEAT_COUNT,
    };

    enqueue_indicator(request);
#endif
}

void ws2812_indicate_connectivity(void) {
#if IS_ENABLED(CONFIG_WS2812_WIDGET_SHOW_CONNECTIVITY)
    struct indicator_request request = {
        .color = hex_to_rgb(CONFIG_WS2812_WIDGET_CONN_COLOR_CONNECTED),
        .fade_in_ms = CONFIG_WS2812_WIDGET_PRE_FADE_MS,
        .hold_ms = 500,
        .fade_out_ms = CONFIG_WS2812_WIDGET_POST_FADE_MS,
        .gap_ms = CONFIG_WS2812_WIDGET_INTERVAL_MS,
        .repeat_count = 1,
    };

    enqueue_indicator(request);
#endif
}

void ws2812_indicate_layer(void) {
#if IS_ENABLED(CONFIG_WS2812_WIDGET_SHOW_LAYER_CHANGE)
    uint8_t layer = zmk_keymap_highest_layer_active();
    if (layer == zmk_keymap_layer_default()) {
        return;
    }

    enqueue_layer_indicator(true);
#endif
}

#if IS_ENABLED(CONFIG_WS2812_WIDGET_SHOW_LAYER_CHANGE)
static bool layer_allowed(uint8_t layer) {
    if (layer == zmk_keymap_layer_default()) {
        return false;
    }

    if (layer >= 32) {
        return true;
    }

    return (CONFIG_WS2812_WIDGET_LAYER_INDICATOR_MASK & BIT(layer)) != 0;
}

static struct k_work_delayable layer_indicator_work;
static bool pending_layer_state;
static bool pending_layer_valid;

static void layer_indicator_work_cb(struct k_work *work) {
    if (!pending_layer_valid) {
        return;
    }

    bool state = pending_layer_state;
    pending_layer_valid = false;
    enqueue_layer_indicator(state);
}

static int layer_listener_cb(const zmk_event_t *eh) {
    const struct zmk_layer_state_changed *layer_ev = as_zmk_layer_state_changed(eh);

    if (!initialized || layer_ev == NULL || !layer_allowed(layer_ev->layer)) {
        return 0;
    }

    pending_layer_state = layer_ev->state;
    pending_layer_valid = true;
    k_work_reschedule(&layer_indicator_work, K_MSEC(CONFIG_WS2812_WIDGET_LAYER_DEBOUNCE_MS));

    return 0;
}

ZMK_LISTENER(ws2812_layer_listener, layer_listener_cb);
ZMK_SUBSCRIPTION(ws2812_layer_listener, zmk_layer_state_changed);
#endif

#if IS_ENABLED(CONFIG_ZMK_BATTERY_REPORTING) && IS_ENABLED(CONFIG_WS2812_WIDGET_SHOW_BATTERY)
static int battery_listener_cb(const zmk_event_t *eh) {
    const struct zmk_battery_state_changed *battery_ev = as_zmk_battery_state_changed(eh);

    if (!initialized || battery_ev == NULL) {
        return 0;
    }

    if (battery_ev->state_of_charge > 0 &&
        battery_ev->state_of_charge <= CONFIG_WS2812_WIDGET_BATTERY_LEVEL_CRITICAL) {
        struct indicator_request request = {
            .color = hex_to_rgb(CONFIG_WS2812_WIDGET_BATTERY_COLOR_CRITICAL),
            .fade_in_ms = CONFIG_WS2812_WIDGET_PRE_FADE_MS,
            .hold_ms = CONFIG_WS2812_WIDGET_BATTERY_HOLD_MS,
            .fade_out_ms = CONFIG_WS2812_WIDGET_POST_FADE_MS,
            .gap_ms = CONFIG_WS2812_WIDGET_INTERVAL_MS,
            .repeat_count = CONFIG_WS2812_WIDGET_BATTERY_CRITICAL_REPEAT_COUNT,
        };

        enqueue_indicator(request);
    }

    return 0;
}

ZMK_LISTENER(ws2812_battery_listener, battery_listener_cb);
ZMK_SUBSCRIPTION(ws2812_battery_listener, zmk_battery_state_changed);
#endif

static int activity_listener_cb(const zmk_event_t *eh) {
    const struct zmk_activity_state_changed *activity_ev = as_zmk_activity_state_changed(eh);

    if (activity_ev == NULL) {
        return 0;
    }

    activity_active = activity_ev->state == ZMK_ACTIVITY_ACTIVE;

    if (!activity_active) {
        set_all_pixels((struct led_rgb){.r = 0, .g = 0, .b = 0});
    }

    return 0;
}

ZMK_LISTENER(ws2812_activity_listener, activity_listener_cb);
ZMK_SUBSCRIPTION(ws2812_activity_listener, zmk_activity_state_changed);

#if IS_ENABLED(CONFIG_WS2812_WIDGET_BATTERY_REMINDER)
static struct k_work_delayable battery_reminder_work;

static void battery_reminder_work_cb(struct k_work *work) {
    if (initialized && activity_active) {
        ws2812_indicate_battery();
    }

    k_work_reschedule(&battery_reminder_work,
                      K_MSEC(CONFIG_WS2812_WIDGET_BATTERY_REMINDER_INTERVAL_MS));
}
#endif

extern void ws2812_indicator_process_thread(void *d0, void *d1, void *d2) {
    ARG_UNUSED(d0);
    ARG_UNUSED(d1);
    ARG_UNUSED(d2);

#if IS_ENABLED(CONFIG_WS2812_WIDGET_SHOW_LAYER_CHANGE)
    k_work_init_delayable(&layer_indicator_work, layer_indicator_work_cb);
#endif

#if IS_ENABLED(CONFIG_WS2812_WIDGET_BATTERY_REMINDER)
    k_work_init_delayable(&battery_reminder_work, battery_reminder_work_cb);
#endif

    while (true) {
        struct indicator_request request;
        k_msgq_get(&indicator_msgq, &request, K_FOREVER);
        execute_indicator_request(&request);
    }
}

K_THREAD_DEFINE(ws2812_indicator_process_tid, 1536, ws2812_indicator_process_thread, NULL, NULL,
                NULL, K_LOWEST_APPLICATION_THREAD_PRIO, 0, 100);

extern void ws2812_indicator_init_thread(void *d0, void *d1, void *d2) {
    ARG_UNUSED(d0);
    ARG_UNUSED(d1);
    ARG_UNUSED(d2);

    if (!device_is_ready(led_strip)) {
        LOG_ERR("WS2812 LED strip device not ready");
        return;
    }

    initialized = true;
    LOG_INF("WS2812 overlay indicator initialized with %d pixels", WS2812_NUM_PIXELS);

#if IS_ENABLED(CONFIG_ZMK_BATTERY_REPORTING) && IS_ENABLED(CONFIG_WS2812_WIDGET_SHOW_BATTERY) &&   \
    IS_ENABLED(CONFIG_WS2812_WIDGET_SHOW_BATTERY_ON_START)
    uint8_t battery_level = zmk_battery_state_of_charge();
    struct indicator_request request = {
        .color = get_battery_color(battery_level),
        .fade_in_ms = CONFIG_WS2812_WIDGET_PRE_FADE_MS,
        .hold_ms = CONFIG_WS2812_WIDGET_BATTERY_HOLD_MS,
        .fade_out_ms = CONFIG_WS2812_WIDGET_POST_FADE_MS,
        .gap_ms = CONFIG_WS2812_WIDGET_INTERVAL_MS,
        .repeat_count = CONFIG_WS2812_WIDGET_BATTERY_REMINDER_REPEAT_COUNT,
    };
    enqueue_indicator(request);
#endif

#if IS_ENABLED(CONFIG_WS2812_WIDGET_BATTERY_REMINDER)
    k_work_reschedule(&battery_reminder_work,
                      K_MSEC(CONFIG_WS2812_WIDGET_BATTERY_REMINDER_INTERVAL_MS));
#endif
}

K_THREAD_DEFINE(ws2812_indicator_init_tid, 1024, ws2812_indicator_init_thread, NULL, NULL, NULL,
                K_LOWEST_APPLICATION_THREAD_PRIO, 0, 200);
