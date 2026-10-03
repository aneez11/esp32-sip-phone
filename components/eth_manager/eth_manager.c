#include "eth_manager.h"
#include "app_config.h"
#include "esp_log.h"

#if defined(CONFIG_SIP_ETH_ENC28J60)

#include "esp_eth.h"
#include "esp_eth_mac.h"
#include "esp_eth_phy.h"
#include "esp_eth_enc28j60.h"
#include "esp_event.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include <string.h>

static const char *TAG = "ETH";

static esp_eth_handle_t             s_eth          = NULL;
static esp_eth_netif_glue_handle_t  s_glue         = NULL;
static esp_netif_t                 *s_netif        = NULL;
static EventGroupHandle_t           s_group        = NULL;
static EventBits_t                  s_connected_bit = 0;
static EventBits_t                  s_ip_bit       = 0;
static bool                         s_link_up      = false;
static bool                         s_ip_up        = false;
static bool                         s_static_ip    = false;
static bool                         s_static_ip_ok = false;
static bool                         s_spi_bus_owned = false;

// A runtime pin wins over the compile-time fallback; -1 means "not wired".
static int pin_or(int8_t configured, int fallback) {
    return configured != -1 ? configured : fallback;
}

// Runs in the system event task: only touches state and signals the bits
// (never blocks or draws), matching the wifi_manager discipline.
static void eth_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data) {
    (void)arg; (void)base; (void)data;
    switch (id) {
    case ETHERNET_EVENT_START:
        ESP_LOGI(TAG, "ENC28J60 driver started");
        break;
    case ETHERNET_EVENT_CONNECTED:
        s_link_up = true;
        ESP_LOGI(TAG, "Link up (10 Mbps)");
        if (s_group) xEventGroupSetBits(s_group, s_connected_bit);
        if (s_static_ip_ok) {
            // No DHCP lease is coming; the interface is usable now.
            s_ip_up = true;
            if (s_group) xEventGroupSetBits(s_group, s_ip_bit);
            ESP_LOGI(TAG, "Static IP active (no DHCP client)");
        }
        break;
    case ETHERNET_EVENT_DISCONNECTED:
        s_link_up = false;
        s_ip_up = false;
        ESP_LOGW(TAG, "Link down");
        if (s_group) xEventGroupClearBits(s_group, s_connected_bit | s_ip_bit);
        break;
    case ETHERNET_EVENT_STOP:
        ESP_LOGI(TAG, "ENC28J60 driver stopped");
        break;
    default:
        break;
    }
}

static void eth_got_ip_handler(void *arg, esp_event_base_t base, int32_t id, void *data) {
    (void)arg; (void)base; (void)id;
    ip_event_got_ip_t *ev = (ip_event_got_ip_t *)data;
    s_ip_up = true;
    ESP_LOGI(TAG, "Ethernet IP: " IPSTR, IP2STR(&ev->ip_info.ip));
    if (s_group) xEventGroupSetBits(s_group, s_connected_bit | s_ip_bit);
}

bool eth_manager_has_link(void) {
    return s_link_up;
}

esp_err_t eth_manager_get_ip(esp_ip4_addr_t *ip) {
    if (!s_netif || !ip) return ESP_FAIL;
    esp_netif_ip_info_t info;
    if (esp_netif_get_ip_info(s_netif, &info) != ESP_OK) return ESP_FAIL;
    *ip = info.ip;
    return (info.ip.addr == 0) ? ESP_FAIL : ESP_OK;
}

void eth_manager_stop(void) {
    if (s_eth) {
        esp_eth_stop(s_eth);
    }
    esp_event_handler_unregister(ETH_EVENT, ESP_EVENT_ANY_ID, &eth_event_handler);
    esp_event_handler_unregister(IP_EVENT, IP_EVENT_ETH_GOT_IP, &eth_got_ip_handler);
    if (s_glue) {
        esp_eth_del_netif_glue(s_glue);
        s_glue = NULL;
    }
    if (s_eth) {
        esp_eth_driver_uninstall(s_eth);
        s_eth = NULL;
    }
    if (s_netif) {
        esp_netif_destroy(s_netif);
        s_netif = NULL;
    }
    if (s_spi_bus_owned) {
        spi_bus_free(ETH_SPI_HOST);
        s_spi_bus_owned = false;
    }
    s_link_up = false;
    s_ip_up = false;
    s_static_ip = false;
    s_static_ip_ok = false;
}

esp_err_t eth_manager_init(EventGroupHandle_t group,
                           EventBits_t connected_bit,
                           EventBits_t ip_bit,
                           const app_settings_t *settings,
                           const hardware_settings_t *hw) {
    if (!settings) return ESP_ERR_INVALID_ARG;
    if (s_eth) return ESP_OK; // already running

    int cs   = pin_or(hw ? hw->pin_eth_cs   : -1, ETH_PIN_CS);
    int intn = pin_or(hw ? hw->pin_eth_int  : -1, ETH_PIN_INT);
    int rst  = pin_or(hw ? hw->pin_eth_rst  : -1, ETH_PIN_RST);
    int sck  = pin_or(hw ? hw->pin_eth_sck  : -1, ETH_PIN_SCK);
    int miso = pin_or(hw ? hw->pin_eth_miso : -1, ETH_PIN_MISO);
    int mosi = pin_or(hw ? hw->pin_eth_mosi : -1, ETH_PIN_MOSI);

    if (cs < 0 || intn < 0 || sck < 0 || miso < 0 || mosi < 0) {
        // Quiet by default: AUTO mode probes Ethernet on every boot and a
        // Wi-Fi-only board legitimately has no pins configured.
        ESP_LOGD(TAG, "ENC28J60 not configured (CS/INT/SCK/MISO/MOSI all required); skipping");
        return ESP_ERR_INVALID_STATE;
    }

    s_group = group;
    s_connected_bit = connected_bit;
    s_ip_bit = ip_bit;
    s_static_ip = (settings->eth_dhcp == 0) && settings->eth_ip[0];

    // The ENC28J60 driver is interrupt driven: it needs the global GPIO ISR
    // service installed before it registers its INT handler.
    esp_err_t err = gpio_install_isr_service(0);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "gpio_install_isr_service: %s", esp_err_to_name(err));
        return err;
    }

    spi_bus_config_t buscfg = {
        .mosi_io_num = mosi,
        .miso_io_num = miso,
        .sclk_io_num = sck,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 1600,
    };
    err = spi_bus_initialize(ETH_SPI_HOST, &buscfg, SPI_DMA_CH_AUTO);
    if (err != ESP_OK) {
        if (err == ESP_ERR_INVALID_STATE) {
            // ESP32-C3 has only SPI2, which the TFT/touch already own; the
            // wired path cannot claim its own bus there.
            ESP_LOGE(TAG, "SPI host %d already in use (TFT/touch?). The ENC28J60 "
                          "needs a free SPI bus; on ESP32-C3 the wired path is unsupported.",
                     (int)ETH_SPI_HOST);
        } else {
            ESP_LOGE(TAG, "spi_bus_initialize(%d): %s", (int)ETH_SPI_HOST, esp_err_to_name(err));
        }
        return err;
    }
    s_spi_bus_owned = true;

    spi_device_interface_config_t spi_devcfg = {
        .mode = 0,
        .clock_speed_hz = ETH_SPI_CLOCK_MHZ * 1000 * 1000,
        .spics_io_num = cs,
        .queue_size = 20,
    };
    // Meet the ENC28J60 CS hold-time spec (the driver adds command/address bits).
    spi_devcfg.cs_ena_posttrans = enc28j60_cal_spi_cs_hold_time(ETH_SPI_CLOCK_MHZ);

    eth_enc28j60_config_t emac_cfg = ETH_ENC28J60_DEFAULT_CONFIG(ETH_SPI_HOST, &spi_devcfg);
    emac_cfg.int_gpio_num = intn;

    eth_mac_config_t mac_cfg = ETH_MAC_DEFAULT_CONFIG();
    esp_eth_mac_t *mac = esp_eth_mac_new_enc28j60(&emac_cfg, &mac_cfg);
    if (!mac) {
        ESP_LOGE(TAG, "ENC28J60 MAC init failed (check wiring and SPI clock)");
        eth_manager_stop();
        return ESP_FAIL;
    }

    eth_phy_config_t phy_cfg = ETH_PHY_DEFAULT_CONFIG();
    phy_cfg.phy_addr = 1;
    phy_cfg.reset_gpio_num = rst;
    esp_eth_phy_t *phy = esp_eth_phy_new_enc28j60(&phy_cfg);
    if (!phy) {
        ESP_LOGE(TAG, "ENC28J60 PHY init failed");
        mac->del(mac);
        eth_manager_stop();
        return ESP_FAIL;
    }

    esp_eth_config_t eth_cfg = ETH_DEFAULT_CONFIG(mac, phy);
    err = esp_eth_driver_install(&eth_cfg, &s_eth);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_eth_driver_install: %s", esp_err_to_name(err));
        mac->del(mac);
        phy->del(phy);
        eth_manager_stop();
        return err;
    }

    // Unique MAC: the real ETH eFuse MAC where present, otherwise a
    // locally-administered address derived from the base MAC (S3/C3 have no
    // internal EMAC, so ESP_MAC_ETH is not meaningful there).
    uint8_t mac_addr[6] = {0};
    if (esp_read_mac(mac_addr, ESP_MAC_ETH) != ESP_OK) {
        if (esp_read_mac(mac_addr, ESP_MAC_WIFI_STA) != ESP_OK) {
            const uint8_t fallback[6] = {0x02, 0x00, 0x00, 0x00, 0x00, 0x01};
            memcpy(mac_addr, fallback, sizeof(mac_addr));
        } else {
            mac_addr[0] = (uint8_t)((mac_addr[0] | 0x02) & 0xFE);
        }
    }
    esp_eth_ioctl(s_eth, ETH_CMD_S_MAC_ADDR, mac_addr);
    ESP_LOGI(TAG, "ENC28J60 MAC %02x:%02x:%02x:%02x:%02x:%02x",
             mac_addr[0], mac_addr[1], mac_addr[2], mac_addr[3], mac_addr[4], mac_addr[5]);

    esp_netif_config_t netif_cfg = ESP_NETIF_DEFAULT_ETH();
    s_netif = esp_netif_new(&netif_cfg);
    if (!s_netif) {
        ESP_LOGE(TAG, "esp_netif_new(ETH) failed");
        eth_manager_stop();
        return ESP_FAIL;
    }

    s_glue = esp_eth_new_netif_glue(s_eth);
    if (!s_glue || esp_netif_attach(s_netif, s_glue) != ESP_OK) {
        ESP_LOGE(TAG, "netif glue attach failed");
        eth_manager_stop();
        return ESP_FAIL;
    }

    esp_netif_set_hostname(s_netif,
                           settings->eth_hostname[0] ? settings->eth_hostname
                                                     : ETH_HOSTNAME_DEFAULT);

    if (s_static_ip) {
        uint32_t ip   = esp_ip4addr_aton(settings->eth_ip);
        uint32_t mask = esp_ip4addr_aton(settings->eth_netmask);
        uint32_t gw   = settings->eth_gw[0] ? esp_ip4addr_aton(settings->eth_gw) : 0;

        if (ip == 0 || mask == 0 || (settings->eth_gw[0] && gw == 0)) {
            // Reject a malformed address instead of applying 0.0.0.0 with DHCP
            // already off; fall back to DHCP so the interface stays usable.
            ESP_LOGE(TAG, "invalid static IP/netmask/gateway; using DHCP instead");
            s_static_ip = false;
        } else {
            esp_err_t derr = esp_netif_dhcpc_stop(s_netif);
            if (derr != ESP_OK && derr != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STOPPED) {
                ESP_LOGW(TAG, "dhcpc_stop: %s", esp_err_to_name(derr));
            }
            esp_netif_ip_info_t ip_info = {0};
            ip_info.ip.addr      = ip;
            ip_info.netmask.addr = mask;
            ip_info.gw.addr      = gw;
            err = esp_netif_set_ip_info(s_netif, &ip_info);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "esp_netif_set_ip_info failed (%s); using DHCP instead",
                         esp_err_to_name(err));
                s_static_ip = false;
                esp_netif_dhcpc_start(s_netif);
            } else {
                s_static_ip_ok = true;
                ESP_LOGI(TAG, "Static IP " IPSTR, IP2STR(&ip_info.ip));
            }
        }
        if (s_static_ip_ok && settings->eth_dns[0]) {
            esp_netif_dns_info_t dns = {0};
            dns.ip.type = ESP_IPADDR_TYPE_V4;
            dns.ip.u_addr.ip4.addr = esp_ip4addr_aton(settings->eth_dns);
            esp_netif_set_dns_info(s_netif, ESP_NETIF_DNS_MAIN, &dns);
        }
    }

    esp_event_handler_register(ETH_EVENT, ESP_EVENT_ANY_ID, &eth_event_handler, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP, &eth_got_ip_handler, NULL);

    err = esp_eth_start(s_eth);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_eth_start: %s", esp_err_to_name(err));
        eth_manager_stop();
        return err;
    }

    ESP_LOGI(TAG, "ENC28J60 up (SPI %d MHz, CS=%d INT=%d RST=%d)",
             ETH_SPI_CLOCK_MHZ, cs, intn, rst);
    return ESP_OK;
}

#else // !CONFIG_SIP_ETH_ENC28J60

// Feature compiled out: Wi-Fi only, identical behaviour to before.
esp_err_t eth_manager_init(EventGroupHandle_t group, EventBits_t connected_bit,
                           EventBits_t ip_bit, const app_settings_t *settings,
                           const hardware_settings_t *hw) {
    (void)group; (void)connected_bit; (void)ip_bit; (void)settings; (void)hw;
    return ESP_ERR_NOT_SUPPORTED;
}
bool      eth_manager_has_link(void)          { return false; }
esp_err_t eth_manager_get_ip(esp_ip4_addr_t *ip) { (void)ip; return ESP_FAIL; }
void      eth_manager_stop(void)              { }

#endif // CONFIG_SIP_ETH_ENC28J60
