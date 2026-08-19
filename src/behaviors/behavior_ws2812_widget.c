#define DT_DRV_COMPAT zmk_behavior_ws2812_widget

#include <zephyr/device.h>
#include <zephyr/logging/log.h>
#include <drivers/behavior.h>
#include <zmk/behavior.h>
#include <zmk_ws2812_widget/widget.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

static int __maybe_unused behavior_ws2812_wdg_init(const struct device *dev) { return 0; }

static int on_keymap_binding_pressed(struct zmk_behavior_binding *binding,
                                     struct zmk_behavior_binding_event event) {
#if IS_ENABLED(CONFIG_WS2812_WIDGET)
    ws2812_note_activity();
    switch (binding->param1) {
    case 0:
#if IS_ENABLED(CONFIG_WS2812_WIDGET_SHOW_LAYER_CHANGE)
        ws2812_indicate_layer();
#endif
        break;
    case 1:
#if IS_ENABLED(CONFIG_ZMK_BATTERY_REPORTING) && IS_ENABLED(CONFIG_WS2812_WIDGET_SHOW_BATTERY)
        ws2812_indicate_battery();
#endif
        break;
    case 2:
        ws2812_toggle_indication_enabled();
        break;
    case 3:
        ws2812_set_indication_enabled(true);
        break;
    case 4:
        ws2812_set_indication_enabled(false);
        break;
    case 5:
#if IS_ENABLED(CONFIG_ZMK_BATTERY_REPORTING) && IS_ENABLED(CONFIG_WS2812_WIDGET_SHOW_BATTERY)
        ws2812_indicate_battery_both();
#endif
        break;
    case 6:
        /* Set persistent layer color on BOTH halves via GLOBAL locality.
         * param2 = layer number. Color/range hardcoded to cyan left-half.
         * param2=0 clears all persistent layers. */
        if (binding->param2 == 0) {
            ws2812_set_persistent_layer_color(0, 0x000000, 0, 0);
        } else {
            ws2812_set_persistent_layer_color(binding->param2, 0x00FFFF, 0, 21);
        }
        break;
    default:
        LOG_WRN("Unknown ws2812_wdg param1: %d", binding->param1);
        return -ENOTSUP;
    }
#endif
    return ZMK_BEHAVIOR_OPAQUE;
}

static int on_keymap_binding_released(struct zmk_behavior_binding *binding,
                                      struct zmk_behavior_binding_event event) {
    return ZMK_BEHAVIOR_OPAQUE;
}

static const struct behavior_driver_api __maybe_unused behavior_ws2812_wdg_driver_api = {
    .locality = BEHAVIOR_LOCALITY_GLOBAL,
    .binding_pressed = on_keymap_binding_pressed,
    .binding_released = on_keymap_binding_released,
#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_METADATA)
    .get_parameter_metadata = zmk_behavior_get_empty_param_metadata,
#endif
};

#define WS2812_WDG_INST(n) \
    BEHAVIOR_DT_INST_DEFINE(n, behavior_ws2812_wdg_init, NULL, NULL, NULL, POST_KERNEL, \
                            CONFIG_KERNEL_INIT_PRIORITY_DEFAULT, &behavior_ws2812_wdg_driver_api);

DT_INST_FOREACH_STATUS_OKAY(WS2812_WDG_INST)