/*
 * Copyright (c) 2024 The ZMK Contributors
 * SPDX-License-Identifier: MIT
 */

#define DT_DRV_COMPAT zmk_behavior_ws2812_layer_sync

#include <zmk/behavior.h>
#include <zmk/keymap.h>
#include <zephyr/device.h>
#include <zephyr/logging/log.h>
#include <zmk_ws2812_widget/widget.h>
#include <drivers/behavior.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

static int on_keymap_binding_pressed(struct zmk_behavior_binding *binding,
                                     struct zmk_behavior_binding_event event) {
    // === НОВАЯ ФУНКЦИЯ: ПОСТОЯННАЯ ПОДСВЕТКА ТОЛЬКО СЛЕВА (21 диод) ===
    if (binding->param1 == 2) {
        ws2812_set_persistent_layer_color(2, 0x00FFFF, 0, 21); 
        return 0;
    }
    if (binding->param1 == 3) {
        ws2812_set_persistent_layer_color(2, 0x000000, 0, 21); 
        return 0;
    }
    // ==================================================================

    // Оригинальная логика драйвера (мигание на обеих половинах)
    bool enabled = (binding->param1 == 1);
    ws2812_apply_layer_sync(enabled);
    return 0;
}

static int on_keymap_binding_released(struct zmk_behavior_binding *binding,
                                      struct zmk_behavior_binding_event event) {
    return 0;
}

static int behavior_ws2812_layer_sync_init(const struct device *dev) {
    return 0;
}

// ИСПРАВЛЕНИЕ ДЛЯ ZMK v0.3.0: используем zmk_behavior_driver_api
static const struct zmk_behavior_driver_api behavior_ws2812_layer_sync_driver_api = {
    .binding_pressed = on_keymap_binding_pressed,
    .binding_released = on_keymap_binding_released,
};

DEVICE_DT_INST_DEFINE(0, behavior_ws2812_layer_sync_init, NULL, NULL, NULL,
                      APPLICATION, CONFIG_KERNEL_INIT_PRIORITY_DEFAULT,
                      &behavior_ws2812_layer_sync_driver_api);
