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
 * @brief Queue a WS2812 battery indication.
 */
void ws2812_indicate_battery(void);

/**
 * @brief Queue a WS2812 connectivity indication.
 */
void ws2812_indicate_connectivity(void);

/**
 * @brief Queue a manual WS2812 layer indication.
 */
void ws2812_indicate_layer(void);
