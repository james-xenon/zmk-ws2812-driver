#define DT_DRV_COMPAT zmk_behavior_sleep_all

#include <zephyr/device.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/poweroff.h>

#include <drivers/behavior.h>
#include <zmk/behavior.h>
#include <zmk/pm.h>

#if IS_ENABLED(CONFIG_ZMK_SPLIT) && IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
#include <zmk/split/central.h>
#endif

/*
 * SLEEP BOTH strategy:
 * 1. This behavior itself is CENTRAL-local.
 * 2. On release, central explicitly sends the already-working "slpsrc"
 *    RELEASE command to every split peripheral.
 * 3. Central waits long enough for the BLE command to be delivered and for
 *    its own matrix/kscan callback to finish and re-arm wake GPIOs.
 * 4. Only then central enters the same deep power-off path.
 */

static int enter_deep_sleep(void) {
    int err = zmk_pm_suspend_devices();
    if (err < 0) {
        zmk_pm_resume_devices();
        return err;
    }

    sys_poweroff();
    return 0;
}

static void central_sleep_work_handler(struct k_work *work) {
    (void)work;
    (void)enter_deep_sleep();
}

K_WORK_DELAYABLE_DEFINE(central_sleep_work, central_sleep_work_handler);

static int on_sleep_all_pressed(struct zmk_behavior_binding *binding,
                                struct zmk_behavior_binding_event event) {
    (void)binding;
    (void)event;
    return ZMK_BEHAVIOR_OPAQUE;
}

static int on_sleep_all_released(struct zmk_behavior_binding *binding,
                                 struct zmk_behavior_binding_event event) {
    (void)binding;

#if IS_ENABLED(CONFIG_ZMK_SPLIT) && IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
    /*
     * sleep_source sleeps on RELEASE.  Sending only the RELEASE command is
     * intentional: it is one BLE command and executes the exact peripheral
     * sleep path that is already proven to work with &slpsrc.
     *
     * "slpsrc" is deliberately <= 8 chars for the split behavior protocol.
     */
    struct zmk_behavior_binding peripheral_sleep = {
        .behavior_dev = "slpsrc",
        .param1 = 0,
        .param2 = 0,
    };

    for (uint8_t source = 0; source < ZMK_SPLIT_CENTRAL_PERIPHERAL_COUNT; source++) {
        (void)zmk_split_central_invoke_behavior(source, &peripheral_sleep, event, false);
    }
#endif

    /*
     * Do NOT power central off inside this key-release callback.
     * Give BLE time to deliver the peripheral command and give kscan time
     * to restore the GPIO wake interrupt on the central half.
     */
    k_work_reschedule(&central_sleep_work, K_MSEC(750));

    return ZMK_BEHAVIOR_OPAQUE;
}

static const struct behavior_driver_api behavior_sleep_all_driver_api = {
    .locality = BEHAVIOR_LOCALITY_CENTRAL,
    .binding_pressed = on_sleep_all_pressed,
    .binding_released = on_sleep_all_released,
};

BEHAVIOR_DT_INST_DEFINE(0, NULL, NULL, NULL, NULL,
                        POST_KERNEL,
                        CONFIG_KERNEL_INIT_PRIORITY_DEFAULT,
                        &behavior_sleep_all_driver_api);
