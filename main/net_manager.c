#include "net_manager.h"
#include "wifi_manager.h"
#include "eth_manager.h"
#include "app_config.h"
#include "config_manager.h"
#include "esp_log.h"
#include "freertos/task.h"

static const char *TAG = "NET";

static bool s_eth_active = false;

// How long AUTO waits for a wired link before giving up and using Wi-Fi.
#define ETH_LINK_PROBE_MS 4000

void net_manager_start(EventGroupHandle_t group, EventBits_t connected_bit,
                       EventBits_t ip_bit, app_settings_t *settings) {
    s_eth_active = false;

    if (!settings) {
        wifi_init_sta(group, connected_bit, ip_bit, settings);
        return;
    }

    bool eth_only = (settings->network_mode == NETWORK_MODE_ETHERNET);
    bool want_eth = (settings->network_mode != NETWORK_MODE_WIFI);

#if defined(CONFIG_SIP_ETH_ENC28J60)
    if (want_eth) {
        hardware_settings_t hw;
        config_manager_load_hw(&hw);

        if (eth_manager_init(group, connected_bit, ip_bit, settings, &hw) == ESP_OK) {
            if (eth_only) {
                s_eth_active = true;
                ESP_LOGI(TAG, "Ethernet-only mode; Wi-Fi station disabled");
            } else {
                // AUTO: prefer wired, but only when a cable is actually present.
                // Wait on the event bit the driver sets on link-up instead of
                // polling, so we wake as soon as the link comes up.
                EventBits_t bits = xEventGroupWaitBits(group, connected_bit, pdFALSE,
                                                       pdFALSE, pdMS_TO_TICKS(ETH_LINK_PROBE_MS));
                if (bits & connected_bit) {
                    s_eth_active = true;
                    ESP_LOGI(TAG, "Ethernet link detected; not starting Wi-Fi STA");
                } else {
                    ESP_LOGW(TAG, "No Ethernet link; falling back to Wi-Fi");
                    eth_manager_stop();
                }
            }
        } else if (eth_only) {
            ESP_LOGE(TAG, "Ethernet-only mode but ENC28J60 init failed; device stays offline");
        }
    }
#else
    // The driver is compiled out: Ethernet-only can never work, so never let a
    // persisted NETWORK_MODE_ETHERNET strand the device without any interface.
    eth_only = false;
    (void)want_eth;
    (void)TAG;
#endif

    if (!eth_only && !s_eth_active) {
        wifi_init_sta(group, connected_bit, ip_bit, settings);
    }
}

esp_err_t net_get_ip(esp_ip4_addr_t *ip) {
    if (!ip) return ESP_ERR_INVALID_ARG;
#if defined(CONFIG_SIP_ETH_ENC28J60)
    if (s_eth_active && eth_manager_get_ip(ip) == ESP_OK) {
        return ESP_OK;
    }
#endif
    return get_my_ip(ip);
}

bool net_is_setup_ap(void) {
    return wifi_is_ap_mode();
}
