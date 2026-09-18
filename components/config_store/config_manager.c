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
    nvs_set_u16(my_handle, "sip_port", settings->sip_port);
    nvs_set_u8(my_handle, "audio_out", settings->audio_out);
    nvs_set_u8(my_handle, "volume", settings->volume);
    nvs_set_u8(my_handle, "role", settings->device_role);
    nvs_set_u8(my_handle, "auto_answer", settings->auto_answer_delay_s);

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
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &my_handle) == ESP_OK) {
        nvs_erase_all(my_handle);
        nvs_commit(my_handle);
        nvs_close(my_handle);
        ESP_LOGI(TAG, "NVS reset complete");
    }
}
