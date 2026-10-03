#include "config_manager.h"
#include "app_config.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "CONFIG";
static const char *NVS_NAMESPACE = "sip_phone";     // Wi-Fi / SIP credentials
static const char *STORAGE_NAMESPACE = "hw_config"; // GPIO / hardware + UI theme

esp_err_t config_manager_init(void) {
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    return err;
}

// Fill every field with its compile-time fallback from app_config.h.
static void apply_defaults(app_settings_t *settings) {
    memset(settings, 0, sizeof(*settings));
    snprintf(settings->wifi_ssid, sizeof(settings->wifi_ssid), "%s", WIFI_SSID);
    snprintf(settings->wifi_password, sizeof(settings->wifi_password), "%s", WIFI_PASSWORD);
    snprintf(settings->sip_server, sizeof(settings->sip_server), "%s", SIP_SERVER_IP);
    snprintf(settings->sip_user, sizeof(settings->sip_user), "%s", SIP_USER);
    snprintf(settings->sip_password, sizeof(settings->sip_password), "%s", SIP_PASSWORD);
    settings->audio_out = AUDIO_OUT_DEFAULT;
    settings->volume = AUDIO_VOLUME_DEFAULT;
    settings->device_role = DEVICE_ROLE_DEFAULT;
    settings->auto_answer_delay_s = AUTO_ANSWER_DELAY_DEFAULT;
    settings->sip_port = SIP_SERVER_PORT;
    // SIP_DOMAIN is empty by default: an empty domain means "use the SIP server
    // as the realm/domain" (sip_client falls back to sip_server). Seeding a
    // literal here would override that fallback on devices that already have a
    // stored sip_server but no stored sip_domain.
    snprintf(settings->sip_domain, sizeof(settings->sip_domain), "%s", SIP_DOMAIN);
    snprintf(settings->sip_display_name, sizeof(settings->sip_display_name), "%s", SIP_DISPLAY_NAME);
    snprintf(settings->sip_target, sizeof(settings->sip_target), "%s", SIP_TARGET_URI);
    snprintf(settings->web_user, sizeof(settings->web_user), "%s", WEB_UI_USER);
    snprintf(settings->web_password, sizeof(settings->web_password), "%s", WEB_UI_PASSWORD);
    settings->network_mode = NETWORK_MODE_DEFAULT;
    settings->eth_dhcp = 1; // DHCP by default
    snprintf(settings->eth_hostname, sizeof(settings->eth_hostname), "%s", ETH_HOSTNAME_DEFAULT);
    settings->display_mode = DISPLAY_MODE_DEFAULT;
    settings->matrix_type = MATRIX_TYPE_NONE;
    settings->matrix_channels = MATRIX_COLOR_RGB;
    settings->matrix_layout = MATRIX_LAYOUT_2D;
    settings->matrix_brightness = MATRIX_BRIGHTNESS_DEFAULT;
    settings->matrix_modules = MATRIX_MODULES_DEFAULT;
    settings->matrix_width = 64;
    settings->matrix_height = 32;
}

// Read a string key; keep the (already applied) default when it is absent.
static void get_str_or_keep(nvs_handle_t h, const char *key, char *dst, size_t dst_size) {
    size_t len = dst_size;
    nvs_get_str(h, key, dst, &len);
}

esp_err_t config_manager_load(app_settings_t *settings) {
    apply_defaults(settings);

    nvs_handle_t my_handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &my_handle);
    if (err != ESP_OK) {
        ESP_LOGI(TAG, "NVS not found, using compile-time defaults");
        return ESP_OK; // Return OK with defaults
    }

    get_str_or_keep(my_handle, "wifi_ssid",  settings->wifi_ssid,   sizeof(settings->wifi_ssid));
    get_str_or_keep(my_handle, "wifi_pass",  settings->wifi_password, sizeof(settings->wifi_password));
    get_str_or_keep(my_handle, "sip_server", settings->sip_server,  sizeof(settings->sip_server));
    get_str_or_keep(my_handle, "sip_user",   settings->sip_user,    sizeof(settings->sip_user));
    get_str_or_keep(my_handle, "sip_pass",   settings->sip_password, sizeof(settings->sip_password));
    get_str_or_keep(my_handle, "sip_domain", settings->sip_domain,  sizeof(settings->sip_domain));
    get_str_or_keep(my_handle, "sip_name",   settings->sip_display_name, sizeof(settings->sip_display_name));
    get_str_or_keep(my_handle, "sip_target", settings->sip_target,  sizeof(settings->sip_target));
    get_str_or_keep(my_handle, "web_user",   settings->web_user,    sizeof(settings->web_user));
    get_str_or_keep(my_handle, "web_pass",   settings->web_password, sizeof(settings->web_password));
    get_str_or_keep(my_handle, "eth_ip",     settings->eth_ip,      sizeof(settings->eth_ip));
    get_str_or_keep(my_handle, "eth_mask",   settings->eth_netmask, sizeof(settings->eth_netmask));
    get_str_or_keep(my_handle, "eth_gw",     settings->eth_gw,      sizeof(settings->eth_gw));
    get_str_or_keep(my_handle, "eth_dns",    settings->eth_dns,     sizeof(settings->eth_dns));
    get_str_or_keep(my_handle, "eth_host",   settings->eth_hostname, sizeof(settings->eth_hostname));

    uint16_t port = 0;
    if (nvs_get_u16(my_handle, "sip_port", &port) == ESP_OK && port != 0) {
        settings->sip_port = port;
    }

    uint8_t audio_out = 0;
    if (nvs_get_u8(my_handle, "audio_out", &audio_out) == ESP_OK && audio_out <= AUDIO_OUT_ES8388) {
        settings->audio_out = audio_out;
    }
    uint8_t volume = 0;
    if (nvs_get_u8(my_handle, "volume", &volume) == ESP_OK && volume <= AUDIO_VOLUME_MAX) {
        settings->volume = volume;
    }
    uint8_t role = 0;
    if (nvs_get_u8(my_handle, "role", &role) == ESP_OK && role <= DEVICE_ROLE_SPEAKER) {
        settings->device_role = role;
    }
    uint8_t delay = 0;
    if (nvs_get_u8(my_handle, "auto_answer", &delay) == ESP_OK) {
        settings->auto_answer_delay_s = (delay > AUTO_ANSWER_DELAY_MAX) ? AUTO_ANSWER_DELAY_MAX : delay;
    }
    // Network interface (Wi-Fi / Ethernet) + static IPv4
    uint8_t net_mode = 0;
    if (nvs_get_u8(my_handle, "net_mode", &net_mode) == ESP_OK && net_mode <= NETWORK_MODE_ETHERNET) {
        settings->network_mode = net_mode;
    }
    uint8_t eth_dhcp = 0;
    if (nvs_get_u8(my_handle, "eth_dhcp", &eth_dhcp) == ESP_OK && eth_dhcp <= 1) {
        settings->eth_dhcp = eth_dhcp;
    }
    // LED matrix display
    uint8_t u8 = 0;
    if (nvs_get_u8(my_handle, "disp_mode", &u8) == ESP_OK && u8 <= DISPLAY_MODE_ALL)
        settings->display_mode = u8;
    if (nvs_get_u8(my_handle, "mx_type", &u8) == ESP_OK && u8 <= MATRIX_TYPE_PREVIEW)
        settings->matrix_type = u8;
    if (nvs_get_u8(my_handle, "mx_chan", &u8) == ESP_OK && u8 <= MATRIX_COLOR_RGB)
        settings->matrix_channels = u8;
    if (nvs_get_u8(my_handle, "mx_lay", &u8) == ESP_OK && u8 <= MATRIX_LAYOUT_LINE)
        settings->matrix_layout = u8;
    if (nvs_get_u8(my_handle, "mx_bright", &u8) == ESP_OK && u8 <= 100)
        settings->matrix_brightness = u8;
    if (nvs_get_u8(my_handle, "mx_mod", &u8) == ESP_OK && u8 > 0 && u8 <= 8)
        settings->matrix_modules = u8;
    uint16_t u16 = 0;
    if (nvs_get_u16(my_handle, "mx_w", &u16) == ESP_OK && u16 > 0)
        settings->matrix_width = u16;
    if (nvs_get_u16(my_handle, "mx_h", &u16) == ESP_OK && u16 > 0)
        settings->matrix_height = u16;

    nvs_close(my_handle);
    ESP_LOGI(TAG, "Settings loaded from NVS");
    return ESP_OK;
}

esp_err_t config_manager_save(const app_settings_t *settings) {
    nvs_handle_t my_handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &my_handle);
    if (err != ESP_OK) return err;

    nvs_set_str(my_handle, "wifi_ssid", settings->wifi_ssid);
    nvs_set_str(my_handle, "wifi_pass", settings->wifi_password);
    nvs_set_str(my_handle, "sip_server", settings->sip_server);
    nvs_set_str(my_handle, "sip_user", settings->sip_user);
    nvs_set_str(my_handle, "sip_pass", settings->sip_password);
    nvs_set_str(my_handle, "sip_domain", settings->sip_domain);
    nvs_set_str(my_handle, "sip_name", settings->sip_display_name);
    nvs_set_str(my_handle, "sip_target", settings->sip_target);
    nvs_set_str(my_handle, "web_user", settings->web_user);
    nvs_set_str(my_handle, "web_pass", settings->web_password);
    nvs_set_str(my_handle, "eth_ip",   settings->eth_ip);
    nvs_set_str(my_handle, "eth_mask", settings->eth_netmask);
    nvs_set_str(my_handle, "eth_gw",   settings->eth_gw);
    nvs_set_str(my_handle, "eth_dns",  settings->eth_dns);
    nvs_set_str(my_handle, "eth_host", settings->eth_hostname);
    nvs_set_u8(my_handle, "net_mode", settings->network_mode);
    nvs_set_u8(my_handle, "eth_dhcp", settings->eth_dhcp);
    nvs_set_u16(my_handle, "sip_port", settings->sip_port);
    nvs_set_u8(my_handle, "audio_out", settings->audio_out);
    nvs_set_u8(my_handle, "volume", settings->volume);
    nvs_set_u8(my_handle, "role", settings->device_role);
    nvs_set_u8(my_handle, "auto_answer", settings->auto_answer_delay_s);
    nvs_set_u8(my_handle, "disp_mode", settings->display_mode);
    nvs_set_u8(my_handle, "mx_type", settings->matrix_type);
    nvs_set_u8(my_handle, "mx_chan", settings->matrix_channels);
    nvs_set_u8(my_handle, "mx_lay", settings->matrix_layout);
    nvs_set_u8(my_handle, "mx_bright", settings->matrix_brightness);
    nvs_set_u8(my_handle, "mx_mod", settings->matrix_modules);
    nvs_set_u16(my_handle, "mx_w", settings->matrix_width);
    nvs_set_u16(my_handle, "mx_h", settings->matrix_height);

    err = nvs_commit(my_handle);
    nvs_close(my_handle);
    ESP_LOGI(TAG, "Settings saved to NVS");
    return err;
}

esp_err_t config_manager_load_hw(hardware_settings_t *hw_settings) {
    nvs_handle_t my_handle;
    esp_err_t err;

    // Set defaults (fallback to app_config.h values if possible, or -1 if unassigned)
    hw_settings->pin_i2s_bck = -1;
    hw_settings->pin_i2s_ws = -1;
    hw_settings->pin_i2s_dout = -1;
    hw_settings->pin_i2s_din = -1;
    hw_settings->pin_i2s_mclk = -1;
    hw_settings->pin_i2c_sda = -1;
    hw_settings->pin_i2c_scl = -1;
    hw_settings->pin_spi_mosi = -1;
    hw_settings->pin_spi_miso = -1;
    hw_settings->pin_spi_clk = -1;
    hw_settings->pin_tft_cs = -1;
    hw_settings->pin_tft_dc = -1;
    hw_settings->pin_tft_rst = -1;
    hw_settings->pin_touch_cs = -1;
    hw_settings->pin_touch_irq = -1;
    // LED matrix — fall back to the target-specific app_config.h defaults.
    hw_settings->pin_mx_r1 = MX_HUB75_R1;
    hw_settings->pin_mx_g1 = MX_HUB75_G1;
    hw_settings->pin_mx_b1 = MX_HUB75_B1;
    hw_settings->pin_mx_r2 = MX_HUB75_R2;
    hw_settings->pin_mx_g2 = MX_HUB75_G2;
    hw_settings->pin_mx_b2 = MX_HUB75_B2;
    hw_settings->pin_mx_a = MX_HUB75_A;
    hw_settings->pin_mx_b = MX_HUB75_B;
    hw_settings->pin_mx_c = MX_HUB75_C;
    hw_settings->pin_mx_d = MX_HUB75_D;
    hw_settings->pin_mx_e = MX_HUB75_E;
    hw_settings->pin_mx_clk = MX_HUB75_CLK;
    hw_settings->pin_mx_lat = MX_HUB75_LAT;
    hw_settings->pin_mx_oe = MX_HUB75_OE;
    hw_settings->pin_mx_din = MX_MAX7219_DIN;
    hw_settings->pin_mx_mclk = MX_MAX7219_CLK;
    hw_settings->pin_mx_cs = MX_MAX7219_CS;
    // ENC28J60 SPI Ethernet — -1 = not wired (configure on the web).
    hw_settings->pin_eth_cs = ETH_PIN_CS;
    hw_settings->pin_eth_int = ETH_PIN_INT;
    hw_settings->pin_eth_rst = ETH_PIN_RST;
    hw_settings->pin_eth_sck = ETH_PIN_SCK;
    hw_settings->pin_eth_miso = ETH_PIN_MISO;
    hw_settings->pin_eth_mosi = ETH_PIN_MOSI;
    hw_settings->ui_theme = 0;

    err = nvs_open(STORAGE_NAMESPACE, NVS_READONLY, &my_handle);
    if (err != ESP_OK) {
        ESP_LOGI(TAG, "Hardware config not initialized yet in NVS. Using defaults.");
        return ESP_ERR_NVS_NOT_FOUND;
    }

    size_t required_size = sizeof(hardware_settings_t);
    err = nvs_get_blob(my_handle, "hw_settings", hw_settings, &required_size);
    if (err != ESP_OK || required_size != sizeof(hardware_settings_t)) {
        ESP_LOGI(TAG, "No valid HW settings in NVS. Using defaults.");
    } else {
        ESP_LOGI(TAG, "Hardware settings loaded from NVS successfully.");
    }

    nvs_close(my_handle);
    return ESP_OK;
}

esp_err_t config_manager_save_hw(const hardware_settings_t *hw_settings) {
    nvs_handle_t my_handle;
    esp_err_t err;

    err = nvs_open(STORAGE_NAMESPACE, NVS_READWRITE, &my_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error (%s) opening NVS handle!", esp_err_to_name(err));
        return err;
    }

    err = nvs_set_blob(my_handle, "hw_settings", hw_settings, sizeof(hardware_settings_t));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to save HW settings blob");
    }

    err = nvs_commit(my_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to commit HW settings");
    }

    nvs_close(my_handle);
    ESP_LOGI(TAG, "Hardware settings saved to NVS successfully.");
    return ESP_OK;
}

void config_manager_reset(void) {
    nvs_handle_t my_handle;
    // Wi-Fi / SIP / web-login / network settings.
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &my_handle) == ESP_OK) {
        nvs_erase_all(my_handle);
        nvs_commit(my_handle);
        nvs_close(my_handle);
    }
    // GPIO map, matrix pins and UI theme.
    if (nvs_open(STORAGE_NAMESPACE, NVS_READWRITE, &my_handle) == ESP_OK) {
        nvs_erase_all(my_handle);
        nvs_commit(my_handle);
        nvs_close(my_handle);
    }
    ESP_LOGI(TAG, "Factory reset complete (all settings cleared)");
}
