#pragma once

#include <stdbool.h>

/**
 * @brief Indicate current battery status with WS2812 LED colors/patterns
 */
void ws2812_indicate_battery(void);

/**
 * @brief Indicate current connectivity status with WS2812 LED colors/patterns
 */
void ws2812_indicate_connectivity(void);

/**
 * @brief Indicate current layer with WS2812 LED colors/patterns
 */
void ws2812_indicate_layer(void);

/**
 * @brief Toggle all automatic/manual WS2812 indication.
 *
 * This does not affect normal ZMK RGB underglow.
 */
void ws2812_toggle_indication_enabled(void);

/**
 * @brief Force enable/disable all WS2812 indication.
 *
 * This does not affect normal ZMK RGB underglow.
 */
void ws2812_set_indication_enabled(bool enabled);

/**
 * @brief Returns whether WS2812 indication is manually enabled.
 */
bool ws2812_get_indication_enabled(void);
