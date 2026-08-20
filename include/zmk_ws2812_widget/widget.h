#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <zephyr/kernel.h>

/**
 * @brief Mark keyboard activity for inactivity-based indicator suppression.
 */
void ws2812_note_activity(void);

/**
 * @brief Enable or disable all WS2812 widget indications.
 */
void ws2812_set_indication_enabled(bool enabled);

/**
 * @brief Toggle all WS2812 widget indications.
 */
void ws2812_toggle_indication_enabled(void);

/**
 * @brief Queue a WS2812 battery indication (local half only).
 */
void ws2812_indicate_battery(void);

/**
 * @brief Queue battery indication for both halves sequentially.
 *        Left half blinks first, then right half.
 *        On peripheral build — shows only local battery.
 */
void ws2812_indicate_battery_both(void);

/**
 * @brief Queue a WS2812 connectivity indication.
 */
void ws2812_indicate_connectivity(void);

/**
 * @brief Queue a manual WS2812 layer indication.
 */
void ws2812_indicate_layer(void);

/**
 * @brief Apply layer-sync color locally. Safe to call on both central and peripheral.
 */
void ws2812_apply_layer_sync(bool enabled);

/**
 * @brief Assign a persistent color to a layer on THIS half of the keyboard.
 *
 * @param layer       Layer number.
 * @param color_hex   0xRRGGBB.
 * @param start_pixel First LED index ON THIS HALF. Индексы всегда начинаются
 *                    с 0 на каждой половине — сквозной нумерации между
 *                    половинами не существует.
 * @param num_pixels  Number of LEDs to light, starting at start_pixel.
 */
void ws2812_set_persistent_layer_color(uint8_t layer, uint32_t color_hex,
                                       uint8_t start_pixel, uint8_t num_pixels);

/**
 * @brief Activate or deactivate a persistent layer by layer number.
 *        Uses GLOBAL locality via ws2812_lsync behavior so it works on peripheral too.
 */
void ws2812_set_persistent_layer_active(uint8_t layer, bool active);
