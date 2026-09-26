if(CONFIG_WS2812_WIDGET)

target_sources(app PRIVATE
    src/widget.c
    src/behaviors/behavior_ws2812_widget.c
    src/behaviors/behavior_ws2812_layer_sync.c
    src/behaviors/behavior_ws2812_activity_sync.c
    src/behaviors/behavior_rgb_timer.c
    src/behavior_sleep_source.c
    src/behavior_sleep_all.c
)

target_include_directories(app PRIVATE
    include
)

endif()