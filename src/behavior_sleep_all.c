#define DT_DRV_COMPAT zmk_behavior_sleep_all

#include <zephyr/device.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>

#include <drivers/behavior.h>
#include <zmk/behavior.h>
#include <zmk/pm.h>

#if IS_ENABLED(CONFIG_ZMK_SPLIT) && IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
#include <zmk/split/central.h>
#endif

/*
 * SLEEP BOTH strategy:
 * 1. On release, central explicitly sends the already-working "slpsrc"
 *    release command to every peripheral.
 * 2. Wait 750 ms so the BLE command is delivered and the peripheral powers off.
 * 3. Put CENTRAL into ZMK soft-off using zmk_pm_soft_off().
 *
 * In this project kscan0 is listed in soft_off_wakers, so zmk_pm_soft_off()
 * re-arms the normal keyboard matrix as a wake source. Therefore any key on
 * the central half can wake it; Reset is not required.
 */

static void central_sleep_work_handler(struct k_work *work) {
    (void)work;

#if IS_ENABLED(CONFIG_ZMK_PM_SOFT_OFF)
    /*
     * This path deliberately differs from direct sys_poweroff():
     * zmk_pm_soft_off() suspends devices and then explicitly resumes/enables
     * soft_off_wakers (kscan0 in our keymap) before entering system off.
     */
    (void)zmk_pm_soft_off();
#else
#error "behavior_sleep_all requires CONFIG_ZMK_PM_SOFT_OFF=y"
#endif
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
     * sleep_source sleeps a peripheral on RELEASE.
     * "slpsrc" is <= 8 chars, which is safe for the split behavior protocol.
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
     * Give the peripheral BLE command time to arrive before central shuts
     * its radio down. zmk_pm_soft_off() itself will prepare kscan0 for wake.
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
