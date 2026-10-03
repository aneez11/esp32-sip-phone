#ifndef OTA_MANAGER_H
#define OTA_MANAGER_H

#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    OTA_STATE_IDLE = 0,
    OTA_STATE_RUNNING,
    OTA_STATE_SUCCESS,
    OTA_STATE_FAILED,
} ota_state_t;

// Current running firmware version (from the app descriptor) and the version
// the image was built with.
const char *ota_running_version(void);

// State of the most recent / in-progress update.
ota_state_t ota_state(void);
const char *ota_state_text(void);       // human-readable status line
int         ota_progress(void);          // 0..100 (-1 when unknown)
const char *ota_last_error(void);        // "" until a failure occurs

// True while an update is in progress; the web layer must not start a second
// one, and long-running work may use it to defer.
bool ota_in_progress(void);

// Download firmware from an http(s) URL and install it into the inactive OTA
// slot. Runs synchronously (call from a worker, not the HTTP handler). On
// success the device reboots; the new image is marked valid only after the
// caller calls ota_mark_valid() during a healthy boot (see below).
esp_err_t ota_update_from_url(const char *url);

// Begin / continue a web-uploaded image. Call ota_upload_begin() once, then
// ota_upload_write() per received chunk, then ota_upload_end(). A mismatch in
// total size or a write error aborts the update.
esp_err_t ota_upload_begin(size_t total_len);
esp_err_t ota_upload_write(const void *data, size_t len);
esp_err_t ota_upload_end(void);
void      ota_upload_abort(void);

// Confirm the running image after a boot so the bootloader will not roll it
// back. Safe to call on every boot; on non-OTA images and already-valid ones
// it is a no-op.
void ota_mark_valid(void);

// Schedule a reboot after a short delay so the HTTP response can be sent.
void ota_schedule_reboot(void);

#ifdef __cplusplus
}
#endif

#endif // OTA_MANAGER_H
