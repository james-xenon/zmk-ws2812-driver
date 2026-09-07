/*
 * WS2812 indicator widget for ZMK v0.3.x.
 *
 * Goals of this revision:
 *   - real host Caps Lock state via CONFIG_ZMK_HID_INDICATORS;
 *   - Caps Lock lights only R/T LEDs on the left half;
 *   - persistent layer colors and Caps can coexist;
 *   - temporary layer/battery/manual flashes temporarily override static state
 *     and then restore it deterministically;
 *   - reset_l0_green_flash (&to 0, then &ws2812_wdg 0) does not destroy Caps,
 *     persistent colors, underglow state, or ext-power state;
 *   - the green reset flash suppresses the automatic red layer-OFF flash.
 */

#include <string.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/led_strip.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

#include <zmk/activity.h>
#include <zmk/behavior.h>
#include <zmk/behavior_queue.h>
#include <zmk/events/activity_state_changed.h>

#if IS_ENABLED(CONFIG_ZMK_BATTERY_REPORTING)
#include <zmk/battery.h>
#include <zmk/events/battery_state_changed.h>
#endif

#if IS_ENABLED(CONFIG_ZMK_RGB_UNDERGLOW)
#include <zmk/rgb_underglow.h>
#endif

#if IS_ENABLED(CONFIG_ZMK_EXT_POWER)
#include <drivers/ext_power.h>
#endif

/* -------------------------------------------------------------------------
 * Half detection
 * ------------------------------------------------------------------------- */
#if !IS_ENABLED(CONFIG_ZMK_SPLIT)
#define WS2812_HALF_IS_LEFT 1
#define WS2812_HALF_IS_RIGHT 1
#elif IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
#define WS2812_HALF_IS_LEFT 1
#define WS2812_HALF_IS_RIGHT 0
#else
#define WS2812_HALF_IS_LEFT 0
#define WS2812_HALF_IS_RIGHT 1
#endif

/* Layer events exist on central only. Peripheral receives persistent state via
 * ws2812_lsync (GLOBAL locality). */
#if !IS_ENABLED(CONFIG_ZMK_SPLIT) || IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
#define WS2812_HAS_LAYER_EVENTS 1
#include <zmk/events/layer_state_changed.h>
#include <zmk/keymap.h>
#else
#define WS2812_HAS_LAYER_EVENTS 0
#endif

/* Real host HID indicators are needed only where the Caps LEDs physically are. */
#if WS2812_HALF_IS_LEFT && IS_ENABLED(CONFIG_ZMK_HID_INDICATORS)
#include <zmk/events/hid_indicators_changed.h>
#include <zmk/hid_indicators.h>
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
BUILD_ASSERT(WS2812_NUM_PIXELS > 0 && WS2812_NUM_PIXELS <= 255,
             "chain-length must be the number of LEDs on THIS half (1..255)");

/* -------------------------------------------------------------------------
 * Persistent layer configuration
 * ------------------------------------------------------------------------- */
#define PERSISTENT_LAYER_LEFT 3
#define PERSISTENT_LAYER_RIGHT 2
#define PERSISTENT_LAYER_COLOR 0x00FFFF

static const uint8_t __maybe_unused persistent_sync_layers[] = {
    PERSISTENT_LAYER_LEFT,
    PERSISTENT_LAYER_RIGHT,
};

/* -------------------------------------------------------------------------
 * Caps Lock indicator
 *
 * On this PCB each half has its own 0..20 LED index space.
 * Left-half LEDs 5 and 6 correspond to R and T in the current hardware map.
 * ------------------------------------------------------------------------- */
#if WS2812_HALF_IS_LEFT
#define CAPS_INDICATOR_START 5
#define CAPS_INDICATOR_COUNT 2
#define CAPS_INDICATOR_COLOR 0xFFFFFF
#define HID_LED_CAPS_LOCK_BIT BIT(1)
static bool caps_lock_active;
#endif

/* Automatic layer-OFF indication is deliberately delayed slightly longer than
 * the keymap macro's 20 ms step. This gives reset_l0_green_flash time to call
 * &ws2812_wdg 0 and cancel/suppress the red OFF flash deterministically. */
#define RESET_MANUAL_SUPPRESS_MS 250U
#define AUTO_LAYER_OFF_MIN_DELAY_MS 80U

/* -------------------------------------------------------------------------
 * Types/state
 * ------------------------------------------------------------------------- */
enum indicator_kind {
    INDICATOR_KIND_MANUAL_LAYER,
    INDICATOR_KIND_LAYER_ON,
    INDICATOR_KIND_LAYER_OFF,
    INDICATOR_KIND_BATTERY_MANUAL,
    INDICATOR_KIND_BATTERY_CRITICAL,
    INDICATOR_KIND_CONNECTIVITY,
    INDICATOR_KIND_SEPARATOR,
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

#define MAX_PERSISTENT_LAYERS 6
struct persistent_layer_config {
    uint8_t layer;
    struct led_rgb color;
    uint8_t start_pixel;
    uint8_t num_pixels;
    bool configured;
    bool active;
};

static const struct device *const led_strip = DEVICE_DT_GET(WS2812_STRIP_NODE);
static struct led_rgb pixels[WS2812_NUM_PIXELS];
static struct persistent_layer_config persistent_layers[MAX_PERSISTENT_LAYERS];

static bool initialized;
static bool widget_enabled = IS_ENABLED(CONFIG_WS2812_WIDGET_ENABLED_ON_START);
static bool activity_active = true;
static int64_t last_activity_ms;
static int64_t last_layer_indication_ms;

/* One owner for restoration of normal RGB/ext-power state.
 * It is captured exactly once when the first static owner appears and restored
 * exactly once when the last static owner disappears. */
static bool normal_state_saved;
static bool normal_underglow_was_on;
static bool normal_ext_power_was_on = true;

/* Serializes all direct writes to the LED strip and all state transitions that
 * save/restore normal underglow. */
K_MUTEX_DEFINE(ws2812_lighting_mutex);

K_MSGQ_DEFINE(indicator_msgq, sizeof(struct indicator_request), 12, 4);

#if IS_ENABLED(CONFIG_ZMK_BATTERY_REPORTING) && IS_ENABLED(CONFIG_WS2812_WIDGET_SHOW_BATTERY) && \
    IS_ENABLED(CONFIG_ZMK_SPLIT) && IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL) &&              \
    IS_ENABLED(CONFIG_ZMK_SPLIT_BLE_CENTRAL_BATTERY_LEVEL_FETCHING)
static uint8_t peripheral_battery_level;
#endif

/* -------------------------------------------------------------------------
 * Pixel helpers
 * ------------------------------------------------------------------------- */
static struct led_rgb hex_to_rgb(uint32_t hex_color) {
    return (struct led_rgb){
        .r = (hex_color >> 16) & 0xFF,
        .g = (hex_color >> 8) & 0xFF,
        .b = hex_color & 0xFF,
    };
}

static void buffer_fill(struct led_rgb color) {
    for (int i = 0; i < WS2812_NUM_PIXELS; i++) {
        pixels[i] = color;
    }
}

static void buffer_range(struct led_rgb color, uint8_t start, uint8_t count) {
    uint16_t end = MIN((uint16_t)start + count, WS2812_NUM_PIXELS);
    if (start >= WS2812_NUM_PIXELS) {
        return;
    }
    for (uint16_t i = start; i < end; i++) {
        pixels[i] = color;
    }
}

static int flush_pixels(void) {
    return led_strip_update_rgb(led_strip, pixels, WS2812_NUM_PIXELS);
}

static int set_all_pixels(struct led_rgb color) {
    buffer_fill(color);
    return flush_pixels();
}

/* -------------------------------------------------------------------------
 * Normal underglow/ext-power helpers
 * ------------------------------------------------------------------------- */
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

#if IS_ENABLED(CONFIG_ZMK_EXT_POWER)
static const struct device *get_ext_power_device(void) {
    return device_get_binding("EXT_POWER");
}
#endif

static bool enable_ext_power_if_needed(void) {
    bool ext_power_was_on = true;
#if IS_ENABLED(CONFIG_WS2812_WIDGET_USE_EXT_POWER) && IS_ENABLED(CONFIG_ZMK_EXT_POWER)
    const struct device *ext_power = get_ext_power_device();
    if (ext_power == NULL) {
        LOG_WRN("EXT_POWER device not found");
        return true;
    }

    ext_power_was_on = ext_power_get(ext_power) > 0;
    if (!ext_power_was_on) {
        ext_power_enable(ext_power);
        k_sleep(K_MSEC(CONFIG_WS2812_WIDGET_EXT_POWER_STARTUP_DELAY_MS));
    }
#endif
    return ext_power_was_on;
}

static void restore_ext_power_if_needed(bool ext_power_was_on, bool underglow_was_on) {
#if IS_ENABLED(CONFIG_WS2812_WIDGET_USE_EXT_POWER) && IS_ENABLED(CONFIG_ZMK_EXT_POWER)
    const struct device *ext_power = get_ext_power_device();
    if (ext_power == NULL) {
        return;
    }

    if (!ext_power_was_on && !underglow_was_on &&
        IS_ENABLED(CONFIG_WS2812_WIDGET_RESTORE_EXT_POWER_OFF)) {
        ext_power_disable(ext_power);
    }
#else
    ARG_UNUSED(ext_power_was_on);
    ARG_UNUSED(underglow_was_on);
#endif
}

/* -------------------------------------------------------------------------
 * Persistent/static state
 * ------------------------------------------------------------------------- */
static bool any_persistent_layer_active(void) {
    for (int i = 0; i < MAX_PERSISTENT_LAYERS; i++) {
        if (persistent_layers[i].configured && persistent_layers[i].active) {
            return true;
        }
    }
    return false;
}

static bool static_lighting_needed(void) {
    if (any_persistent_layer_active()) {
        return true;
    }
#if WS2812_HALF_IS_LEFT
    if (caps_lock_active) {
        return true;
    }
#endif
    return false;
}

static void redraw_static_lighting_locked(void) {
    buffer_fill((struct led_rgb){0, 0, 0});

    /* Base static layer color first. */
    for (int i = 0; i < MAX_PERSISTENT_LAYERS; i++) {
        if (persistent_layers[i].configured && persistent_layers[i].active) {
            buffer_range(persistent_layers[i].color,
                         persistent_layers[i].start_pixel,
                         persistent_layers[i].num_pixels);
        }
    }

    /* Caps has higher static priority than persistent color on R/T. */
#if WS2812_HALF_IS_LEFT
    if (caps_lock_active) {
        buffer_range(hex_to_rgb(CAPS_INDICATOR_COLOR),
                     CAPS_INDICATOR_START, CAPS_INDICATOR_COUNT);
    }
#endif

    flush_pixels();
}

static void capture_normal_state_locked(void) {
    if (normal_state_saved) {
        return;
    }

    normal_underglow_was_on = pause_underglow_if_needed();
    normal_ext_power_was_on = enable_ext_power_if_needed();
    normal_state_saved = true;
}

static void restore_normal_state_locked(void) {
    if (!normal_state_saved) {
        return;
    }

    set_all_pixels((struct led_rgb){0, 0, 0});
    restore_underglow_if_needed(normal_underglow_was_on);
    restore_ext_power_if_needed(normal_ext_power_was_on, normal_underglow_was_on);

    normal_state_saved = false;
    normal_underglow_was_on = false;
    normal_ext_power_was_on = true;
}

static void sync_static_lighting_locked(void) {
    if (!initialized || !device_is_ready(led_strip)) {
        return;
    }

    if (static_lighting_needed()) {
        capture_normal_state_locked();
        redraw_static_lighting_locked();
    } else {
        restore_normal_state_locked();
    }
}

void ws2812_set_persistent_layer_color(uint8_t layer, uint32_t color_hex,
                                       uint8_t start_pixel, uint8_t num_pixels) {
    int slot = -1;

    for (int i = 0; i < MAX_PERSISTENT_LAYERS; i++) {
        if (persistent_layers[i].configured && persistent_layers[i].layer == layer) {
            slot = i;
            break;
        }
        if (!persistent_layers[i].configured && slot < 0) {
            slot = i;
        }
    }

    if (slot < 0) {
        LOG_WRN("No free persistent layer slots");
        return;
    }

    if (start_pixel >= WS2812_NUM_PIXELS) {
        LOG_ERR("Persistent layer %d: start_pixel %d is outside this half (0..%d)",
                layer, start_pixel, WS2812_NUM_PIXELS - 1);
        return;
    }

    persistent_layers[slot].layer = layer;
    persistent_layers[slot].color = hex_to_rgb(color_hex);
    persistent_layers[slot].start_pixel = start_pixel;
    persistent_layers[slot].num_pixels = num_pixels;
    persistent_layers[slot].configured = true;
    persistent_layers[slot].active = false;

    LOG_INF("Persistent layer %d configured: color=0x%06X pixels=%d-%d",
            layer, color_hex, start_pixel,
            MIN((uint16_t)start_pixel + num_pixels, WS2812_NUM_PIXELS) - 1);
}

void ws2812_set_persistent_layer_active(uint8_t layer, bool active) {
    int slot = -1;

    for (int i = 0; i < MAX_PERSISTENT_LAYERS; i++) {
        if (persistent_layers[i].configured && persistent_layers[i].layer == layer) {
            slot = i;
            break;
        }
    }

    if (slot < 0) {
        return;
    }

    /* An early GLOBAL lsync may arrive before init completes. Remember it so
     * init can render the correct static state later. */
    if (!initialized || !device_is_ready(led_strip)) {
        persistent_layers[slot].active = active;
        return;
    }

    /* Change the owner state while holding the same mutex as animations.
     * Otherwise an activation during a transient flash could capture the
     * temporarily-paused underglow as the user's original state. */
    k_mutex_lock(&ws2812_lighting_mutex, K_FOREVER);
    if (persistent_layers[slot].active != active) {
        persistent_layers[slot].active = active;
        sync_static_lighting_locked();
    }
    k_mutex_unlock(&ws2812_lighting_mutex);
}

/* -------------------------------------------------------------------------
 * Caps Lock: REAL host state
 * ------------------------------------------------------------------------- */
#if WS2812_HALF_IS_LEFT && IS_ENABLED(CONFIG_ZMK_HID_INDICATORS)
static int hid_caps_listener_cb(const zmk_event_t *eh) {
    const struct zmk_hid_indicators_changed *ev = as_zmk_hid_indicators_changed(eh);
    if (ev == NULL) {
        return 0;
    }

    bool new_caps_state = (ev->indicators & HID_LED_CAPS_LOCK_BIT) != 0;

    if (!initialized || !device_is_ready(led_strip)) {
        caps_lock_active = new_caps_state;
        return 0;
    }

    /* Update the owner state under the animation mutex. If a host report
     * arrives during a blink, the blink restores the true pre-blink lighting
     * first; only then does Caps take ownership and snapshot that state. */
    k_mutex_lock(&ws2812_lighting_mutex, K_FOREVER);
    if (new_caps_state != caps_lock_active) {
        caps_lock_active = new_caps_state;
        sync_static_lighting_locked();
    }
    k_mutex_unlock(&ws2812_lighting_mutex);

    LOG_DBG("Host Caps Lock state: %s", new_caps_state ? "ON" : "OFF");
    return 0;
}

ZMK_LISTENER(ws2812_hid_caps_listener, hid_caps_listener_cb);
ZMK_SUBSCRIPTION(ws2812_hid_caps_listener, zmk_hid_indicators_changed);
#endif

/* -------------------------------------------------------------------------
 * Fade/transient helpers
 * ------------------------------------------------------------------------- */
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

    if (initialized && device_is_ready(led_strip)) {
        k_mutex_lock(&ws2812_lighting_mutex, K_FOREVER);
        if (static_lighting_needed()) {
            /* Caps/persistent status remains authoritative even if temporary
             * widget flashes are disabled. */
            sync_static_lighting_locked();
        }
        /* With no static owner, toggling the widget must not write black over
         * normal ZMK underglow. It only enables/disables temporary indicators. */
        k_mutex_unlock(&ws2812_lighting_mutex);
    }

    LOG_INF("WS2812 temporary indications %s", enabled ? "enabled" : "disabled");
}

void ws2812_toggle_indication_enabled(void) {
    ws2812_set_indication_enabled(!widget_enabled);
}

static void execute_indicator_request(const struct indicator_request *request) {
    if (request->kind == INDICATOR_KIND_SEPARATOR) {
        k_sleep(K_MSEC(request->hold_ms));
        return;
    }

    k_mutex_lock(&ws2812_lighting_mutex, K_FOREVER);

    bool had_static_owner = static_lighting_needed();
    bool transient_underglow_was_on = false;
    bool transient_ext_power_was_on = true;

    if (had_static_owner) {
        /* Static state already owns the strip. Make sure its normal-state
         * snapshot exists, then temporarily paint over it. */
        capture_normal_state_locked();
    } else {
        transient_underglow_was_on = pause_underglow_if_needed();
        transient_ext_power_was_on = enable_ext_power_if_needed();
    }

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

    if (static_lighting_needed()) {
        /* Critical rule: every transient flash returns to persistent+Caps,
         * not blindly to plain underglow. */
        capture_normal_state_locked();
        redraw_static_lighting_locked();
    } else if (had_static_owner) {
        /* This branch is mostly defensive: state listeners are serialized by
         * the same mutex, so static state normally cannot change mid-animation. */
        restore_normal_state_locked();
    } else {
        restore_underglow_if_needed(transient_underglow_was_on);
        restore_ext_power_if_needed(transient_ext_power_was_on,
                                    transient_underglow_was_on);
    }

    k_mutex_unlock(&ws2812_lighting_mutex);
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

/* -------------------------------------------------------------------------
 * Layer/manual indications
 * ------------------------------------------------------------------------- */
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

#if IS_ENABLED(CONFIG_WS2812_WIDGET_SHOW_LAYER_CHANGE) && WS2812_HAS_LAYER_EVENTS
static struct k_work_delayable layer_indicator_work;
static bool pending_layer_state;
static bool pending_layer_valid;
static uint32_t manual_layer_suppress_until_ms;
#endif

void ws2812_indicate_layer(void) {
#if IS_ENABLED(CONFIG_WS2812_WIDGET_SHOW_LAYER_CHANGE)
    last_layer_indication_ms = k_uptime_get();

#if WS2812_HAS_LAYER_EVENTS
    /* This is the manual green confirmation used by reset_l0_green_flash.
     * Cancel a pending automatic layer OFF indication and suppress any near-
     * simultaneous delayed one. */
    manual_layer_suppress_until_ms = k_uptime_get_32() + RESET_MANUAL_SUPPRESS_MS;
    pending_layer_valid = false;
    k_work_cancel_delayable(&layer_indicator_work);
#endif

    enqueue_indicator(make_manual_layer_request(), false);
#endif
}

void ws2812_apply_layer_sync(bool enabled) {
#if IS_ENABLED(CONFIG_WS2812_WIDGET_SHOW_LAYER_CHANGE)
    last_layer_indication_ms = k_uptime_get();
    enqueue_indicator(make_layer_request(enabled), false);
#else
    ARG_UNUSED(enabled);
#endif
}

#if IS_ENABLED(CONFIG_WS2812_WIDGET_SHOW_LAYER_CHANGE) && WS2812_HAS_LAYER_EVENTS
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
    return CONFIG_WS2812_WIDGET_LAYER_TRIGGER_0 >= 0 ||
           CONFIG_WS2812_WIDGET_LAYER_TRIGGER_1 >= 0 ||
           CONFIG_WS2812_WIDGET_LAYER_TRIGGER_2 >= 0 ||
           CONFIG_WS2812_WIDGET_LAYER_TRIGGER_3 >= 0;
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

static bool suppress_window_active(void) {
    uint32_t now = k_uptime_get_32();
    return (int32_t)(manual_layer_suppress_until_ms - now) > 0;
}

static void layer_indicator_work_cb(struct k_work *work) {
    ARG_UNUSED(work);

    if (!pending_layer_valid) {
        return;
    }

    bool state = pending_layer_state;
    pending_layer_valid = false;

    /* Manual green reset confirmation wins over the automatic red OFF flash. */
    if (!state && suppress_window_active()) {
        return;
    }

    struct zmk_behavior_binding binding = {
        .behavior_dev = DEVICE_DT_NAME(DT_NODELABEL(ws2812_lsync)),
        .param1 = state ? 1 : 0,
        .param2 = 0,
    };
    struct zmk_behavior_binding_event event = {
        .position = 0,
        .timestamp = k_uptime_get(),
    };

    zmk_behavior_queue_add(&event, binding, true, 0);
    zmk_behavior_queue_add(&event, binding, false, 10);
}

static int layer_listener_cb(const zmk_event_t *eh) {
    const struct zmk_layer_state_changed *ev = as_zmk_layer_state_changed(eh);
    if (!initialized || ev == NULL || !layer_should_trigger(ev->layer)) {
        return 0;
    }

    pending_layer_state = ev->state;
    pending_layer_valid = true;

    uint32_t delay_ms = CONFIG_WS2812_WIDGET_LAYER_DEBOUNCE_MS;
    if (!ev->state) {
        delay_ms = MAX(delay_ms, AUTO_LAYER_OFF_MIN_DELAY_MS);
    }

    k_work_reschedule(&layer_indicator_work, K_MSEC(delay_ms));
    return 0;
}

ZMK_LISTENER(ws2812_layer_listener, layer_listener_cb);
ZMK_SUBSCRIPTION(ws2812_layer_listener, zmk_layer_state_changed);
#endif

/* -------------------------------------------------------------------------
 * Persistent layers: central listens locally and forwards state globally
 * ------------------------------------------------------------------------- */
#if WS2812_HAS_LAYER_EVENTS

#if IS_ENABLED(CONFIG_ZMK_SPLIT) && IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
#if !DT_NODE_EXISTS(DT_NODELABEL(ws2812_lsync))
#error "ws2812_lsync behavior node not found: add #include <behaviors/ws2812_layer_sync.dtsi> to the keymap"
#endif

static bool layer_needs_peripheral_sync(uint8_t layer) {
    for (size_t i = 0; i < ARRAY_SIZE(persistent_sync_layers); i++) {
        if (persistent_sync_layers[i] == layer) {
            return true;
        }
    }
    return false;
}

static void forward_persistent_layer(uint8_t layer, bool state) {
    struct zmk_behavior_binding binding = {
        .behavior_dev = DEVICE_DT_NAME(DT_NODELABEL(ws2812_lsync)),
        .param1 = layer,
        .param2 = state ? 1 : 0,
    };
    struct zmk_behavior_binding_event event = {
        .position = 0,
        .timestamp = k_uptime_get(),
    };

    zmk_behavior_queue_add(&event, binding, true, 0);
    zmk_behavior_queue_add(&event, binding, false, 10);
}
#endif

static int persistent_layer_listener_cb(const zmk_event_t *eh) {
    const struct zmk_layer_state_changed *ev = as_zmk_layer_state_changed(eh);
    if (ev == NULL) {
        return 0;
    }

#if IS_ENABLED(CONFIG_ZMK_SPLIT) && IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
    if (layer_needs_peripheral_sync(ev->layer)) {
        forward_persistent_layer(ev->layer, ev->state);
    }
#endif

    /* Local side update. If this layer belongs only to the opposite half,
     * ws2812_set_persistent_layer_active() simply finds no local slot. */
    ws2812_set_persistent_layer_active(ev->layer, ev->state);
    return 0;
}

ZMK_LISTENER(ws2812_persistent_layer_listener, persistent_layer_listener_cb);
ZMK_SUBSCRIPTION(ws2812_persistent_layer_listener, zmk_layer_state_changed);
#endif

/* -------------------------------------------------------------------------
 * Battery
 * ------------------------------------------------------------------------- */
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
    if (battery_level >= CONFIG_WS2812_WIDGET_BATTERY_LEVEL_FULL) {
        return hex_to_rgb(CONFIG_WS2812_WIDGET_BATTERY_COLOR_FULL);
    }
    if (battery_level >= CONFIG_WS2812_WIDGET_BATTERY_LEVEL_HIGH) {
        return hex_to_rgb(CONFIG_WS2812_WIDGET_BATTERY_COLOR_HIGH);
    }
    return hex_to_rgb(CONFIG_WS2812_WIDGET_BATTERY_COLOR_MEDIUM);
}

static struct indicator_request make_battery_request(struct led_rgb color,
                                                      uint8_t repeat_count,
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
#if IS_ENABLED(CONFIG_ZMK_BATTERY_REPORTING)
    uint8_t battery_level = zmk_battery_state_of_charge();
    int retry = 0;

    while (battery_level == 0 && retry++ < 10) {
        k_sleep(K_MSEC(100));
        battery_level = zmk_battery_state_of_charge();
    }

    enqueue_indicator(make_battery_request(
                          get_battery_status_color(battery_level),
                          CONFIG_WS2812_WIDGET_BATTERY_BLINK_REPEAT,
                          INDICATOR_KIND_BATTERY_MANUAL),
                      false);
#endif
}

void ws2812_indicate_battery_both(void) {
#if IS_ENABLED(CONFIG_ZMK_BATTERY_REPORTING) && IS_ENABLED(CONFIG_ZMK_SPLIT) && \
    IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL) &&                                  \
    IS_ENABLED(CONFIG_ZMK_SPLIT_BLE_CENTRAL_BATTERY_LEVEL_FETCHING)
    uint8_t local_level = zmk_battery_state_of_charge();
    int retry = 0;

    while (local_level == 0 && retry++ < 10) {
        k_sleep(K_MSEC(100));
        local_level = zmk_battery_state_of_charge();
    }

    enqueue_indicator(make_battery_request(
                          get_battery_status_color(local_level),
                          CONFIG_WS2812_WIDGET_BATTERY_BOTH_LEFT_REPEAT,
                          INDICATOR_KIND_BATTERY_MANUAL),
                      false);

    struct indicator_request sep = {
        .kind = INDICATOR_KIND_SEPARATOR,
        .hold_ms = CONFIG_WS2812_WIDGET_BATTERY_BOTH_SEPARATOR_MS,
    };
    enqueue_indicator(sep, false);

    enqueue_indicator(make_battery_request(
                          get_battery_status_color(peripheral_battery_level),
                          CONFIG_WS2812_WIDGET_BATTERY_BOTH_RIGHT_REPEAT,
                          INDICATOR_KIND_BATTERY_MANUAL),
                      false);
#else
    ws2812_indicate_battery();
#endif
}

static struct k_work_delayable battery_reminder_work;

static void schedule_next_battery_reminder(void) {
    k_work_reschedule(&battery_reminder_work,
                      K_MSEC(CONFIG_WS2812_WIDGET_BATTERY_REMINDER_INTERVAL_MS));
}

static void battery_reminder_work_cb(struct k_work *work) {
    ARG_UNUSED(work);

#if IS_ENABLED(CONFIG_WS2812_WIDGET_BATTERY_REMINDER) && IS_ENABLED(CONFIG_ZMK_BATTERY_REPORTING)
    int64_t now = k_uptime_get();
    if (last_layer_indication_ms > 0 &&
        now - last_layer_indication_ms < CONFIG_WS2812_WIDGET_BATTERY_COOLDOWN_AFTER_LAYER_MS) {
        schedule_next_battery_reminder();
        return;
    }

    if (indication_allowed(true)) {
        uint8_t battery_level = zmk_battery_state_of_charge();
        bool should_show = battery_level > 0;

#if IS_ENABLED(CONFIG_WS2812_WIDGET_BATTERY_REMINDER_ONLY_CRITICAL)
        should_show = should_show &&
                      battery_level <= CONFIG_WS2812_WIDGET_BATTERY_LEVEL_CRITICAL;
#endif

        if (should_show) {
            enqueue_indicator(make_battery_request(
                                  hex_to_rgb(CONFIG_WS2812_WIDGET_BATTERY_REMINDER_COLOR),
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

#if IS_ENABLED(CONFIG_ZMK_SPLIT) && IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL) && \
    IS_ENABLED(CONFIG_ZMK_SPLIT_BLE_CENTRAL_BATTERY_LEVEL_FETCHING)
    {
        uint8_t local = zmk_battery_state_of_charge();
        if (ev->state_of_charge != local) {
            peripheral_battery_level = ev->state_of_charge;
        }
    }
#endif

    if (ev->state_of_charge > 0 &&
        ev->state_of_charge <= CONFIG_WS2812_WIDGET_BATTERY_LEVEL_CRITICAL) {
        enqueue_indicator(make_battery_request(
                              hex_to_rgb(CONFIG_WS2812_WIDGET_BATTERY_COLOR_CRITICAL),
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
void ws2812_indicate_battery_both(void) {}
#endif

/* -------------------------------------------------------------------------
 * Connectivity manual indication
 * ------------------------------------------------------------------------- */
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

/* -------------------------------------------------------------------------
 * Activity/sleep
 * ------------------------------------------------------------------------- */
static int activity_listener_cb(const zmk_event_t *eh) {
    const struct zmk_activity_state_changed *ev = as_zmk_activity_state_changed(eh);
    if (ev == NULL) {
        return 0;
    }

    if (ev->state == ZMK_ACTIVITY_ACTIVE) {
        activity_active = true;
        ws2812_note_activity();

        if (initialized && device_is_ready(led_strip) && static_lighting_needed()) {
            k_mutex_lock(&ws2812_lighting_mutex, K_FOREVER);
            /* Ext power may have been cut externally while sleeping. Ensure it
             * is available, but do not overwrite the original pre-static state. */
            enable_ext_power_if_needed();
            redraw_static_lighting_locked();
            k_mutex_unlock(&ws2812_lighting_mutex);
        }
    } else if (ev->state == ZMK_ACTIVITY_SLEEP) {
        activity_active = false;

        if (initialized && device_is_ready(led_strip)) {
            k_mutex_lock(&ws2812_lighting_mutex, K_FOREVER);
            set_all_pixels((struct led_rgb){0, 0, 0});
            k_mutex_unlock(&ws2812_lighting_mutex);
        }
    }

    return 0;
}

ZMK_LISTENER(ws2812_activity_listener, activity_listener_cb);
ZMK_SUBSCRIPTION(ws2812_activity_listener, zmk_activity_state_changed);

/* -------------------------------------------------------------------------
 * Worker threads/init
 * ------------------------------------------------------------------------- */
static void indicator_process_thread(void *d0, void *d1, void *d2) {
    ARG_UNUSED(d0);
    ARG_UNUSED(d1);
    ARG_UNUSED(d2);

#if IS_ENABLED(CONFIG_WS2812_WIDGET_SHOW_LAYER_CHANGE) && WS2812_HAS_LAYER_EVENTS
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

K_THREAD_DEFINE(ws2812_indicator_process_tid, 1536, indicator_process_thread,
                NULL, NULL, NULL, K_LOWEST_APPLICATION_THREAD_PRIO, 0, 100);

static void indicator_init_thread(void *d0, void *d1, void *d2) {
    ARG_UNUSED(d0);
    ARG_UNUSED(d1);
    ARG_UNUSED(d2);

    if (!device_is_ready(led_strip)) {
        LOG_ERR("WS2812 LED strip device is not ready");
        return;
    }

    /* Configure half-local persistent layer ownership before initialized=true,
     * so an early GLOBAL lsync update has a valid target slot. */
#if WS2812_HALF_IS_LEFT
    ws2812_set_persistent_layer_color(PERSISTENT_LAYER_LEFT,
                                      PERSISTENT_LAYER_COLOR,
                                      0, WS2812_NUM_PIXELS);
#endif
#if WS2812_HALF_IS_RIGHT
    ws2812_set_persistent_layer_color(PERSISTENT_LAYER_RIGHT,
                                      PERSISTENT_LAYER_COLOR,
                                      0, WS2812_NUM_PIXELS);
#endif

#if WS2812_HALF_IS_LEFT && IS_ENABLED(CONFIG_ZMK_HID_INDICATORS)
    /* Read current host state as well as listening for future changes. This
     * covers cases where the host report arrived before our init thread. */
    caps_lock_active =
        (zmk_hid_indicators_get_current_profile() & HID_LED_CAPS_LOCK_BIT) != 0;
#endif

    initialized = true;
    ws2812_note_activity();

    k_mutex_lock(&ws2812_lighting_mutex, K_FOREVER);
    if (static_lighting_needed()) {
        sync_static_lighting_locked();
    } else {
        /* User config has underglow OFF on start; this guarantees the widget
         * itself starts from a known black frame without changing ZMK state. */
        set_all_pixels((struct led_rgb){0, 0, 0});
    }
    k_mutex_unlock(&ws2812_lighting_mutex);

    LOG_INF("WS2812 indicator initialized with %d pixels (half: %s, Caps host-state: %s)",
            WS2812_NUM_PIXELS,
            WS2812_HALF_IS_LEFT ? "left/central" : "right/peripheral",
#if WS2812_HALF_IS_LEFT && IS_ENABLED(CONFIG_ZMK_HID_INDICATORS)
            caps_lock_active ? "ON" : "OFF"
#else
            "n/a"
#endif
    );

#if IS_ENABLED(CONFIG_WS2812_WIDGET_SHOW_BATTERY)
#if IS_ENABLED(CONFIG_WS2812_WIDGET_SHOW_BATTERY_ON_START)
    ws2812_indicate_battery();
#endif
#if IS_ENABLED(CONFIG_WS2812_WIDGET_BATTERY_REMINDER)
    schedule_next_battery_reminder();
#endif
#endif
}

K_THREAD_DEFINE(ws2812_indicator_init_tid, 1024, indicator_init_thread,
                NULL, NULL, NULL, K_LOWEST_APPLICATION_THREAD_PRIO, 0, 200);
