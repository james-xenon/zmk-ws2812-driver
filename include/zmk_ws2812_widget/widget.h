#pragma once

#include <stdbool.h>
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

// В конец файла zmk_ws2812_widget/widget.h добавить:

/**
 * @brief Установить постоянную подсветку для указанного слоя
 * @param layer Номер слоя
 * @param color_hex Цвет в формате 0xRRGGBB
 * @param start_pixel Начальный индекс пикселя
 * @param num_pixels Количество пикселей
 */
void ws2812_set_persistent_layer_color(uint8_t layer, uint32_t color_hex, 
                                       uint8_t start_pixel, uint8_t num_pixels);