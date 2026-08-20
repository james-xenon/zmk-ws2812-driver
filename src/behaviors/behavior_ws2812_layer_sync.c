#define DT_DRV_COMPAT zmk_behavior_ws2812_layer_sync

#include <zephyr/device.h>
#include <zephyr/logging/log.h>

#include <drivers/behavior.h>
#include <zmk/behavior.h>

#include <zmk_ws2812_widget/widget.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

static int __maybe_unused behavior_ws2812_lsync_init(const struct device *dev) {
    return 0;
}

/*
 * This behavior is marked GLOBAL locality, so ZMK automatically
 * runs it on the central AND every peripheral half of a split keyboard.
 *
 * param1 semantics:
 *   0         = legacy layer OFF blink (white->red fade)
 *   1         = legacy layer ON blink (white fade)
 *   2..255    = activate/deactivate persistent layer color for that layer number
 */
static int on_keymap_binding_pressed(struct zmk_behavior_binding *binding,
                                     struct zmk_behavior_binding_event event) {
    if (binding->param1 >= 2) {
        /* param1 is a persistent layer number */
        ws2812_set_persistent_layer_active(binding->param1, true);
    } else {
        /* Legacy behavior: 0 = off blink, 1 = on blink */
        ws2812_apply_layer_sync(binding->param1 != 0);
    }
    return ZMK_BEHAVIOR_OPAQUE;
}

static int on_keymap_binding_released(struct zmk_behavior_binding *binding,
                                      struct zmk_behavior_binding_event event) {
    if (binding->param1 >= 2) {
        /* Deactivate persistent layer on release */
        ws2812_set_persistent_layer_active(binding->param1, false);
    }
    /* Legacy: nothing on release (blink already finished) */
    return ZMK_BEHAVIOR_OPAQUE;
}

static const struct behavior_driver_api behavior_ws2812_lsync_driver_api = {
    .locality = BEHAVIOR_LOCALITY_GLOBAL,
    .binding_pressed = on_keymap_binding_pressed,
    .binding_released = on_keymap_binding_released,
#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_METADATA)
    .get_parameter_metadata = zmk_behavior_get_empty_param_metadata,
#endif
};

#define WS2812_LSYNC_INST(n)                                                                     \
    BEHAVIOR_DT_INST_DEFINE(n, behavior_ws2812_lsync_init, NULL, NULL, NULL, POST_KERNEL,        \
                            CONFIG_KERNEL_INIT_PRIORITY_DEFAULT, &behavior_ws2812_lsync_driver_api);

DT_INST_FOREACH_STATUS_OKAY(WS2812_LSYNC_INST)