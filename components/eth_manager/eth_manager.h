#ifndef ETH_MANAGER_H
#define ETH_MANAGER_H

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "esp_netif_types.h"
#include "esp_err.h"
#include "config_manager.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Bring up the ENC28J60 Ethernet interface. The netif is created here; DHCP or
// the stored static address is applied according to settings->eth_dhcp. The
// driver sets connected_bit on link-up and ip_bit once an address is acquired,
// so the rest of the firmware can wait on the same bits as Wi-Fi.
//
// Returns ESP_ERR_INVALID_STATE when the feature is compiled out or no usable
// pin map is configured (-1 pins), ESP_ERR_NOT_FOUND/ESP_FAIL on driver errors.
esp_err_t eth_manager_init(EventGroupHandle_t group,
                           EventBits_t connected_bit,
                           EventBits_t ip_bit,
                           const app_settings_t *settings,
                           const hardware_settings_t *hw);

// Link up (cable connected).
bool      eth_manager_has_link(void);

// Current IPv4 address of the Ethernet netif.
esp_err_t eth_manager_get_ip(esp_ip4_addr_t *ip);

// Stop the driver and release the SPI bus/netif. Used when AUTO falls back to
// Wi-Fi after no cable is detected. Safe to call when not initialised.
void      eth_manager_stop(void);

#ifdef __cplusplus
}
#endif

#endif // ETH_MANAGER_H
