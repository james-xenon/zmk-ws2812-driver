#define DT_DRV_COMPAT zmk_behavior_sleep_source

#include <zephyr/device.h>
#include <zephyr/init.h>
#include <zephyr/sys/poweroff.h>

#include <drivers/behavior.h>
#include <zmk/behavior.h>
#include <zmk/pm.h>

static int on_sleep_pressed(struct zmk_behavior_binding *binding,
                            struct zmk_behavior_binding_event event) {
    /* Ждём release, чтобы central успел получить отпускание клавиши. */
    return ZMK_BEHAVIOR_OPAQUE;
}

static int on_sleep_released(struct zmk_behavior_binding *binding,
                             struct zmk_behavior_binding_event event) {

    if (zmk_pm_suspend_devices() < 0) {
        zmk_pm_resume_devices();
        return ZMK_BEHAVIOR_OPAQUE;
    }

    sys_poweroff();

    return ZMK_BEHAVIOR_OPAQUE;
}

static const struct behavior_driver_api behavior_sleep_source_driver_api = {
    .locality = BEHAVIOR_LOCALITY_EVENT_SOURCE,
    .binding_pressed = on_sleep_pressed,
    .binding_released = on_sleep_released,
};

BEHAVIOR_DT_INST_DEFINE(0, NULL, NULL, NULL, NULL,
                        POST_KERNEL,
                        CONFIG_KERNEL_INIT_PRIORITY_DEFAULT,
                        &behavior_sleep_source_driver_api);