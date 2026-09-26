#define DT_DRV_COMPAT zmk_behavior_sleep_all

#include <zephyr/device.h>
#include <zephyr/init.h>
#include <zephyr/sys/poweroff.h>

#include <drivers/behavior.h>
#include <zmk/behavior.h>
#include <zmk/pm.h>

#define IS_SPLIT_PERIPHERAL \
    (IS_ENABLED(CONFIG_ZMK_SPLIT) && !IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL))

static int enter_deep_sleep(void) {
    int err = zmk_pm_suspend_devices();
    if (err < 0) {
        zmk_pm_resume_devices();
        return ZMK_BEHAVIOR_OPAQUE;
    }

    sys_poweroff();
    return ZMK_BEHAVIOR_OPAQUE;
}

static int on_sleep_all_pressed(struct zmk_behavior_binding *binding,
                                struct zmk_behavior_binding_event event) {
    /*
     * GLOBAL behavior is sent to the peripheral before being run locally.
     * Put the peripheral to sleep on PRESS so the command cannot be lost
     * when the central later powers down.
     *
     * Keep the SLEEP ALL key physically on the CENTRAL half.
     */
    if (IS_SPLIT_PERIPHERAL) {
        return enter_deep_sleep();
    }

    return ZMK_BEHAVIOR_OPAQUE;
}

static int on_sleep_all_released(struct zmk_behavior_binding *binding,
                                 struct zmk_behavior_binding_event event) {
    /* Central sleeps only after its local key has been released. */
    if (!IS_SPLIT_PERIPHERAL) {
        return enter_deep_sleep();
    }

    return ZMK_BEHAVIOR_OPAQUE;
}

static const struct behavior_driver_api behavior_sleep_all_driver_api = {
    .locality = BEHAVIOR_LOCALITY_GLOBAL,
    .binding_pressed = on_sleep_all_pressed,
    .binding_released = on_sleep_all_released,
};

BEHAVIOR_DT_INST_DEFINE(0, NULL, NULL, NULL, NULL,
                        POST_KERNEL,
                        CONFIG_KERNEL_INIT_PRIORITY_DEFAULT,
                        &behavior_sleep_all_driver_api);
