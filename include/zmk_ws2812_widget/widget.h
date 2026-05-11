#pragma once

#include <zephyr/kernel.h>

/**
 * @brief Queue a WS2812 battery indication.
 *
 * Manual indication shows the current battery status color.
 * Periodic automatic reminders are red and only run when the battery is at or
 * below CONFIG_WS2812_WIDGET_BATTERY_LEVEL_CRITICAL.
 */
void ws2812_indicate_battery(void);

/**
 * @brief Queue a WS2812 connectivity indication.
 *
 * This patched driver keeps the function for compatibility with existing
 * behavior bindings. Connectivity indication is intentionally minimal because
 * the current task is battery + permanent-layer indication.
 */
void ws2812_indicate_connectivity(void);

/**
 * @brief Queue a manual WS2812 layer indication.
 *
 * Manual layer indication uses CONFIG_WS2812_WIDGET_LAYER_COLOR_MANUAL.
 * Automatic layer indication uses white for layer ON and red for layer OFF.
 */
void ws2812_indicate_layer(void);
