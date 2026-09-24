if(CONFIG_WS2812_WIDGET)

target_sources(app PRIVATE
    src/widget.c
)

target_sources(app PRIVATE
    src/behaviors/behavior_ws2812_widget.c
)

target_sources(app PRIVATE
    src/behaviors/behavior_ws2812_layer_sync.c
)

target_sources(app PRIVATE
    src/rgb_idle_timer.c
    src/behaviors/behavior_rgb_timer.c
)

target_include_directories(app PRIVATE
    include
)

endif()