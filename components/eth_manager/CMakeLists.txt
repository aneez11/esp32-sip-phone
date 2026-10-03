# ENC28J60 SPI Ethernet manager. Thin wrapper over esp_eth + the
# espressif/enc28j60 registry driver that mirrors wifi_manager: it owns the
# netif, sets the shared event bits and exposes the interface IP.
idf_component_register(
    SRCS
        "eth_manager.c"
    INCLUDE_DIRS
        "."
    REQUIRES
        config_store
        esp_eth
        esp_netif
        esp_event
        esp_hw_support
        esp_driver_spi
        esp_driver_gpio
        lwip
)
