/*
 * Copyright (c) 2024 The ZMK Contributors
 * SPDX-License-Identifier: MIT
 */

#define DT_DRV_COMPAT zmk_behavior_ws2812_widget

#include <zephyr/device.h>
#include <drivers/behavior.h>
#include <zephyr/logging/log.h>

#include <zmk/behavior.h>
#include <zmk_ws2812_widget/widget.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#if DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)

static int on_keymap_binding_pressed(struct zmk_behavior_binding *binding,
                                     struct zmk_behavior_binding_event event) {
    switch (binding->param1) {
    case 0:
        ws2812_toggle_indication_enabled();
        break;
    case 1:
        ws2812_set_indication_enabled(true);
        break;
    case 2:
        ws2812_set_indication_enabled(false);
        break;
    case 3:
        ws2812_indicate_battery();
        break;
    case 4:
        ws2812_indicate_connectivity();
        break;
    case 5:
        ws2812_indicate_battery_both();
        break;
    case 6:
        /* Set persistent layer color on BOTH halves via GLOBAL locality.
         * param1 = layer number. Color/range hardcoded to cyan left-half.
         * param1=0 clears all persistent layers. */
        if (binding->param2 == 0) {
            /* Clear: set layer 0 with black as sentinel */
            ws2812_set_persistent_layer_color(0, 0x000000, 0, 0);
        } else {
            ws2812_set_persistent_layer_color(binding->param2, 0x00FFFF, 0, 21);
        }
        break;
    default:
        LOG_WRN("Unknown ws2812_wdg param1: %d", binding->param1);
        return -ENOTSUP;
    }

    return 0;
}

static int on_keymap_binding_released(struct zmk_behavior_binding *binding,
                                      struct zmk_behavior_binding_event event) {
    return 0;
}

static const struct behavior_driver_api behavior_ws2812_widget_driver_api = {
    .binding_pressed = on_keymap_binding_pressed,
    .binding_released = on_keymap_binding_released,
    .locality = BEHAVIOR_LOCALITY_GLOBAL,
};

BEHAVIOR_DT_INST_DEFINE(0, NULL, NULL, NULL, NULL, POST_KERNEL,
                        CONFIG_KERNEL_INIT_PRIORITY_DEFAULT,
                        &behavior_ws2812_widget_driver_api);

#endif /* DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT) */