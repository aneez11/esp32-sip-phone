#include "ota_manager.h"
#include "app_config.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_https_ota.h"
#include "esp_app_desc.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

static const char *TAG = "OTA";

static ota_state_t s_state = OTA_STATE_IDLE;
static char        s_status[96] = "Idle";
static int         s_progress = -1;
static char        s_error[96] = "";

// Web-upload session
static const esp_partition_t *s_part = NULL;
static esp_ota_handle_t       s_handle = 0;
static size_t                 s_total = 0;
static size_t                 s_written = 0;
static bool                   s_active = false;

const char *ota_running_version(void) {
    const esp_app_desc_t *d = esp_app_get_description();
    return d ? d->version : "unknown";
}

ota_state_t ota_state(void)      { return s_state; }
const char *ota_state_text(void) { return s_status; }
int         ota_progress(void)   { return s_progress; }
const char *ota_last_error(void) { return s_error; }
bool ota_in_progress(void)       { return s_state == OTA_STATE_RUNNING; }

static void set_status(const char *msg) {
    snprintf(s_status, sizeof(s_status), "%s", msg);
    ESP_LOGI(TAG, "%s", msg);
}

static void set_error(const char *msg) {
    snprintf(s_error, sizeof(s_error), "%s", msg);
    snprintf(s_status, sizeof(s_status), "Failed: %s", msg);
    s_state = OTA_STATE_FAILED;
    s_progress = -1;
    ESP_LOGE(TAG, "%s", msg);
}

void ota_schedule_reboot(void) {
    // The caller has already queued the HTTP response; give it a moment to
    // flush before the device restarts.
    vTaskDelay(pdMS_TO_TICKS(1500));
    esp_restart();
}

void ota_mark_valid(void) {
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t st;
    if (esp_ota_get_state_partition(running, &st) == ESP_OK) {
        if (st == ESP_OTA_IMG_PENDING_VERIFY) {
            if (esp_ota_mark_app_valid_cancel_rollback() == ESP_OK) {
                ESP_LOGI(TAG, "New firmware confirmed; rollback cancelled");
            } else {
                ESP_LOGE(TAG, "mark_app_valid failed");
            }
        }
    }
}

// ---------------------------------------------------------------------
//  Network (URL) update
// ---------------------------------------------------------------------

static esp_err_t http_event_handler(esp_http_client_event_t *evt) {
    if (evt->event_id == HTTP_EVENT_ON_DATA) {
        // esp_https_ota reads progress internally; nothing to copy here.
    }
    return ESP_OK;
}

esp_err_t ota_update_from_url(const char *url) {
    if (!url || !url[0]) return ESP_ERR_INVALID_ARG;
    if (s_state == OTA_STATE_RUNNING) return ESP_ERR_INVALID_STATE;

    s_state = OTA_STATE_RUNNING;
    s_error[0] = '\0';
    s_progress = 0;
    set_status("Connecting to update server...");

    esp_http_client_config_t http = {
        .url = url,
        .timeout_ms = 20000,
        .keep_alive_enable = true,
        .event_handler = http_event_handler,
    };
    esp_https_ota_config_t ota_cfg = {
        .http_config = &http,
    };

    esp_https_ota_handle_t h = NULL;
    esp_err_t err = esp_https_ota_begin(&ota_cfg, &h);
    if (err != ESP_OK) {
        set_error("Cannot reach update server");
        return err;
    }

    esp_app_desc_t new_desc;
    if (esp_https_ota_get_img_desc(h, &new_desc) == ESP_OK) {
        ESP_LOGI(TAG, "Incoming firmware version: %s", new_desc.version);
    }

    while (1) {
        err = esp_https_ota_perform(h);
        if (err != ESP_ERR_HTTPS_OTA_IN_PROGRESS) break;
        int total = esp_https_ota_get_image_len_read(h);
        int size  = esp_https_ota_get_image_size(h);
        if (size > 0) {
            s_progress = (int)((int64_t)total * 100 / size);
            if (s_progress > 100) s_progress = 100;
        }
    }

    if (err != ESP_OK || !esp_https_ota_is_complete_data_received(h)) {
        esp_https_ota_abort(h);
        set_error("Download incomplete or corrupted");
        return ESP_FAIL;
    }

    err = esp_https_ota_finish(h);
    if (err != ESP_OK) {
        set_error((err == ESP_ERR_OTA_VALIDATE_FAILED) ? "Image validation failed"
                                                       : "Install failed");
        return err;
    }

    s_state = OTA_STATE_SUCCESS;
    s_progress = 100;
    set_status("Update installed; rebooting");
    ota_schedule_reboot();
    return ESP_OK; // unreachable
}

// ---------------------------------------------------------------------
//  Web upload (chunked)
// ---------------------------------------------------------------------

esp_err_t ota_upload_begin(size_t total_len) {
    if (s_active || s_state == OTA_STATE_RUNNING) return ESP_ERR_INVALID_STATE;

    s_part = esp_ota_get_next_update_partition(NULL);
    if (!s_part) {
        set_error("No OTA partition found (needs an OTA partition table)");
        return ESP_ERR_NOT_FOUND;
    }
    s_total = total_len;
    s_written = 0;
    s_error[0] = '\0';
    s_progress = 0;
    s_state = OTA_STATE_RUNNING;
    s_active = true;
    set_status("Receiving firmware...");

    esp_err_t err = esp_ota_begin(s_part, total_len, &s_handle);
    if (err != ESP_OK) {
        s_active = false;
        set_error("esp_ota_begin failed");
        return err;
    }
    ESP_LOGI(TAG, "Receiving %u bytes into %s", (unsigned)total_len, s_part->label);
    return ESP_OK;
}

esp_err_t ota_upload_write(const void *data, size_t len) {
    if (!s_active) return ESP_ERR_INVALID_STATE;
    esp_err_t err = esp_ota_write(s_handle, data, len);
    if (err != ESP_OK) {
        s_active = false;
        esp_ota_abort(s_handle);
        s_handle = 0;
        set_error("Flash write failed");
        return err;
    }
    s_written += len;
    if (s_total) {
        s_progress = (int)((int64_t)s_written * 100 / s_total);
        if (s_progress > 100) s_progress = 100;
    }
    return ESP_OK;
}

esp_err_t ota_upload_end(void) {
    if (!s_active) return ESP_ERR_INVALID_STATE;
    esp_err_t err = esp_ota_end(s_handle);
    s_handle = 0;
    s_active = false;
    if (err != ESP_OK) {
        set_error((err == ESP_ERR_OTA_VALIDATE_FAILED) ? "Image validation failed"
                                                       : "esp_ota_end failed");
        return err;
    }
    err = esp_ota_set_boot_partition(s_part);
    if (err != ESP_OK) {
        set_error("Failed to select new boot partition");
        return err;
    }
    s_state = OTA_STATE_SUCCESS;
    s_progress = 100;
    set_status("Update installed; rebooting");
    return ESP_OK;
}

void ota_upload_abort(void) {
    if (s_active) {
        esp_ota_abort(s_handle);
        s_handle = 0;
        s_active = false;
    }
    if (s_state == OTA_STATE_RUNNING) {
        s_state = OTA_STATE_FAILED;
        s_progress = -1;
        snprintf(s_status, sizeof(s_status), "Upload aborted");
    }
}
