#pragma once

#include <stdint.h>

/** @brief Show the configured battery indicator pattern. */
void ws2812_indicate_battery(void);

/** @brief Show a simple connectivity indicator pattern, if enabled. */
void ws2812_indicate_connectivity(void);

/** @brief Show the current layer as a temporary overlay indicator. */
void ws2812_indicate_layer(void);
