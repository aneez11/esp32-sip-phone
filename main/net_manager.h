#ifndef NET_MANAGER_H
#define NET_MANAGER_H

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "esp_netif_types.h"
#include "esp_err.h"
#include "config_manager.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Bring up networking according to settings->network_mode:
//   WIFI      -> Wi-Fi station (and captive-portal AP on missing credentials).
//   ETHERNET  -> ENC28J60 only; the Wi-Fi station is never started.
//   AUTO      -> ENC28J60 when a cable is detected, otherwise Wi-Fi.
// Sets the same connected/ip event bits either way, so SIP and the UI need no
// knowledge of the underlying interface. On ESP32 family the internal EMAC and
// Wi-Fi share the same lwIP stack, so only one interface is brought up at once.
void net_manager_start(EventGroupHandle_t group, EventBits_t connected_bit,
                       EventBits_t ip_bit, app_settings_t *settings);

// IPv4 address of the active interface (Ethernet when it owns the link, else
// the Wi-Fi station/AP).
esp_err_t net_get_ip(esp_ip4_addr_t *ip);

// True while the captive-portal setup AP is active (Wi-Fi only).
bool net_is_setup_ap(void);

#ifdef __cplusplus
}
#endif

#endif // NET_MANAGER_H
