/*
 * WS2812 temporary indicator widget for ZMK.
 * Driver-only version with Persistent Layer Color support.
 */

#include <zmk/behavior.h>
#include <zmk/behavior_queue.h>
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

#if IS_ENABLED(CONFIG_ZMK_EXT_POWER)
#include <drivers/ext_power.h>
#endif

/* ========================================================================
 * КАКАЯ ЭТО ПОЛОВИНА
 *
 * ВАЖНО: у сплита каждая половина — отдельный МК со своей лентой.
 * Прошивка собирается для каждой половины отдельно, и индексы пикселей
 * на каждой половине ВСЕГДА начинаются с нуля. Сквозной нумерации
 * "0..20 = левая, 21..41 = правая" НЕ СУЩЕСТВУЕТ.
 * Выбор половины делается ТОЛЬКО на этапе компиляции, вот здесь.
 *
 * В этом шилде (Kconfig.defconfig) central = ЛЕВАЯ половина.
 * ======================================================================== */
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

/* События слоёв существуют только на central (или на не-сплите).
 * Peripheral о слоях не знает вообще, ему состояние присылает central
 * через поведение ws2812_lsync (BEHAVIOR_LOCALITY_GLOBAL). */
#if !IS_ENABLED(CONFIG_ZMK_SPLIT) || IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
	#define WS2812_HAS_LAYER_EVENTS 1
	#include <zmk/events/layer_state_changed.h>
	#include <zmk/keymap.h>
#else
	#define WS2812_HAS_LAYER_EVENTS 0
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

/* ========================================================================
 * НАСТРОЙКА PERSISTENT-ЦВЕТОВ СЛОЁВ
 *
 * Слой светит на той половине, для которой он здесь объявлен.
 * Диапазон пикселей всегда 0..WS2812_NUM_PIXELS-1 — вся лента этой половины.
 * ======================================================================== */

/* Слой 3 (numb_layer) — светится ЛЕВАЯ половина */
#define PERSISTENT_LAYER_LEFT 3
/* Слой 2 (symb_layer) — светится ПРАВАЯ половина */
#define PERSISTENT_LAYER_RIGHT 2
/* Цвет */
#define PERSISTENT_LAYER_COLOR 0x00FFFF

/* Слой 19 (caps_indicator) — два диода на ЛЕВОЙ половине */
#define PERSISTENT_LAYER_CAPS       19
#define PERSISTENT_LAYER_CAPS_COLOR 0xFFFFFF
#define PERSISTENT_LAYER_CAPS_START 5
#define PERSISTENT_LAYER_CAPS_COUNT 2

/* Полный список persistent-слоёв. Central рассылает состояние
 * ИМЕННО этих слоёв на peripheral. Список обязан быть одинаковым
 * в обеих сборках, поэтому он объявлен вне #if по половинам. */
static const uint8_t __maybe_unused persistent_sync_layers[] = {
	PERSISTENT_LAYER_LEFT,
	PERSISTENT_LAYER_RIGHT,
	PERSISTENT_LAYER_CAPS,
};

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

static const struct device *const led_strip = DEVICE_DT_GET(WS2812_STRIP_NODE);
static struct led_rgb pixels[WS2812_NUM_PIXELS];

static bool initialized;
static bool widget_enabled = IS_ENABLED(CONFIG_WS2812_WIDGET_ENABLED_ON_START);
static bool activity_active = true;
static int64_t last_activity_ms;
static int64_t last_layer_indication_ms;

/* ========================================================================
 * PERSISTENT LAYER COLOR SUPPORT
 * ======================================================================== */

#define MAX_PERSISTENT_LAYERS 6

struct persistent_layer_config {
	uint8_t layer;
	struct led_rgb color;
	uint8_t start_pixel;
	uint8_t num_pixels;
	bool configured;
	bool active;
};

static struct persistent_layer_config persistent_layers[MAX_PERSISTENT_LAYERS];
static bool persistent_underglow_active = false;
static bool persistent_underglow_was_on = false;
static bool persistent_ext_power_was_on = true;

#if IS_ENABLED(CONFIG_ZMK_BATTERY_REPORTING) && IS_ENABLED(CONFIG_WS2812_WIDGET_SHOW_BATTERY) && \
	IS_ENABLED(CONFIG_ZMK_SPLIT) && IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL) && \
	IS_ENABLED(CONFIG_ZMK_SPLIT_BLE_CENTRAL_BATTERY_LEVEL_FETCHING)

	static uint8_t peripheral_battery_level = 0;

#endif

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

static int set_pixel_range(struct led_rgb color, uint8_t start, uint8_t count) {
	uint16_t end = MIN((uint16_t)start + count, WS2812_NUM_PIXELS);
	for (uint16_t i = start; i < end; i++) {
		pixels[i] = color;
	}
	return led_strip_update_rgb(led_strip, pixels, WS2812_NUM_PIXELS);
}

static void clear_pixel_range(uint8_t start, uint8_t count) {
	set_pixel_range((struct led_rgb){0, 0, 0}, start, count);
}

static void apply_persistent_layers(void) {
	bool any_active = false;
	for (int i = 0; i < MAX_PERSISTENT_LAYERS; i++) {
		if (persistent_layers[i].configured && persistent_layers[i].active) {
			set_pixel_range(persistent_layers[i].color,
				persistent_layers[i].start_pixel,
				persistent_layers[i].num_pixels);
			any_active = true;
		}
	}
	persistent_underglow_active = any_active;
}

static bool any_persistent_layer_active(void) {
	for (int i = 0; i < MAX_PERSISTENT_LAYERS; i++) {
		if (persistent_layers[i].configured && persistent_layers[i].active) return true;
	}
	return false;
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

/* ========================================================================
 * FORWARD DECLARATIONS for static helper functions used below
 * ======================================================================== */
static bool pause_underglow_if_needed(void);
static bool enable_ext_power_if_needed(void);
static void restore_underglow_if_needed(bool was_on);
static void restore_ext_power_if_needed(bool ext_power_was_on, bool underglow_was_on);
/* ========================================================================
 * END FORWARD DECLARATIONS
 * ======================================================================== */

/* Activate or deactivate a persistent layer by layer number.
 * Called from ws2812_lsync behavior (GLOBAL locality) so it works on peripheral.
 * Uses device_is_ready instead of initialized flag because on peripheral
 * the init thread may not have completed when lsync is first called. */
void ws2812_set_persistent_layer_active(uint8_t layer, bool active) {
	for (int i = 0; i < MAX_PERSISTENT_LAYERS; i++) {
		if (!persistent_layers[i].configured) continue;
		if (persistent_layers[i].layer != layer) continue;

		bool was_active = persistent_layers[i].active;
		bool any_before = any_persistent_layer_active();

		/* Состояние запоминаем всегда, даже если лента ещё не поднялась.
		 * Init-поток докрасит её в конце инициализации. */
		persistent_layers[i].active = active;

		if (!device_is_ready(led_strip)) return;
		if (active == was_active) return;

		if (active) {
			/* Сохраняем состояние underglow/ext-power только при переходе
			 * "ни одного слоя не активно" -> "есть активный". Иначе второй
			 * слой затрёт сохранённое значение нулём и underglow не вернётся. */
			if (!any_before) {
				persistent_underglow_was_on = pause_underglow_if_needed();
				persistent_ext_power_was_on = enable_ext_power_if_needed();
			}
			apply_persistent_layers();
		} else {
			clear_pixel_range(persistent_layers[i].start_pixel,
			                  persistent_layers[i].num_pixels);

			bool any_still_active = any_persistent_layer_active();
			persistent_underglow_active = any_still_active;

			if (!any_still_active) {
				restore_underglow_if_needed(persistent_underglow_was_on);
				restore_ext_power_if_needed(persistent_ext_power_was_on,
				                            persistent_underglow_was_on);
				persistent_underglow_was_on = false;
			}
		}
		return;
	}
}

/* ========================================================================
 * END PERSISTENT LAYER COLOR SUPPORT
 * ======================================================================== */

static struct led_rgb scale_rgb(struct led_rgb color, uint16_t numerator, uint16_t denominator) {
	if (denominator == 0) return color;
	return (struct led_rgb){
		.r = (uint8_t)(((uint16_t)color.r * numerator) / denominator),
		.g = (uint8_t)(((uint16_t)color.g * numerator) / denominator),
		.b = (uint8_t)(((uint16_t)color.b * numerator) / denominator),
	};
}

static uint16_t fade_step_count(uint16_t duration_ms) {
	if (duration_ms == 0) return 0;
	return MAX(1, duration_ms / CONFIG_WS2812_WIDGET_FADE_STEP_MS);
}

static void fade_from_black_to_color(struct led_rgb color, uint16_t duration_ms) {
	uint16_t steps = fade_step_count(duration_ms);
	if (steps == 0) { set_all_pixels(color); return; }
	uint16_t delay_ms = MAX(1, duration_ms / steps);
	for (uint16_t step = 0; step <= steps; step++) {
		set_all_pixels(scale_rgb(color, step, steps));
		k_sleep(K_MSEC(delay_ms));
	}
}

static void fade_from_color_to_black(struct led_rgb color, uint16_t duration_ms) {
	uint16_t steps = fade_step_count(duration_ms);
	if (steps == 0) { set_all_pixels((struct led_rgb){0,0,0}); return; }
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
	if (!initialized || !widget_enabled || !activity_active) return false;
	if (persistent_underglow_active) return false;
	if (periodic && !periodic_indication_allowed()) return false;
	return true;
}

void ws2812_note_activity(void) { last_activity_ms = k_uptime_get(); }

void ws2812_set_indication_enabled(bool enabled) {
	widget_enabled = enabled;
	ws2812_note_activity();
	if (!enabled && initialized) set_all_pixels((struct led_rgb){0, 0, 0});
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
		if (persistent_underglow_active) apply_persistent_layers();
		k_sleep(K_MSEC(CONFIG_WS2812_WIDGET_UNDERGLOW_RESTORE_DELAY_MS));
		zmk_rgb_underglow_on();
		return;
	}
#else
	ARG_UNUSED(was_on);
#endif
	if (persistent_underglow_active) apply_persistent_layers();
	else set_all_pixels((struct led_rgb){0, 0, 0});
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
	if (ext_power == NULL) { LOG_WRN("EXT_POWER device not found"); return true; }
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
	if (ext_power == NULL) return;
	if (!ext_power_was_on && !underglow_was_on &&
		IS_ENABLED(CONFIG_WS2812_WIDGET_RESTORE_EXT_POWER_OFF)) {
		ext_power_disable(ext_power);
	}
#else
	ARG_UNUSED(ext_power_was_on);
	ARG_UNUSED(underglow_was_on);
#endif
}

static void execute_indicator_request(const struct indicator_request *request) {
	if (request->kind == INDICATOR_KIND_SEPARATOR) {
		k_sleep(K_MSEC(request->hold_ms));
		return;
	}

	bool underglow_was_on = pause_underglow_if_needed();
	bool ext_power_was_on = enable_ext_power_if_needed();

	for (uint8_t i = 0; i < request->repeat_count; i++) {
		fade_from_black_to_color(request->color, request->fade_in_ms);
		if (request->hold_ms > 0) {
			set_all_pixels(request->color);
			k_sleep(K_MSEC(request->hold_ms));
		}
		fade_from_color_to_black(request->color, request->fade_out_ms);
		set_all_pixels((struct led_rgb){0,0,0});
		if (i + 1 < request->repeat_count && request->gap_ms > 0) {
			k_sleep(K_MSEC(request->gap_ms));
		}
	}

	restore_underglow_if_needed(underglow_was_on);
	restore_ext_power_if_needed(ext_power_was_on, underglow_was_on);
}

static void enqueue_indicator(struct indicator_request request, bool periodic) {
	if (!periodic) ws2812_note_activity();
	if (!indication_allowed(periodic)) return;
	int rc = k_msgq_put(&indicator_msgq, &request, K_NO_WAIT);
	if (rc != 0) LOG_WRN("WS2812 indicator queue full, dropping request kind %d", request.kind);
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

void ws2812_apply_layer_sync(bool enabled) {
#if IS_ENABLED(CONFIG_WS2812_WIDGET_SHOW_LAYER_CHANGE)
	last_layer_indication_ms = k_uptime_get();
	enqueue_indicator(make_layer_request(enabled), false);
#endif
}

/* ========================================================================
 * CENTRAL-ONLY: временная индикация смены слоя (мигание)
 * ======================================================================== */
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
	if (layer == zmk_keymap_layer_default()) return false;
	if (any_explicit_layer_triggers_configured()) return layer_is_explicit_trigger(layer);
	if (layer >= 32) return false;
	return (CONFIG_WS2812_WIDGET_LAYER_INDICATOR_MASK & BIT(layer)) != 0;
}

static struct k_work_delayable layer_indicator_work;
static bool pending_layer_state;
static bool pending_layer_valid;

static void layer_indicator_work_cb(struct k_work *work) {
	ARG_UNUSED(work);
	if (!pending_layer_valid) return;
	bool state = pending_layer_state;
	pending_layer_valid = false;

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
	if (!initialized || ev == NULL || !layer_should_trigger(ev->layer)) return 0;
	pending_layer_state = ev->state;
	pending_layer_valid = true;
	k_work_reschedule(&layer_indicator_work, K_MSEC(CONFIG_WS2812_WIDGET_LAYER_DEBOUNCE_MS));
	return 0;
}

ZMK_LISTENER(ws2812_layer_listener, layer_listener_cb);
ZMK_SUBSCRIPTION(ws2812_layer_listener, zmk_layer_state_changed);

#endif /* временная индикация смены слоя */

/* ========================================================================
 * CENTRAL-ONLY: persistent-слои
 * Слушаем события слоёв локально И рассылаем состояние на peripheral,
 * потому что peripheral сам о слоях ничего не знает.
 * ======================================================================== */
#if WS2812_HAS_LAYER_EVENTS

#if IS_ENABLED(CONFIG_ZMK_SPLIT) && IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)

#if !DT_NODE_EXISTS(DT_NODELABEL(ws2812_lsync))
#error "ws2812_lsync behavior node not found: add #include <behaviors/ws2812_layer_sync.dtsi> to the keymap"
#endif

static bool layer_needs_peripheral_sync(uint8_t layer) {
	for (size_t i = 0; i < ARRAY_SIZE(persistent_sync_layers); i++) {
		if (persistent_sync_layers[i] == layer) return true;
	}
	return false;
}

/* Отправляем состояние слоя на все peripheral-половины.
 * ws2812_lsync объявлено с BEHAVIOR_LOCALITY_GLOBAL, поэтому ZMK
 * прогоняет его локально и ретранслирует на каждую peripheral-половину.
 * param1 = номер слоя, param2 = 1 (включён) / 0 (выключен).
 * Состояние передаётся в параметре, а не через press/release, чтобы
 * потерянный BLE-пакет чинился следующим переключением слоя. */
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

#endif /* split central */

static int persistent_layer_listener_cb(const zmk_event_t *eh) {
	const struct zmk_layer_state_changed *ev = as_zmk_layer_state_changed(eh);
	if (ev == NULL) return 0;

#if IS_ENABLED(CONFIG_ZMK_SPLIT) && IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
	/* Рассылку делаем ДО проверки initialized и независимо от того,
	 * настроен ли этот слой локально: слой 2 светит на правой половине,
	 * и у левой сборки для него слота нет вообще. */
	if (layer_needs_peripheral_sync(ev->layer)) {
		forward_persistent_layer(ev->layer, ev->state);
	}
#endif

	if (!initialized) return 0;

	bool any_active_before = persistent_underglow_active;
	bool touched = false;

	for (int i = 0; i < MAX_PERSISTENT_LAYERS; i++) {
		if (!persistent_layers[i].configured) continue;

		if (persistent_layers[i].layer == ev->layer) {
			persistent_layers[i].active = ev->state;
			touched = true;
		}
	}

	if (!touched) return 0;

	bool any_active_now = any_persistent_layer_active();
	persistent_underglow_active = any_active_now;

	if (any_active_now && !any_active_before) {
		persistent_underglow_was_on = pause_underglow_if_needed();
		persistent_ext_power_was_on = enable_ext_power_if_needed();
		apply_persistent_layers();

	} else if (!any_active_now && any_active_before) {
		/* Очищаем ТОЛЬКО деактивированные persistent-слои,
		 * а не все сконфигурированные. Без этого underglow
		 * не восстанавливается корректно после сброса. */
		for (int i = 0; i < MAX_PERSISTENT_LAYERS; i++) {
			if (persistent_layers[i].configured && !persistent_layers[i].active) {
				clear_pixel_range(persistent_layers[i].start_pixel,
				                  persistent_layers[i].num_pixels);
			}
		}

#if IS_ENABLED(CONFIG_WS2812_WIDGET_PAUSE_RGB_UNDERGLOW) && IS_ENABLED(CONFIG_ZMK_RGB_UNDERGLOW)
		if (persistent_underglow_was_on) {
			restore_underglow_if_needed(true);
			persistent_underglow_was_on = false;
		} else {
			restore_underglow_if_needed(false);
		}
#else
		set_all_pixels((struct led_rgb){0, 0, 0});
#endif
		restore_ext_power_if_needed(persistent_ext_power_was_on, persistent_underglow_was_on);

	} else if (any_active_now && any_active_before) {
		apply_persistent_layers();
	}

	return 0;
}

ZMK_LISTENER(ws2812_persistent_layer_listener, persistent_layer_listener_cb);
ZMK_SUBSCRIPTION(ws2812_persistent_layer_listener, zmk_layer_state_changed);

#endif /* WS2812_HAS_LAYER_EVENTS */

/* ========================================================================
 * BATTERY
 * ======================================================================== */

#if IS_ENABLED(CONFIG_WS2812_WIDGET_SHOW_BATTERY)

static struct led_rgb get_battery_status_color(uint8_t battery_level) {
	if (battery_level == 0) return hex_to_rgb(CONFIG_WS2812_WIDGET_COLOR_OFF);
	if (battery_level <= CONFIG_WS2812_WIDGET_BATTERY_LEVEL_CRITICAL)
		return hex_to_rgb(CONFIG_WS2812_WIDGET_BATTERY_COLOR_CRITICAL);
	if (battery_level <= CONFIG_WS2812_WIDGET_BATTERY_LEVEL_LOW)
		return hex_to_rgb(CONFIG_WS2812_WIDGET_BATTERY_COLOR_LOW);
	if (battery_level >= CONFIG_WS2812_WIDGET_BATTERY_LEVEL_FULL)
		return hex_to_rgb(CONFIG_WS2812_WIDGET_BATTERY_COLOR_FULL);
	if (battery_level >= CONFIG_WS2812_WIDGET_BATTERY_LEVEL_HIGH)
		return hex_to_rgb(CONFIG_WS2812_WIDGET_BATTERY_COLOR_HIGH);
	return hex_to_rgb(CONFIG_WS2812_WIDGET_BATTERY_COLOR_MEDIUM);
}

static struct indicator_request make_battery_request(struct led_rgb color, uint8_t repeat_count,
	enum indicator_kind kind) {
	return (struct indicator_request){
		.kind = kind, .color = color,
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
		INDICATOR_KIND_BATTERY_MANUAL), false);
}

void ws2812_indicate_battery_both(void) {
#if IS_ENABLED(CONFIG_ZMK_SPLIT) && IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL) && \
	IS_ENABLED(CONFIG_ZMK_SPLIT_BLE_CENTRAL_BATTERY_LEVEL_FETCHING)

	uint8_t local_level = zmk_battery_state_of_charge();
	int retry = 0;
	while (local_level == 0 && retry++ < 10) {
		k_sleep(K_MSEC(100));
		local_level = zmk_battery_state_of_charge();
	}

	enqueue_indicator(make_battery_request(get_battery_status_color(local_level),
		CONFIG_WS2812_WIDGET_BATTERY_BOTH_LEFT_REPEAT,
		INDICATOR_KIND_BATTERY_MANUAL), false);

	struct indicator_request sep = {
		.kind = INDICATOR_KIND_SEPARATOR,
		.hold_ms = CONFIG_WS2812_WIDGET_BATTERY_BOTH_SEPARATOR_MS,
	};
	enqueue_indicator(sep, false);

	enqueue_indicator(make_battery_request(get_battery_status_color(peripheral_battery_level),
		CONFIG_WS2812_WIDGET_BATTERY_BOTH_RIGHT_REPEAT,
		INDICATOR_KIND_BATTERY_MANUAL), false);
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
#if IS_ENABLED(CONFIG_WS2812_WIDGET_BATTERY_REMINDER)
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
		should_show = should_show && battery_level <= CONFIG_WS2812_WIDGET_BATTERY_LEVEL_CRITICAL;
#endif

		if (should_show) {
			enqueue_indicator(make_battery_request(
				hex_to_rgb(CONFIG_WS2812_WIDGET_BATTERY_REMINDER_COLOR),
				CONFIG_WS2812_WIDGET_BATTERY_REMINDER_REPEAT_COUNT,
				INDICATOR_KIND_BATTERY_CRITICAL), true);
		}
	}
#endif
	schedule_next_battery_reminder();
}

#if IS_ENABLED(CONFIG_ZMK_BATTERY_REPORTING)
static int battery_listener_cb(const zmk_event_t *eh) {
	const struct zmk_battery_state_changed *ev = as_zmk_battery_state_changed(eh);
	if (!initialized || ev == NULL) return 0;

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
			INDICATOR_KIND_BATTERY_CRITICAL), false);
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
	if (ev == NULL) return 0;

	if (ev->state == ZMK_ACTIVITY_ACTIVE) {
		activity_active = true;
		ws2812_note_activity();
		if (persistent_underglow_active) {
			persistent_underglow_was_on = pause_underglow_if_needed();
			persistent_ext_power_was_on = enable_ext_power_if_needed();
			apply_persistent_layers();
		}
	} else if (ev->state == ZMK_ACTIVITY_SLEEP) {
		activity_active = false;
		set_all_pixels((struct led_rgb){0, 0, 0});
	}
	return 0;
}

ZMK_LISTENER(ws2812_activity_listener, activity_listener_cb);
ZMK_SUBSCRIPTION(ws2812_activity_listener, zmk_activity_state_changed);

static void indicator_process_thread(void *d0, void *d1, void *d2) {
	ARG_UNUSED(d0); ARG_UNUSED(d1); ARG_UNUSED(d2);

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
		if (CONFIG_WS2812_WIDGET_INTERVAL_MS > 0)
			k_sleep(K_MSEC(CONFIG_WS2812_WIDGET_INTERVAL_MS));
	}
}

K_THREAD_DEFINE(ws2812_indicator_process_tid, 1536, indicator_process_thread,
	NULL, NULL, NULL, K_LOWEST_APPLICATION_THREAD_PRIO, 0, 100);

static void indicator_init_thread(void *d0, void *d1, void *d2) {
	ARG_UNUSED(d0); ARG_UNUSED(d1); ARG_UNUSED(d2);

	if (!device_is_ready(led_strip)) {
		LOG_ERR("WS2812 LED strip device is not ready");
		return;
	}

	/* Настраиваем persistent-слои ДО initialized, чтобы lsync,
	 * прилетевший от central на старте, попал в уже готовый слот. */
#if WS2812_HALF_IS_LEFT
	/* ЛЕВАЯ половина: слой 3 (numb_layer) — вся её лента */
	ws2812_set_persistent_layer_color(PERSISTENT_LAYER_LEFT, PERSISTENT_LAYER_COLOR,
		0, WS2812_NUM_PIXELS);
	/* Слой 19 (caps_indicator) — диоды 5 и 6, белый */
	ws2812_set_persistent_layer_color(PERSISTENT_LAYER_CAPS, PERSISTENT_LAYER_CAPS_COLOR,
		PERSISTENT_LAYER_CAPS_START, PERSISTENT_LAYER_CAPS_COUNT);
#endif
#if WS2812_HALF_IS_RIGHT
	/* ПРАВАЯ половина: слой 2 (symb_layer) — вся её лента */
	ws2812_set_persistent_layer_color(PERSISTENT_LAYER_RIGHT, PERSISTENT_LAYER_COLOR,
		0, WS2812_NUM_PIXELS);
#endif

	initialized = true;
	ws2812_note_activity();

	if (any_persistent_layer_active()) {
		persistent_underglow_was_on = pause_underglow_if_needed();
		persistent_ext_power_was_on = enable_ext_power_if_needed();
		apply_persistent_layers();
	} else {
		set_all_pixels((struct led_rgb){0,0,0});
	}

	LOG_INF("WS2812 temporary indicator initialized with %d pixels (half: %s)",
		WS2812_NUM_PIXELS, WS2812_HALF_IS_LEFT ? "left/central" : "right/peripheral");

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
