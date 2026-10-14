/*
WS2812 indicator widget for ZMK v0.3.x.
Based on the old working widget.
Persistent cyan layers:
layer 2 = Russian SYMB       -> right half, cyan
layer 3 = Russian NUMB       -> left half,  cyan
layer 7 = English SYMB       -> right half, cyan
layer 8 = English NUMB       -> left half,  cyan only when latched (tog);
                              a plain momentary hold stays dark
Automatic white/red toggle layers:
layer 1  = KP_MODE
layer 13 = NAV2
layer 16 = GAMING1
layer 19 = SPEEDTYPE
Layer 20 LEFTHAND is intentionally NOT in automatic trigger mask,
because the keymap already flashes it explicitly using:
&ws2812_lsync 1
&ws2812_lsync 0
Manual reset flashes:
&ws2812_wdg 0 is used by reset_l0_green_flash.
&ws2812_wdg 9 is used by reset_kp_mode_green_flash and produces orange manual flash.
It produces the manual flash and suppresses automatic layer
ON/OFF flashes inside the suppression window.
*/
#include <string.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/led_strip.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/util.h>
#if defined(CONFIG_SOC_NRF52840)
#include <hal/nrf_power.h>
#endif
#include <zmk/activity.h>
#include <zmk/behavior.h>
#include <zmk/behavior_queue.h>
#include <zmk/events/activity_state_changed.h>
#include <zmk/events/position_state_changed.h>
#if IS_ENABLED(CONFIG_ZMK_BATTERY_REPORTING)
#include <zmk/battery.h>
#include <zmk/events/battery_state_changed.h>
#endif
#if IS_ENABLED(CONFIG_ZMK_RGB_UNDERGLOW)
#include <zmk/rgb_underglow.h>
#if IS_ENABLED(CONFIG_WS2812_WIDGET_USE_RGB_UNDERGLOW_OVERLAY_API)
/* Implemented by the RGB underglow patch used by this keyboard.
These calls pause/resume rendering without changing the persisted RGB on/off state. */
int zmk_rgb_underglow_overlay_suspend(void);
int zmk_rgb_underglow_overlay_resume(void);
#endif
#endif
#if IS_ENABLED(CONFIG_ZMK_EXT_POWER)
#include <drivers/ext_power.h>
#endif
/* -------------------------------------------------------------------------
Half detection
------------------------------------------------------------------------- */
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
ws2812_lsync (GLOBAL locality). */
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

void ws2812_indicate_layer_orange(void);

/* -------------------------------------------------------------------------
Local USB VBUS detection
Do not use ZMK USB state here. In split keyboards only the central half
owns the USB stack. The nRF52840 hardware VBUS detector works independently
on each half and reports whether the local USB VBUS pin is powered.
------------------------------------------------------------------------- */
static bool ws2812_usb_vbus_present(void) {
#if defined(CONFIG_SOC_NRF52840)
	return (NRF_POWER->USBREGSTATUS & 0x01U) != 0U;
#else
	return false;
#endif
}

#define WS2812_STRIP_NODE DT_CHOSEN(zmk_ws2812_widget)
#if !DT_NODE_EXISTS(WS2812_STRIP_NODE)
#error "WS2812 widget chosen node zmk,ws2812-widget not found"
#endif
#define WS2812_NUM_PIXELS DT_PROP(WS2812_STRIP_NODE, chain_length)

/* -------------------------------------------------------------------------
Widget brightness
This brightness is independent from normal ZMK RGB underglow brightness.
It applies only to colors physically written by this widget: Caps Lock,
persistent layers, layer/manual flashes, battery and connectivity.
The value is adjustable from the keymap through &ws2812_wdg 6/7/8 and is
persisted in Zephyr settings when CONFIG_SETTINGS is enabled.
------------------------------------------------------------------------- */
#define WS2812_WIDGET_BRIGHTNESS_DEFAULT 50U
#define WS2812_WIDGET_BRIGHTNESS_MIN 10U
#define WS2812_WIDGET_BRIGHTNESS_MAX 100U
#define WS2812_WIDGET_BRIGHTNESS_STEP 10U

BUILD_ASSERT(WS2812_WIDGET_BRIGHTNESS_MIN <= WS2812_WIDGET_BRIGHTNESS_DEFAULT,
	     "WS2812 widget default brightness is below the minimum");
BUILD_ASSERT(WS2812_WIDGET_BRIGHTNESS_DEFAULT <= WS2812_WIDGET_BRIGHTNESS_MAX,
	     "WS2812 widget default brightness is above the maximum");
BUILD_ASSERT(WS2812_WIDGET_BRIGHTNESS_MAX <= 100U,
	     "WS2812 widget brightness maximum must not exceed 100 percent");
BUILD_ASSERT(CONFIG_WS2812_WIDGET_FADE_STEP_MS > 0,
	     "CONFIG_WS2812_WIDGET_FADE_STEP_MS must be greater than zero");
BUILD_ASSERT(WS2812_NUM_PIXELS > 0 && WS2812_NUM_PIXELS <= 255,
	     "chain-length must be the number of LEDs on THIS half (1..255)");

/* -------------------------------------------------------------------------
Persistent layer configuration
Old working logic preserved:
layer 2 -> right half cyan
layer 3 -> left half cyan
English persistent layers:
layer 7 -> English symbols persistent, cyan on both halves
layer 8 -> English numbers persistent, left half cyan only when latched
------------------------------------------------------------------------- */
#define KP_MODE_LAYER                 1
#define SYMB_LAYER                    2
#define NUMB_LAYER                    3
#define SYMB_ENG_LAYER                7
#define NUMB_ENG_NOLIGHT_LAYER        8
#define NAV2_LAYER                    13
#define GAMING1_LAYER                 16
#define SPEEDTYPE_LAYER               19

#define PERSISTENT_LAYER_LEFT         NUMB_LAYER
#define PERSISTENT_LAYER_RIGHT        SYMB_LAYER
#define PERSISTENT_LAYER_LEFT_ENG     NUMB_ENG_NOLIGHT_LAYER
#define PERSISTENT_LAYER_RIGHT_ENG    SYMB_ENG_LAYER
#define PERSISTENT_LAYER_COLOR        0x00FFFF

/*
Automatic white/red layer-change flashes.
Only these layers produce normal ON/OFF flashes:
1  KP_MODE
13 NAV2
16 GAMING1
19 SPEEDTYPE
Cyan persistent layers 2, 3, 7, 8 do NOT flash. They only hold static color.
LEFTHAND layer 20 is not here because keymap explicitly calls
&ws2812_lsync 1/0 for it.
*/
#define WS2812_AUTOMATIC_TOGGLE_LAYER_MASK                                             \
	(BIT(KP_MODE_LAYER) |                                                             \
	 BIT(NAV2_LAYER) |                                                                \
	 BIT(GAMING1_LAYER) |                                                             \
	 BIT(SPEEDTYPE_LAYER))

#define WS2812_WIDGET_LAYER_COLOR_MANUAL_ORANGE 0xFF4500

/* The central only forwards persistent layers that are drawn on the other half. */
#if IS_ENABLED(CONFIG_ZMK_SPLIT) && IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
#if WS2812_HALF_IS_LEFT
static const uint8_t __maybe_unused persistent_sync_layers[] = {
	PERSISTENT_LAYER_RIGHT,
	PERSISTENT_LAYER_RIGHT_ENG,
};
#else
static const uint8_t __maybe_unused persistent_sync_layers[] = {
	PERSISTENT_LAYER_LEFT,
	PERSISTENT_LAYER_LEFT_ENG,
};
#endif
#else
static const uint8_t __maybe_unused persistent_sync_layers[] = {
	PERSISTENT_LAYER_LEFT,
	PERSISTENT_LAYER_RIGHT,
	PERSISTENT_LAYER_LEFT_ENG,
	PERSISTENT_LAYER_RIGHT_ENG,
};
#endif

/* -------------------------------------------------------------------------
Layer 8 latched/momentary detection for the LEFT half.
Layer 8 is used in two ways:
latched by combo / &tog NUMB_ENG_NOLIGHT_LAYER
momentary by holding keys that switch to layer 8
Left half must glow cyan only in the latched case.
Current keymap momentary sources for layer 8:
position 13 - left outer key in row 2 on layer 7:
            &bspc_to_layer_ht NUMB_ENG_NOLIGHT_LAYER FSLH
position 27 - right outer key in row 2 on layer 7:
            &bspc_to_layer_ht NUMB_ENG_NOLIGHT_LAYER BSLH
position 46 - right thumb digits key on layers 1/7:
            &thumb_num_sym_fn NUMB_ENG_NOLIGHT_LAYER 0
------------------------------------------------------------------------- */
#if WS2812_HALF_IS_LEFT
static const uint8_t eng_numb_momentary_positions[] = {13, 27, 46};
static bool eng_numb_layer_on;
static bool eng_numb_momentary;
static volatile uint8_t eng_numb_momentary_key_count;

static bool eng_numb_is_momentary_position(uint8_t position) {
	for (size_t i = 0; i < ARRAY_SIZE(eng_numb_momentary_positions); i++) {
		if (eng_numb_momentary_positions[i] == position) {
			return true;
		}
	}
	return false;
}

static void update_eng_numb_cyan(void) {
	ws2812_set_persistent_layer_active(
		PERSISTENT_LAYER_LEFT_ENG,
		eng_numb_layer_on && !eng_numb_momentary);
}
#endif

/* -------------------------------------------------------------------------
Caps Lock indicator
Old working widget configuration preserved:
LEFT half LEDs 0..1
electric purple 0x8000FF
------------------------------------------------------------------------- */
#if WS2812_HALF_IS_LEFT
#define CAPS_INDICATOR_START 0
#define CAPS_INDICATOR_COUNT 2
#define CAPS_INDICATOR_COLOR 0x8000FF
#define HID_LED_CAPS_LOCK_BIT BIT(1)

static bool caps_lock_active;
#endif

/* Automatic layer-OFF indication is deliberately delayed slightly longer than
the keymap macro's 20 ms step. This gives reset_l0_green_flash time to call
&ws2812_wdg 0 and cancel/suppress the red OFF flash deterministically.
Automatic layer-ON indication is also delayed enough for reset_kp_mode_green_flash
to suppress the white ON flash when switching to layer 1 by the four-key combo.
*/
#define RESET_MANUAL_SUPPRESS_MS 250U
#define AUTO_LAYER_OFF_MIN_DELAY_MS 80U
#define AUTO_LAYER_ON_MIN_DELAY_MS 80U

/* -------------------------------------------------------------------------
Types/state
------------------------------------------------------------------------- */
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
static struct led_rgb output_pixels[WS2812_NUM_PIXELS];
static struct persistent_layer_config persistent_layers[MAX_PERSISTENT_LAYERS];
static uint8_t widget_brightness_percent = WS2812_WIDGET_BRIGHTNESS_DEFAULT;
static bool initialized;
static bool widget_enabled = IS_ENABLED(CONFIG_WS2812_WIDGET_ENABLED_ON_START);
static bool activity_active = true;
static int64_t last_activity_ms;
static int64_t last_layer_indication_ms;

/* One owner for restoration of normal RGB/ext-power state.
It is captured exactly once when the first static owner appears and restored
exactly once when the last static owner disappears. */
static bool normal_state_saved;
static bool normal_underglow_was_on;
static bool normal_ext_power_was_on = true;

/* Serializes all direct writes to the LED strip and all state transitions that
save/restore normal underglow. */
K_MUTEX_DEFINE(ws2812_lighting_mutex);

/* IMPORTANT: the animation thread holds ws2812_lighting_mutex for the whole
blink. Everything that runs in ZMK's event/work context (key press listener,
layer listener, Caps listener, behaviors) must therefore NEVER wait on that
mutex, otherwise the whole keyboard stalls until the animation is over.
Such callers only record the new state and call ws2812_request_static_sync().
If the strip is busy, the running animation is aborted and the state is applied
by whoever releases the mutex (see ws2812_lighting_unlock()). */
static atomic_t static_sync_pending = ATOMIC_INIT(0);
static atomic_t animation_abort = ATOMIC_INIT(0);
K_SEM_DEFINE(animation_abort_sem, 0, 1);

static void idle_work_handler(struct k_work *work);
K_WORK_DELAYABLE_DEFINE(idle_work, idle_work_handler);

static bool idle_timer_enabled = IS_ENABLED(CONFIG_WS2812_IDLE_TIMER);
static bool idle_display_off = false;
static bool idle_underglow_suspended = false;
static uint32_t idle_timeout_minutes = CONFIG_WS2812_IDLE_TIMEOUT_MINUTES;

K_MSGQ_DEFINE(indicator_msgq, sizeof(struct indicator_request), 12, 4);

#if IS_ENABLED(CONFIG_ZMK_BATTERY_REPORTING) && IS_ENABLED(CONFIG_WS2812_WIDGET_SHOW_BATTERY) && IS_ENABLED(CONFIG_ZMK_SPLIT) && IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL) && IS_ENABLED(CONFIG_ZMK_SPLIT_BLE_CENTRAL_BATTERY_LEVEL_FETCHING)
static uint8_t peripheral_battery_level;
#endif

/* -------------------------------------------------------------------------
Pixel helpers
------------------------------------------------------------------------- */
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
	/* Keep pixels[] at full logical color and apply brightness only at output.
	 * This makes brightness changes lossless and immediately reversible. */
	for (int i = 0; i < WS2812_NUM_PIXELS; i++) {
		output_pixels[i].r =
			(uint8_t)(((uint16_t)pixels[i].r * widget_brightness_percent) / 100U);
		output_pixels[i].g =
			(uint8_t)(((uint16_t)pixels[i].g * widget_brightness_percent) / 100U);
		output_pixels[i].b =
			(uint8_t)(((uint16_t)pixels[i].b * widget_brightness_percent) / 100U);
	}
	return led_strip_update_rgb(led_strip, output_pixels, WS2812_NUM_PIXELS);
}

static int set_all_pixels(struct led_rgb color) {
	buffer_fill(color);
	return flush_pixels();
}

/* Forward declarations used by brightness control. */
static bool static_lighting_needed(void);
static void redraw_static_lighting_locked(void);
static void sync_static_lighting_locked(void);

/* -------------------------------------------------------------------------
Non-blocking access to the LED strip
------------------------------------------------------------------------- */
static void ws2812_abort_animation(void) {
	atomic_set(&animation_abort, 1);
	k_sem_give(&animation_abort_sem);
}

/* Release the lighting mutex and apply any static-state change that another
 * thread could not apply because we were holding the mutex. Every owner of
 * ws2812_lighting_mutex must unlock through this function. */
static void ws2812_lighting_unlock(void) {
	k_mutex_unlock(&ws2812_lighting_mutex);

	while (atomic_get(&static_sync_pending) != 0) {
		if (k_mutex_lock(&ws2812_lighting_mutex, K_NO_WAIT) != 0) {
			/* Someone else owns the strip now; they flush on their unlock. */
			return;
		}
		if (atomic_cas(&static_sync_pending, 1, 0)) {
			sync_static_lighting_locked();
		}
		k_mutex_unlock(&ws2812_lighting_mutex);
	}
}

/* Re-sync persistent/Caps lighting WITHOUT ever waiting for a running blink.
 * Strip idle: applied immediately.
 * Strip busy: the pending flag is picked up when the owner unlocks, and the
 * blink is cut short (if requested) so the new state shows up at once. */
static void ws2812_request_static_sync(bool interrupt_animation) {
	atomic_set(&static_sync_pending, 1);

	if (k_mutex_lock(&ws2812_lighting_mutex, K_NO_WAIT) == 0) {
		if (atomic_cas(&static_sync_pending, 1, 0)) {
			sync_static_lighting_locked();
		}
		ws2812_lighting_unlock();
		return;
	}

	if (interrupt_animation) {
		ws2812_abort_animation();
	}
}

/* Take the mutex; if a blink is running, cut it short instead of sitting
 * behind it (the animation thread notices within one frame). */
static void ws2812_lock_interrupting(void) {
	if (k_mutex_lock(&ws2812_lighting_mutex, K_NO_WAIT) == 0) {
		return;
	}
	ws2812_abort_animation();
	k_mutex_lock(&ws2812_lighting_mutex, K_FOREVER);
}

/* -------------------------------------------------------------------------
Widget brightness control + persistence
------------------------------------------------------------------------- */
static uint8_t clamp_widget_brightness(uint8_t percent) {
	return CLAMP(percent, WS2812_WIDGET_BRIGHTNESS_MIN, WS2812_WIDGET_BRIGHTNESS_MAX);
}

#if IS_ENABLED(CONFIG_SETTINGS)
static void save_widget_brightness_work_handler(struct k_work *work) {
	ARG_UNUSED(work);
	int rc = settings_save_one("ws2812_widget/brightness",
				   &widget_brightness_percent,
				   sizeof(widget_brightness_percent));
	if (rc < 0) {
		LOG_WRN("Failed to save WS2812 widget brightness: %d", rc);
	}
}

K_WORK_DELAYABLE_DEFINE(widget_brightness_save_work, save_widget_brightness_work_handler);
#endif

static void schedule_widget_brightness_save(void) {
#if IS_ENABLED(CONFIG_SETTINGS)
	k_work_reschedule(&widget_brightness_save_work,
			  K_MSEC(CONFIG_ZMK_SETTINGS_SAVE_DEBOUNCE));
#endif
}

void ws2812_set_widget_brightness(uint8_t percent) {
	uint8_t new_value = clamp_widget_brightness(percent);

	widget_brightness_percent = new_value;
	/* Never wait for a running blink: its following frames and its final
	 * static redraw already use the new brightness. */
	ws2812_request_static_sync(false);

	schedule_widget_brightness_save();
	LOG_INF("WS2812 widget brightness: %u%%", widget_brightness_percent);
}

void ws2812_change_widget_brightness(int8_t direction) {
	int value = widget_brightness_percent;

	if (direction > 0) {
		value += WS2812_WIDGET_BRIGHTNESS_STEP;
	} else if (direction < 0) {
		value -= WS2812_WIDGET_BRIGHTNESS_STEP;
	} else {
		return;
	}

	value = CLAMP(value, WS2812_WIDGET_BRIGHTNESS_MIN, WS2812_WIDGET_BRIGHTNESS_MAX);
	ws2812_set_widget_brightness((uint8_t)value);
}

void ws2812_reset_widget_brightness(void) {
	ws2812_set_widget_brightness(WS2812_WIDGET_BRIGHTNESS_DEFAULT);
}

uint8_t ws2812_get_widget_brightness(void) {
	return widget_brightness_percent;
}

#if IS_ENABLED(CONFIG_SETTINGS)
static int ws2812_widget_settings_set(const char *name, size_t len,
				      settings_read_cb read_cb, void *cb_arg) {
	if (strcmp(name, "brightness") != 0) {
		return -ENOENT;
	}

	if (len != sizeof(widget_brightness_percent)) {
		return -EINVAL;
	}

	uint8_t saved = WS2812_WIDGET_BRIGHTNESS_DEFAULT;
	int rc = read_cb(cb_arg, &saved, sizeof(saved));
	if (rc < 0) {
		return rc;
	}

	widget_brightness_percent = clamp_widget_brightness(saved);
	LOG_INF("Loaded WS2812 widget brightness: %u%%", widget_brightness_percent);
	return 0;
}

SETTINGS_STATIC_HANDLER_DEFINE(ws2812_widget_brightness,
			       "ws2812_widget",
			       NULL,
			       ws2812_widget_settings_set,
			       NULL,
			       NULL);
#endif

/* -------------------------------------------------------------------------
Normal underglow/ext-power helpers
------------------------------------------------------------------------- */
static bool pause_underglow_if_needed(void) {
	bool was_on = false;

#if IS_ENABLED(CONFIG_WS2812_WIDGET_PAUSE_RGB_UNDERGLOW) && IS_ENABLED(CONFIG_ZMK_RGB_UNDERGLOW)
	if (zmk_rgb_underglow_get_state(&was_on) == 0 && was_on) {
#if IS_ENABLED(CONFIG_WS2812_WIDGET_USE_RGB_UNDERGLOW_OVERLAY_API)
		zmk_rgb_underglow_overlay_suspend();
#else
		zmk_rgb_underglow_off();
#endif
		k_sleep(K_MSEC(CONFIG_WS2812_WIDGET_UNDERGLOW_OFF_DELAY_MS));
	}
#endif

	return was_on;
}

static void restore_underglow_if_needed(bool was_on) {
#if IS_ENABLED(CONFIG_WS2812_WIDGET_PAUSE_RGB_UNDERGLOW) && IS_ENABLED(CONFIG_ZMK_RGB_UNDERGLOW)
	if (was_on) {
		k_sleep(K_MSEC(CONFIG_WS2812_WIDGET_UNDERGLOW_RESTORE_DELAY_MS));
#if IS_ENABLED(CONFIG_WS2812_WIDGET_USE_RGB_UNDERGLOW_OVERLAY_API)
		zmk_rgb_underglow_overlay_resume();
#else
		zmk_rgb_underglow_on();
#endif
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
Persistent/static state
------------------------------------------------------------------------- */
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

	/* Record the state and request a re-sync. This must not wait for a running
	 * blink: this function is called from the layer listener, i.e. from the
	 * thread that also processes key events. A blink in progress is aborted
	 * and the animation thread hands its transient underglow snapshot over to
	 * the static owner, so the original underglow state is never lost. */
	if (persistent_layers[slot].active != active) {
		persistent_layers[slot].active = active;
		ws2812_request_static_sync(true);
	}
}

/* -------------------------------------------------------------------------
Caps Lock: REAL host state
------------------------------------------------------------------------- */
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

	/* Record the state and request a re-sync without waiting for a running
	blink. If a blink is in progress it is aborted and Caps takes ownership
	of the strip as soon as the animation thread unwinds. */
	if (new_caps_state != caps_lock_active) {
		caps_lock_active = new_caps_state;
		ws2812_request_static_sync(true);
	}

	LOG_DBG("Host Caps Lock state: %s", new_caps_state ? "ON" : "OFF");
	return 0;
}

ZMK_LISTENER(ws2812_hid_caps_listener, hid_caps_listener_cb);
ZMK_SUBSCRIPTION(ws2812_hid_caps_listener, zmk_hid_indicators_changed);
#endif

/* -------------------------------------------------------------------------
Fade/transient helpers
------------------------------------------------------------------------- */
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

/* Interruptible sleep. Returns true if a state change (layer, Caps, idle wake)
 * asked the running animation to stop. */
static bool animation_sleep(uint32_t ms) {
	if (atomic_get(&animation_abort) != 0) {
		return true;
	}
	if (ms == 0) {
		return false;
	}
	return k_sem_take(&animation_abort_sem, K_MSEC(ms)) == 0;
}

/* Both fade helpers return true when aborted. */
static bool fade_from_black_to_color(struct led_rgb color, uint16_t duration_ms) {
	uint16_t steps = fade_step_count(duration_ms);
	if (steps == 0) {
		set_all_pixels(color);
		return false;
	}

	uint16_t delay_ms = MAX(1, duration_ms / steps);

	for (uint16_t step = 0; step <= steps; step++) {
		set_all_pixels(scale_rgb(color, step, steps));
		if (animation_sleep(delay_ms)) {
			return true;
		}
	}
	return false;
}

static bool fade_from_color_to_black(struct led_rgb color, uint16_t duration_ms) {
	uint16_t steps = fade_step_count(duration_ms);
	if (steps == 0) {
		set_all_pixels((struct led_rgb){0, 0, 0});
		return false;
	}

	uint16_t delay_ms = MAX(1, duration_ms / steps);

	for (uint16_t step = 0; step <= steps; step++) {
		set_all_pixels(scale_rgb(color, steps - step, steps));
		if (animation_sleep(delay_ms)) {
			return true;
		}
	}
	return false;
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
	if (!initialized || !widget_enabled || !activity_active || idle_display_off) {
		return false;
	}

	if (periodic && !periodic_indication_allowed()) {
		return false;
	}

	return true;
}

static void ws2812_set_idle_display_local(bool off) {
	if (!initialized || !device_is_ready(led_strip)) {
		idle_display_off = off;
		return;
	}

	/* Fast path. This runs on EVERY key press (via ws2812_note_activity()).
	 * Do not touch the lighting mutex when there is nothing to change,
	 * otherwise typing stalls for as long as a blink holds the mutex. */
	if (off == idle_display_off) {
		return;
	}

	ws2812_lock_interrupting();

	if (off) {
		if (!idle_display_off) {
			idle_display_off = true;

#if IS_ENABLED(CONFIG_WS2812_RGB_IDLE_TIMER)
			/* Writing one black frame is not enough: the normal ZMK underglow
			 * worker redraws the strip every ~50 ms. Pause that renderer while
			 * idle, but only when it is not already paused by a persistent
			 * widget owner (Caps/persistent layer). */
			if (!normal_state_saved && !static_lighting_needed()) {
				idle_underglow_suspended = pause_underglow_if_needed();
			}
#endif

			for (int i = 0; i < WS2812_NUM_PIXELS; i++) {
				output_pixels[i] = (struct led_rgb){0, 0, 0};
			}
			led_strip_update_rgb(led_strip, output_pixels, WS2812_NUM_PIXELS);
		}
	} else {
		if (idle_display_off) {
			idle_display_off = false;

			if (idle_underglow_suspended) {
				idle_underglow_suspended = false;
				restore_underglow_if_needed(true);
			} else {
				/* pixels[] keeps the logical static/indicator state. Re-apply
				 * that state when the widget owns the strip. */
				flush_pixels();
			}
		}
	}

	ws2812_lighting_unlock();
}

void ws2812_apply_idle_sync(bool off) {
	if (off) {
		/* The GLOBAL central OFF event is authoritative. Cancel any leftover
		 * peripheral deadline so it cannot produce a second, independent OFF. */
		k_work_cancel_delayable(&idle_work);
	}

	ws2812_set_idle_display_local(off);
}

#if IS_ENABLED(CONFIG_ZMK_SPLIT) && IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
#if !DT_NODE_EXISTS(DT_NODELABEL(ws2812_async))
#error "ws2812_async behavior node not found: add #include <behaviors/ws2812_activity_sync.dtsi> to the keymap"
#endif

/* Keep the peripheral's local idle timer aligned while the user is typing on
the central half.  The peripheral already sees its own key presses locally,
so only central-originated activity needs to be mirrored. */
#define WS2812_ACTIVITY_SYNC_INTERVAL_MS 10000U
static int64_t last_activity_sync_ms;

static void forward_activity_to_all_halves(void) {
	struct zmk_behavior_binding binding = {
		.behavior_dev = DEVICE_DT_NAME(DT_NODELABEL(ws2812_async)),
		.param1 = 0,
		.param2 = 0,
	};

	struct zmk_behavior_binding_event event = {
		.position = 0,
		.timestamp = k_uptime_get(),
	};

	int rc = zmk_behavior_queue_add(&event, binding, true, 0);
	if (rc != 0) {
		LOG_WRN("WS2812 activity sync queue full: %d", rc);
	}
}

/* The central half owns the idle deadline. Send the OFF state through the
 * existing GLOBAL behavior so both physical LED strips are blanked by the
 * same timeout event instead of relying on two independent local timers. */
static void forward_idle_off_to_all_halves(void) {
	struct zmk_behavior_binding binding = {
		.behavior_dev = DEVICE_DT_NAME(DT_NODELABEL(ws2812_async)),
		.param1 = 1,
		.param2 = 0,
	};

	struct zmk_behavior_binding_event event = {
		.position = 0,
		.timestamp = k_uptime_get(),
	};

	int rc = zmk_behavior_queue_add(&event, binding, true, 0);
	if (rc != 0) {
		LOG_WRN("WS2812 idle OFF sync queue full: %d", rc);
	}
}
#endif

void ws2812_note_activity(void) {
	last_activity_ms = k_uptime_get();

	/* Always wake the local half immediately. On split central, the key/activity
	 * listeners below mirror this wake to the peripheral when required. */
	ws2812_set_idle_display_local(false);
	ws2812_idle_timer_reset();
}

static void idle_work_handler(struct k_work *work) {
	ARG_UNUSED(work);

	if (!idle_timer_enabled) {
		return;
	}

	int64_t timeout_ms = (int64_t)idle_timeout_minutes * 60LL * 1000LL;
	int64_t elapsed_ms = k_uptime_get() - last_activity_ms;

	if (last_activity_ms > 0 && elapsed_ms < timeout_ms) {
		k_work_reschedule(&idle_work, K_MSEC(timeout_ms - elapsed_ms));
		return;
	}

#if IS_ENABLED(CONFIG_ZMK_SPLIT) && !IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
	/* Central activity is mirrored periodically, so the peripheral's local
	 * deadline can differ slightly. Do not blank it independently: wait for
	 * the central half's GLOBAL OFF event so both halves turn off together. */
	LOG_DBG("WS2812 peripheral idle deadline reached; waiting for central OFF sync");
	return;
#endif

	LOG_INF("WS2812 idle timeout fired after %u min", idle_timeout_minutes);
	ws2812_set_idle_display_local(true);

#if IS_ENABLED(CONFIG_ZMK_SPLIT) && IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
	forward_idle_off_to_all_halves();
#endif
}

void ws2812_idle_timer_init(void) {
	/* idle_work is statically initialized with K_WORK_DELAYABLE_DEFINE.
	 * This avoids a boot-time race where activity listeners could try to
	 * reschedule the work before the delayed init thread initialized it. */
}

void ws2812_idle_timer_reset(void) {
	if (!idle_timer_enabled) {
		return;
	}

	k_work_reschedule(&idle_work, K_MINUTES(idle_timeout_minutes));
	LOG_DBG("WS2812 local idle timer reset: %u min", idle_timeout_minutes);
}

void ws2812_idle_timer_toggle(void) {
	idle_timer_enabled = !idle_timer_enabled;

	if (idle_timer_enabled) {
		ws2812_idle_timer_reset();
	} else {
		k_work_cancel_delayable(&idle_work);
		ws2812_set_idle_display_local(false);
	}

	LOG_INF("WS2812 local idle timer %s (%u min)", idle_timer_enabled ? "ON" : "OFF",
		idle_timeout_minutes);
}

void ws2812_idle_timeout_change(int8_t direction) {
	if (direction > 0) {
		if (idle_timeout_minutes < 60U) {
			idle_timeout_minutes++;
		}
	} else if (direction < 0 && idle_timeout_minutes > 1U) {
		idle_timeout_minutes--;
	}

	/* &rgb_timer is GLOBAL, so both halves update the same timeout value. */
	idle_timer_enabled = true;
	ws2812_idle_timer_reset();

	LOG_INF("WS2812 local idle timeout: %u min (timer ON)", idle_timeout_minutes);
}

bool ws2812_idle_timer_enabled(void) {
	return idle_timer_enabled;
}

void ws2812_set_indication_enabled(bool enabled) {
	widget_enabled = enabled;
	ws2812_note_activity();

	if (initialized && device_is_ready(led_strip) && static_lighting_needed()) {
		/* Caps/persistent status remains authoritative even if temporary
		 * widget flashes are disabled. With no static owner, toggling the
		 * widget must not write black over normal ZMK underglow. It only
		 * enables/disables temporary indicators. */
		ws2812_request_static_sync(false);
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

	/* Battery critical warning is local to each half. If this half is powered
	 * through USB, suppress only this half's warning. Other indicators remain
	 * unchanged. The check is repeated at execution time so a cable inserted
	 * after queueing the warning cancels the remaining flashes. */
	if (request->kind == INDICATOR_KIND_BATTERY_CRITICAL &&
	    ws2812_usb_vbus_present()) {
		return;
	}

	k_mutex_lock(&ws2812_lighting_mutex, K_FOREVER);

	/* Start from a clean abort state, then apply any static-state change that
	 * arrived while this request was waiting for the strip. */
	atomic_set(&animation_abort, 0);
	k_sem_reset(&animation_abort_sem);
	if (atomic_cas(&static_sync_pending, 1, 0)) {
		sync_static_lighting_locked();
	}

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

	bool aborted = false;

	for (uint8_t i = 0; i < request->repeat_count && !aborted; i++) {
		if (fade_from_black_to_color(request->color, request->fade_in_ms)) {
			aborted = true;
			break;
		}

		if (request->hold_ms > 0) {
			set_all_pixels(request->color);
			if (animation_sleep(request->hold_ms)) {
				aborted = true;
				break;
			}
		}

		if (fade_from_color_to_black(request->color, request->fade_out_ms)) {
			aborted = true;
			break;
		}
		set_all_pixels((struct led_rgb){0, 0, 0});

		if (i + 1 < request->repeat_count && request->gap_ms > 0) {
			if (animation_sleep(request->gap_ms)) {
				aborted = true;
				break;
			}
		}
	}

	if (aborted) {
		set_all_pixels((struct led_rgb){0, 0, 0});
	}

	if (static_lighting_needed()) {
		if (!had_static_owner && !normal_state_saved) {
			/* A persistent layer / Caps became active while this blink was
			 * running. The blink already paused underglow; hand that snapshot
			 * to the static owner instead of capturing the paused state. */
			normal_underglow_was_on = transient_underglow_was_on;
			normal_ext_power_was_on = transient_ext_power_was_on;
			normal_state_saved = true;
		} else {
			/* Critical rule: every transient flash returns to persistent+Caps,
			 * not blindly to plain underglow. */
			capture_normal_state_locked();
		}
		redraw_static_lighting_locked();
	} else if (had_static_owner) {
		/* Static owner disappeared while the blink was running. */
		restore_normal_state_locked();
	} else {
		restore_underglow_if_needed(transient_underglow_was_on);
		restore_ext_power_if_needed(transient_ext_power_was_on,
					    transient_underglow_was_on);
	}

	ws2812_lighting_unlock();
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
Layer/manual indications
------------------------------------------------------------------------- */
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

static __maybe_unused struct indicator_request make_manual_layer_request_with_color(uint32_t color_hex) {
	struct indicator_request request = make_manual_layer_request();
	request.color = hex_to_rgb(color_hex);
	return request;
}

#if IS_ENABLED(CONFIG_WS2812_WIDGET_SHOW_LAYER_CHANGE) && WS2812_HAS_LAYER_EVENTS
static struct k_work_delayable layer_indicator_work;
static bool pending_layer_state;
static bool pending_layer_valid;
static uint32_t manual_layer_suppress_until_ms;
#endif

static void indicate_manual_layer_with_color(uint32_t color_hex) {
#if IS_ENABLED(CONFIG_WS2812_WIDGET_SHOW_LAYER_CHANGE)
	last_layer_indication_ms = k_uptime_get();

#if WS2812_HAS_LAYER_EVENTS
	/*
	 * This is the manual confirmation used by reset_l0_green_flash and
	 * reset_kp_mode_green_flash.
	 *
	 * Cancel a pending automatic layer ON/OFF indication and suppress any
	 * near-simultaneous delayed one. This makes the four-key reset to layer 0
	 * or layer 1 show only the manual flash.
	 */
	manual_layer_suppress_until_ms = k_uptime_get_32() + RESET_MANUAL_SUPPRESS_MS;
	pending_layer_valid = false;
	k_work_cancel_delayable(&layer_indicator_work);
#endif

	enqueue_indicator(make_manual_layer_request_with_color(color_hex), false);
#endif
}

void ws2812_indicate_layer(void) {
	indicate_manual_layer_with_color(CONFIG_WS2812_WIDGET_LAYER_COLOR_MANUAL);
}

void ws2812_indicate_layer_orange(void) {
	indicate_manual_layer_with_color(WS2812_WIDGET_LAYER_COLOR_MANUAL_ORANGE);
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
static bool layer_is_automatic_toggle_layer(uint8_t layer) {
	if (layer >= 32) {
		return false;
	}

	return (WS2812_AUTOMATIC_TOGGLE_LAYER_MASK & BIT(layer)) != 0;
}

static bool layer_should_trigger(uint8_t layer) {
	if (layer == zmk_keymap_layer_default()) {
		return false;
	}

	/*
	 * Use the explicit local whitelist instead of Kconfig trigger masks.
	 *
	 * This is required because the new keymap shifted layer numbers:
	 *   old layer 6  NAV2      -> new layer 13
	 *   old layer 7  GAMING1   -> new layer 16
	 *   old layer 11 SPEEDTYPE -> new layer 19
	 *   new layer 1  KP_MODE   -> Android mode
	 *
	 * Cyan persistent layers 2, 3, 7, 8 must not produce white/red flashes.
	 */
	return layer_is_automatic_toggle_layer(layer);
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

	/*
	 * Manual reset confirmation wins over automatic layer ON/OFF flashes.
	 *
	 * Old widget suppressed only OFF. New keymap can reset directly to layer 1,
	 * which is an automatic toggle layer, so ON must also be suppressed inside
	 * the manual reset window.
	 */
	if (suppress_window_active()) {
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

	/* Use the old ZMK behavior queue path. It is the mechanism that behaved
	 * predictably in the old widget. ws2812_lsync does its work on press and
	 * ignores release, but keeping press+release matches the old split sync. */
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
	} else {
		delay_ms = MAX(delay_ms, AUTO_LAYER_ON_MIN_DELAY_MS);
	}

	k_work_reschedule(&layer_indicator_work, K_MSEC(delay_ms));
	return 0;
}

ZMK_LISTENER(ws2812_layer_listener, layer_listener_cb);
ZMK_SUBSCRIPTION(ws2812_layer_listener, zmk_layer_state_changed);
#endif

/* -------------------------------------------------------------------------
Persistent layers: central listens locally and forwards state globally
------------------------------------------------------------------------- */
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

	/* Old working split-sync mechanism. Do not replace with direct invoke here:
	 * the behavior queue is the path that kept halves predictable in the old
	 * widget. */
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

#if WS2812_HALF_IS_LEFT
	/*
	 * Layer 8 on the left half is cyan only when latched.
	 *
	 * If layer 8 became active while one of the momentary keys was held,
	 * mark it as momentary and keep the left half dark.
	 */
	if (ev->layer == PERSISTENT_LAYER_LEFT_ENG) {
		eng_numb_layer_on = ev->state;

		if (ev->state) {
			eng_numb_momentary = (eng_numb_momentary_key_count > 0U);
		} else {
			eng_numb_momentary = false;
		}

		update_eng_numb_cyan();
		return 0;
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
Battery
------------------------------------------------------------------------- */
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
#if IS_ENABLED(CONFIG_ZMK_BATTERY_REPORTING) && IS_ENABLED(CONFIG_ZMK_SPLIT) && IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL) && IS_ENABLED(CONFIG_ZMK_SPLIT_BLE_CENTRAL_BATTERY_LEVEL_FETCHING)
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

#if IS_ENABLED(CONFIG_ZMK_SPLIT) && IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL) && IS_ENABLED(CONFIG_ZMK_SPLIT_BLE_CENTRAL_BATTERY_LEVEL_FETCHING)
	{
		uint8_t local = zmk_battery_state_of_charge();
		if (ev->state_of_charge != local) {
			peripheral_battery_level = ev->state_of_charge;
		}
	}
#endif

	if (ev->state_of_charge > 0 &&
	    ev->state_of_charge <= CONFIG_WS2812_WIDGET_BATTERY_LEVEL_CRITICAL &&
	    !ws2812_usb_vbus_present()) {
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
Connectivity manual indication
------------------------------------------------------------------------- */
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
Activity/sleep + split wake synchronization
------------------------------------------------------------------------- */
/* Every physical key press wakes and resets the timer on the half that sees it.
Peripheral key events are also reported to the central, so right-side keys
refresh both halves naturally. Central-originated keys are mirrored to the
peripheral with the lightweight activity heartbeat below. */
static int key_activity_listener_cb(const zmk_event_t *eh) {
	const struct zmk_position_state_changed *ev = as_zmk_position_state_changed(eh);

#if WS2812_HAS_LAYER_EVENTS && WS2812_HALF_IS_LEFT
	/*
	 * Track both press and release of keys that can momentary activate layer 8.
	 * If layer 8 turns on while at least one such key is physically down,
	 * treat it as a momentary hold, not as a latch.
	 */
	if (ev != NULL && eng_numb_is_momentary_position(ev->position)) {
		if (ev->state) {
			if (eng_numb_momentary_key_count < 255U) {
				eng_numb_momentary_key_count++;
			}
		} else if (eng_numb_momentary_key_count > 0U) {
			eng_numb_momentary_key_count--;
		}
	}
#endif

	if (ev == NULL || !ev->state) {
		return ZMK_EV_EVENT_BUBBLE;
	}

#if IS_ENABLED(CONFIG_ZMK_SPLIT) && IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
	bool was_off = idle_display_off;
#endif

	ws2812_note_activity();

#if IS_ENABLED(CONFIG_ZMK_SPLIT) && IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
	/* Peripheral-originated keys already wake/reset that half locally before
	 * they are reported to the central.  Mirror only central-originated keys.
	 * The first key after idle is always mirrored immediately; while typing,
	 * a 10 s heartbeat is enough to keep the peripheral timer aligned. */
	if (ev->source == ZMK_POSITION_STATE_CHANGE_SOURCE_LOCAL) {
		int64_t now = k_uptime_get();
		if (was_off || last_activity_sync_ms == 0 ||
		    now - last_activity_sync_ms >= WS2812_ACTIVITY_SYNC_INTERVAL_MS) {
			last_activity_sync_ms = now;
			forward_activity_to_all_halves();
		}
	}
#endif

	return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(ws2812_key_activity_listener, key_activity_listener_cb);
ZMK_SUBSCRIPTION(ws2812_key_activity_listener, zmk_position_state_changed);

/* ZMK activity-state changes cover non-key activity (e.g. pointing input) and
normal sleep/wake transitions. Each half still owns its local idle timer. */
static int activity_listener_cb(const zmk_event_t *eh) {
	const struct zmk_activity_state_changed *ev = as_zmk_activity_state_changed(eh);

	if (ev == NULL) {
		return ZMK_EV_EVENT_BUBBLE;
	}

	if (ev->state == ZMK_ACTIVITY_ACTIVE) {
#if IS_ENABLED(CONFIG_ZMK_SPLIT) && IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
		bool was_off = idle_display_off;
#endif

		activity_active = true;
		ws2812_note_activity();

#if IS_ENABLED(CONFIG_ZMK_SPLIT) && IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
		int64_t now = k_uptime_get();
		if (was_off || last_activity_sync_ms == 0 ||
		    now - last_activity_sync_ms >= WS2812_ACTIVITY_SYNC_INTERVAL_MS) {
			last_activity_sync_ms = now;
			forward_activity_to_all_halves();
		}
#endif

		if (initialized && device_is_ready(led_strip) && static_lighting_needed()) {
			ws2812_lock_interrupting();
			enable_ext_power_if_needed();
			redraw_static_lighting_locked();
			ws2812_lighting_unlock();
		}
	} else if (ev->state == ZMK_ACTIVITY_SLEEP) {
		activity_active = false;
		ws2812_set_idle_display_local(true);
	}

	return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(ws2812_activity_listener, activity_listener_cb);
ZMK_SUBSCRIPTION(ws2812_activity_listener, zmk_activity_state_changed);

/* -------------------------------------------------------------------------
Worker threads/init
------------------------------------------------------------------------- */
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
	 * so an early GLOBAL lsync update has a valid target slot.
	 *
	 * Left half:
	 *   layer 3 = Russian NUMB, cyan
	 *   layer 7 = English SYMB, cyan
	 *   layer 8 = English NUMB, cyan only when latched
	 *
	 * Right half:
	 *   layer 2 = Russian SYMB, cyan
	 *   layer 7 = English SYMB, cyan
	 */
#if WS2812_HALF_IS_LEFT
	ws2812_set_persistent_layer_color(PERSISTENT_LAYER_LEFT,
					  PERSISTENT_LAYER_COLOR,
					  0, WS2812_NUM_PIXELS);
	ws2812_set_persistent_layer_color(PERSISTENT_LAYER_LEFT_ENG,
					  PERSISTENT_LAYER_COLOR,
					  0, WS2812_NUM_PIXELS);
	/* Layer 7 is the English symbol layer and is cyan on both halves. The
	 * central half therefore needs a local slot as well as peripheral sync. */
	ws2812_set_persistent_layer_color(PERSISTENT_LAYER_RIGHT_ENG,
					  PERSISTENT_LAYER_COLOR,
					  0, WS2812_NUM_PIXELS);

#if WS2812_HAS_LAYER_EVENTS
	update_eng_numb_cyan();
#endif
#endif

#if WS2812_HALF_IS_RIGHT
	ws2812_set_persistent_layer_color(PERSISTENT_LAYER_RIGHT,
					  PERSISTENT_LAYER_COLOR,
					  0, WS2812_NUM_PIXELS);
	ws2812_set_persistent_layer_color(PERSISTENT_LAYER_RIGHT_ENG,
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

	ws2812_idle_timer_init();
	ws2812_idle_timer_reset();
	ws2812_note_activity();

	k_mutex_lock(&ws2812_lighting_mutex, K_FOREVER);

	if (static_lighting_needed()) {
		sync_static_lighting_locked();
	} else {
		/* User config has underglow OFF on start; this guarantees the widget
		 * itself starts from a known black frame without changing ZMK state. */
		set_all_pixels((struct led_rgb){0, 0, 0});
	}

	ws2812_lighting_unlock();

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