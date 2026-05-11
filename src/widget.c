/*
 * WS2812 temporary indicator widget for ZMK.
 *
 * This version intentionally does NOT implement persistent layer colors.
 * Layer changes are temporary overlay indications:
 *   - configured permanent layer ON  -> one smooth white blink
 *   - configured permanent layer OFF -> one smooth red blink
 *
 * Battery reminder:
 *   - if battery level is critical or lower
 *   - every CONFIG_WS2812_WIDGET_BATTERY_PERIODIC_MS
 *   - CONFIG_WS2812_WIDGET_BATTERY_BLINK_REPEAT red smooth blinks
 *
 * It is driver-only. It does not patch ZMK core and it does not touch the keymap.
 */

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/led_strip.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

#include <zmk/activity.h>
#include <zmk/battery.h>
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

enum ws2812_request_kind {
    WS2812_REQ_BATTERY_MANUAL,
    WS2812_REQ_BATTERY_CRITICAL,
    WS2812_REQ_CONNECTIVITY,
    WS2812_REQ_LAYER_MANUAL,
    WS2812_REQ_LAYER_ON,
    WS2812_REQ_LAYER_OFF,
};

struct ws2812_request {
    enum ws2812_request_kind kind;
    struct led_rgb color;
    uint8_t repeat_count;
    uint16_t fade_in_ms;
    uint16_t hold_ms;
    uint16_t fade_out_ms;
    uint16_t pause_ms;
};

static const struct device *const led_strip = DEVICE_DT_GET(WS2812_STRIP_NODE);
static struct led_rgb pixels[WS2812_NUM_PIXELS];

static bool initialized;
static bool widget_enabled = IS_ENABLED(CONFIG_WS2812_WIDGET_ENABLED_ON_START);
static int64_t last_activity_ms;
static int64_t last_layer_indication_ms;

K_MSGQ_DEFINE(ws2812_msgq, sizeof(struct ws2812_request), 12, 4);

static struct led_rgb hex_to_rgb(uint32_t hex_color) {
    return (struct led_rgb){
        .r = (hex_color >> 16) & 0xFF,
        .g = (hex_color >> 8) & 0xFF,
        .b = hex_color & 0xFF,
    };
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

static int set_leds_color(struct led_rgb color) {
    for (int i = 0; i < WS2812_NUM_PIXELS; i++) {
        pixels[i] = color;
    }

    return led_strip_update_rgb(led_strip, pixels, WS2812_NUM_PIXELS);
}

static bool widget_allowed_now(bool periodic) {
    if (!initialized || !widget_enabled) {
        return false;
    }

#if IS_ENABLED(CONFIG_WS2812_WIDGET_AUTO_DISABLE_AFTER_INACTIVITY)
    if (periodic) {
        int64_t now = k_uptime_get();
        if (last_activity_ms > 0 &&
            now - last_activity_ms > CONFIG_WS2812_WIDGET_INACTIVITY_DISABLE_MS) {
            return false;
        }
    }
#else
    ARG_UNUSED(periodic);
#endif

    return true;
}

static uint16_t step_delay_ms(uint16_t total_ms, uint16_t steps) {
    if (steps == 0) {
        return total_ms;
    }

    uint16_t delay = total_ms / steps;
    return MAX(delay, 1);
}

static void fade_to_color(struct led_rgb color, bool fade_in, uint16_t duration_ms) {
#if IS_ENABLED(CONFIG_WS2812_WIDGET_LAYER_FADE_ENABLE)
    const uint16_t steps = MAX(CONFIG_WS2812_WIDGET_LAYER_FADE_STEPS, 1);
#else
    const uint16_t steps = 1;
#endif

    if (duration_ms == 0) {
        set_leds_color(fade_in ? color : (struct led_rgb){0, 0, 0});
        return;
    }

    uint16_t delay = step_delay_ms(duration_ms, steps);

    for (uint16_t step = 0; step <= steps; step++) {
        uint16_t level = fade_in ? step : (steps - step);
        set_leds_color(scale_rgb(color, level, steps));
        k_sleep(K_MSEC(delay));
    }
}

static void execute_request_pixels(const struct ws2812_request *request) {
    for (uint8_t i = 0; i < request->repeat_count; i++) {
        fade_to_color(request->color, true, request->fade_in_ms);

        if (request->hold_ms > 0) {
            set_leds_color(request->color);
            k_sleep(K_MSEC(request->hold_ms));
        }

        fade_to_color(request->color, false, request->fade_out_ms);
        set_leds_color((struct led_rgb){0, 0, 0});

        if (i + 1 < request->repeat_count && request->pause_ms > 0) {
            k_sleep(K_MSEC(request->pause_ms));
        }
    }
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

    set_leds_color((struct led_rgb){0, 0, 0});
}

static void enqueue_request(struct ws2812_request request, bool periodic) {
    if (!widget_allowed_now(periodic)) {
        return;
    }

    int rc = k_msgq_put(&ws2812_msgq, &request, K_NO_WAIT);
    if (rc != 0) {
        LOG_WRN("WS2812 request queue full, dropping request kind %d", request.kind);
    }
}

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

static struct ws2812_request make_battery_request(struct led_rgb color,
                                                  enum ws2812_request_kind kind,
                                                  uint8_t repeat_count) {
    return (struct ws2812_request){
        .kind = kind,
        .color = color,
        .repeat_count = repeat_count,
        .fade_in_ms = CONFIG_WS2812_WIDGET_BATTERY_FADE_IN_MS,
        .hold_ms = CONFIG_WS2812_WIDGET_BATTERY_HOLD_MS,
        .fade_out_ms = CONFIG_WS2812_WIDGET_BATTERY_FADE_OUT_MS,
        .pause_ms = CONFIG_WS2812_WIDGET_BATTERY_BLINK_PAUSE_MS,
    };
}

void ws2812_indicate_battery(void) {
    uint8_t battery_level = zmk_battery_state_of_charge();
    int retry = 0;

    while (battery_level == 0 && retry++ < 10) {
        k_sleep(K_MSEC(100));
        battery_level = zmk_battery_state_of_charge();
    }

    struct led_rgb color = get_battery_status_color(battery_level);
    enqueue_request(make_battery_request(color, WS2812_REQ_BATTERY_MANUAL,
                                         CONFIG_WS2812_WIDGET_BATTERY_BLINK_REPEAT),
                    false);
}

static void battery_periodic_work_handler(struct k_work *work);
K_WORK_DELAYABLE_DEFINE(battery_periodic_work, battery_periodic_work_handler);

static void schedule_next_battery_period(void) {
    k_work_reschedule(&battery_periodic_work,
                      K_MSEC(CONFIG_WS2812_WIDGET_BATTERY_PERIODIC_MS));
}

static void battery_periodic_work_handler(struct k_work *work) {
    ARG_UNUSED(work);

    if (!widget_allowed_now(true)) {
        schedule_next_battery_period();
        return;
    }

    int64_t now = k_uptime_get();
    if (last_layer_indication_ms > 0 &&
        now - last_layer_indication_ms < CONFIG_WS2812_WIDGET_BATTERY_COOLDOWN_AFTER_LAYER_MS) {
        schedule_next_battery_period();
        return;
    }

    uint8_t battery_level = zmk_battery_state_of_charge();

    if (battery_level > 0 && battery_level <= CONFIG_WS2812_WIDGET_BATTERY_LEVEL_CRITICAL) {
        struct ws2812_request request =
            make_battery_request(hex_to_rgb(CONFIG_WS2812_WIDGET_BATTERY_COLOR_CRITICAL),
                                 WS2812_REQ_BATTERY_CRITICAL,
                                 CONFIG_WS2812_WIDGET_BATTERY_BLINK_REPEAT);
        enqueue_request(request, true);
    }

    schedule_next_battery_period();
}

static int battery_listener_cb(const zmk_event_t *eh) {
    if (!initialized) {
        return 0;
    }

    const struct zmk_battery_state_changed *ev = as_zmk_battery_state_changed(eh);
    if (ev == NULL) {
        return 0;
    }

    if (ev->state_of_charge > 0 &&
        ev->state_of_charge <= CONFIG_WS2812_WIDGET_BATTERY_LEVEL_CRITICAL) {
        struct ws2812_request request =
            make_battery_request(hex_to_rgb(CONFIG_WS2812_WIDGET_BATTERY_COLOR_CRITICAL),
                                 WS2812_REQ_BATTERY_CRITICAL,
                                 CONFIG_WS2812_WIDGET_BATTERY_BLINK_REPEAT);
        enqueue_request(request, false);
    }

    return 0;
}

ZMK_LISTENER(ws2812_battery_listener, battery_listener_cb);
ZMK_SUBSCRIPTION(ws2812_battery_listener, zmk_battery_state_changed);
#else
void ws2812_indicate_battery(void) {}
#endif

void ws2812_indicate_connectivity(void) {
#if IS_ENABLED(CONFIG_WS2812_WIDGET_SHOW_CONNECTIVITY)
    struct ws2812_request request = {
        .kind = WS2812_REQ_CONNECTIVITY,
        .color = hex_to_rgb(CONFIG_WS2812_WIDGET_COLOR_CYAN),
        .repeat_count = 1,
        .fade_in_ms = 200,
        .hold_ms = CONFIG_WS2812_WIDGET_CONN_BLINK_MS,
        .fade_out_ms = 200,
        .pause_ms = CONFIG_WS2812_WIDGET_INTERVAL_MS,
    };

    enqueue_request(request, false);
#endif
}

#if IS_ENABLED(CONFIG_WS2812_WIDGET_SHOW_LAYER_CHANGE)
static uint8_t pending_layer;
static bool pending_layer_state;
static bool pending_layer_valid;

static bool layer_is_triggered(uint8_t layer) {
    return (CONFIG_WS2812_WIDGET_LAYER_TRIGGER_0 >= 0 &&
            layer == CONFIG_WS2812_WIDGET_LAYER_TRIGGER_0) ||
           (CONFIG_WS2812_WIDGET_LAYER_TRIGGER_1 >= 0 &&
            layer == CONFIG_WS2812_WIDGET_LAYER_TRIGGER_1) ||
           (CONFIG_WS2812_WIDGET_LAYER_TRIGGER_2 >= 0 &&
            layer == CONFIG_WS2812_WIDGET_LAYER_TRIGGER_2) ||
           (CONFIG_WS2812_WIDGET_LAYER_TRIGGER_3 >= 0 &&
            layer == CONFIG_WS2812_WIDGET_LAYER_TRIGGER_3);
}

static struct ws2812_request make_layer_request(struct led_rgb color,
                                                enum ws2812_request_kind kind) {
    return (struct ws2812_request){
        .kind = kind,
        .color = color,
        .repeat_count = CONFIG_WS2812_WIDGET_LAYER_BLINK_REPEAT,
        .fade_in_ms = CONFIG_WS2812_WIDGET_LAYER_FADE_IN_MS,
        .hold_ms = CONFIG_WS2812_WIDGET_LAYER_HOLD_MS,
        .fade_out_ms = CONFIG_WS2812_WIDGET_LAYER_FADE_OUT_MS,
        .pause_ms = CONFIG_WS2812_WIDGET_LAYER_BLINK_PAUSE_MS,
    };
}

static void layer_work_handler(struct k_work *work);
K_WORK_DELAYABLE_DEFINE(layer_work, layer_work_handler);

static void layer_work_handler(struct k_work *work) {
    ARG_UNUSED(work);

    if (!pending_layer_valid) {
        return;
    }

    uint8_t layer = pending_layer;
    bool state = pending_layer_state;
    pending_layer_valid = false;

    if (!layer_is_triggered(layer)) {
        return;
    }

    struct led_rgb color =
        state ? hex_to_rgb(CONFIG_WS2812_WIDGET_LAYER_COLOR_ON)
              : hex_to_rgb(CONFIG_WS2812_WIDGET_LAYER_COLOR_OFF);

    struct ws2812_request request =
        make_layer_request(color, state ? WS2812_REQ_LAYER_ON : WS2812_REQ_LAYER_OFF);

    last_layer_indication_ms = k_uptime_get();
    enqueue_request(request, false);
}

void ws2812_indicate_layer(void) {
    struct ws2812_request request =
        make_layer_request(hex_to_rgb(CONFIG_WS2812_WIDGET_LAYER_COLOR_MANUAL),
                           WS2812_REQ_LAYER_MANUAL);
    last_layer_indication_ms = k_uptime_get();
    enqueue_request(request, false);
}

static int layer_listener_cb(const zmk_event_t *eh) {
    if (!initialized) {
        return 0;
    }

    const struct zmk_layer_state_changed *ev = as_zmk_layer_state_changed(eh);
    if (ev == NULL) {
        return 0;
    }

    if (!layer_is_triggered(ev->layer)) {
        return 0;
    }

    pending_layer = ev->layer;
    pending_layer_state = ev->state;
    pending_layer_valid = true;

    k_work_reschedule(&layer_work, K_MSEC(CONFIG_WS2812_WIDGET_LAYER_DEBOUNCE_MS));

    return 0;
}

ZMK_LISTENER(ws2812_layer_listener, layer_listener_cb);
ZMK_SUBSCRIPTION(ws2812_layer_listener, zmk_layer_state_changed);
#else
void ws2812_indicate_layer(void) {}
#endif

static int activity_listener_cb(const zmk_event_t *eh) {
    const struct zmk_activity_state_changed *ev = as_zmk_activity_state_changed(eh);
    if (ev == NULL) {
        return 0;
    }

    if (ev->state == ZMK_ACTIVITY_ACTIVE) {
        last_activity_ms = k_uptime_get();
    }

    if (ev->state == ZMK_ACTIVITY_SLEEP) {
        set_leds_color((struct led_rgb){0, 0, 0});
    }

    return 0;
}

ZMK_LISTENER(ws2812_activity_listener, activity_listener_cb);
ZMK_SUBSCRIPTION(ws2812_activity_listener, zmk_activity_state_changed);

static void led_process_thread(void *d0, void *d1, void *d2) {
    ARG_UNUSED(d0);
    ARG_UNUSED(d1);
    ARG_UNUSED(d2);

    while (true) {
        struct ws2812_request request;

        k_msgq_get(&ws2812_msgq, &request, K_FOREVER);

        LOG_DBG("WS2812 indication request kind %d", request.kind);

        bool underglow_was_on = pause_underglow_if_needed();
        execute_request_pixels(&request);
        restore_underglow_if_needed(underglow_was_on);

        k_sleep(K_MSEC(CONFIG_WS2812_WIDGET_INTERVAL_MS));
    }
}

K_THREAD_DEFINE(ws2812_led_process_tid, 1024, led_process_thread, NULL, NULL, NULL,
                K_LOWEST_APPLICATION_THREAD_PRIO, 0, 100);

static void led_init_thread(void *d0, void *d1, void *d2) {
    ARG_UNUSED(d0);
    ARG_UNUSED(d1);
    ARG_UNUSED(d2);

    if (!device_is_ready(led_strip)) {
        LOG_ERR("WS2812 LED strip device is not ready");
        return;
    }

    last_activity_ms = k_uptime_get();
    initialized = true;

    set_leds_color((struct led_rgb){0, 0, 0});

    LOG_INF("WS2812 indicator initialized with %d pixels", WS2812_NUM_PIXELS);

#if IS_ENABLED(CONFIG_WS2812_WIDGET_SHOW_BATTERY)
    schedule_next_battery_period();

#if IS_ENABLED(CONFIG_WS2812_WIDGET_SHOW_BATTERY_ON_START)
    ws2812_indicate_battery();
#endif
#endif
}

K_THREAD_DEFINE(ws2812_led_init_tid, 1024, led_init_thread, NULL, NULL, NULL,
                K_LOWEST_APPLICATION_THREAD_PRIO, 0, 200);
