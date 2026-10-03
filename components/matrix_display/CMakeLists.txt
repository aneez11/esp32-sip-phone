set(MATRIX_SRCS
    "matrix_display.c"
    "matrix_backend.c"
    "font5x7.c"
    "backends/matrix_preview.c"
    "backends/matrix_max7219.c"
    "backends/matrix_hub75.cpp"
)

# The MAX7219 and HUB75 backends are compiled in so a runtime selection can
# switch between them; large HUB75 code is still gated by CONFIG_SIP_MATRIX_HUB75
# (the .cpp compiles to a null stub when the option is off).
idf_component_register(
    SRCS ${MATRIX_SRCS}
    INCLUDE_DIRS "."
    REQUIRES esp_driver_gpio config_store
    PRIV_REQUIRES esp_timer
)
