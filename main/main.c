#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_sntp.h"
#include "nvs_flash.h"
#include <time.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>

#include "net_manager.h"
#include "sip_client.h"
#include "audio_pipeline.h"
#include "app_config.h" // Include configurations
#include "ui_controller.h" // Include UI Controller
#include "config_manager.h"
#include "display.h"
#include "phonebook.h"
#include "keypad.h"
#include "display_tft.h"
#include "touch_driver.h"
#include "ui_lvgl.h"
#include "matrix_display.h"
#include "driver/gpio.h"

static const char *TAG = "MAIN";

// Event group to signal application state.
// (WIFI_CONNECTED_BIT / SIP_REGISTERED_BIT / IP_ACQUIRED_BIT are defined
//  centrally in app_config.h so every module agrees on them.)
EventGroupHandle_t app_event_group;

// Shared handles (if needed across modules, though better passed as params)
sip_client_handle_t g_sip_client = NULL;
audio_pipeline_handle_t g_audio_pipeline = NULL;
matrix_display_handle_t g_matrix_display = NULL;
app_settings_t g_app_settings;

// Build a matrix config from the stored settings (NVS) + compile-time default
// pins. Returns false when the matrix is disabled (DISPLAY_MODE_OFF / no type).
static bool build_matrix_cfg(matrix_cfg_t *out) {
    if (!out || g_app_settings.display_mode == DISPLAY_MODE_OFF ||
        g_app_settings.matrix_type == MATRIX_TYPE_NONE) {
        return false;
    }
    hardware_settings_t hw;
    config_manager_load_hw(&hw);

    *out = (matrix_cfg_t){0};
    out->color = (matrix_color_t)g_app_settings.matrix_channels;
    out->brightness = g_app_settings.matrix_brightness;
    out->modules = g_app_settings.matrix_modules ? g_app_settings.matrix_modules : MATRIX_MODULES_DEFAULT;

    if (g_app_settings.matrix_type == MATRIX_TYPE_HUB75) {
        out->type = MX_HUB75;
        out->layout = MX_LAYOUT_2D;
        out->width = g_app_settings.matrix_width ? g_app_settings.matrix_width : 64;
        out->height = g_app_settings.matrix_height ? g_app_settings.matrix_height : 32;
        const int8_t pins[14] = {
            hw.pin_mx_r1, hw.pin_mx_g1, hw.pin_mx_b1, hw.pin_mx_r2, hw.pin_mx_g2,
            hw.pin_mx_b2, hw.pin_mx_a, hw.pin_mx_b, hw.pin_mx_c, hw.pin_mx_d,
            hw.pin_mx_e, hw.pin_mx_clk, hw.pin_mx_lat, hw.pin_mx_oe,
        };
        memcpy(out->pins, pins, sizeof(pins));
    } else if (g_app_settings.matrix_type == MATRIX_TYPE_MAX7219) {
        out->type = MX_MAX7219;
        out->layout = MX_LAYOUT_LINE;
        out->width = (uint16_t)(8 * out->modules);
        out->height = 8;
        out->pins[0] = hw.pin_mx_din;
        out->pins[1] = hw.pin_mx_mclk;
        out->pins[2] = hw.pin_mx_cs;
    } else { // MATRIX_TYPE_PREVIEW
        out->type = MX_PREVIEW;
        out->layout = MX_LAYOUT_2D;
        out->width = g_app_settings.matrix_width ? g_app_settings.matrix_width : 64;
        out->height = g_app_settings.matrix_height ? g_app_settings.matrix_height : 32;
    }
    return true;
}

// --- Application State Machine (Simplified Example) ---
typedef enum {
    APP_STATE_INIT,
    APP_STATE_WIFI_CONNECTING,
    APP_STATE_SIP_REGISTERING,
    APP_STATE_IDLE, // Registered, ready for calls
    APP_STATE_IN_CALL,
    APP_STATE_ERROR
} app_state_t;

volatile app_state_t current_app_state = APP_STATE_INIT;

void app_control_task(void *pvParameters) {
    ESP_LOGI(TAG, "Application control task started.");
    current_app_state = APP_STATE_WIFI_CONNECTING;

    while(1) {
        EventBits_t bits = xEventGroupWaitBits(app_event_group,
                                             WIFI_CONNECTED_BIT | SIP_REGISTERED_BIT,
                                             pdFALSE, // Don't clear on exit
                                             pdFALSE, // Wait for ANY bit
                                             pdMS_TO_TICKS(500)); // Poll every 500ms

        if ((bits & WIFI_CONNECTED_BIT) && current_app_state < APP_STATE_SIP_REGISTERING) {
             ESP_LOGI(TAG, "Wi-Fi Connected. Starting SIP Registration.");
             current_app_state = APP_STATE_SIP_REGISTERING;
             if (g_sip_client) sip_client_start_registration(g_sip_client);
        } else if (!(bits & WIFI_CONNECTED_BIT) && current_app_state >= APP_STATE_SIP_REGISTERING) {
             ESP_LOGW(TAG, "Wi-Fi Lost. Downgrading state.");
             current_app_state = APP_STATE_WIFI_CONNECTING;
             xEventGroupClearBits(app_event_group, SIP_REGISTERED_BIT);
        }

        if ((bits & SIP_REGISTERED_BIT) && current_app_state < APP_STATE_IDLE) {
            ESP_LOGI(TAG, "SIP Registered. Application Idle.");
            current_app_state = APP_STATE_IDLE;
        }

        // Add logic here to react to call state changes signaled FROM sip_client
        // e.g., if sip_client signals incoming call, update state, maybe ring a buzzer

        // xEventGroupWaitBits returns immediately as soon as one of the waited-for
        // bits is set, so the call above does not actually block once Wi-Fi is up.
        // Yield here, otherwise this task spins at priority 6 and starves the idle
        // task (task watchdog reset spam, and the web server starves too).
        vTaskDelay(pdMS_TO_TICKS(200));
    }
}


// ---- UI <-> SIP bridges ----
// (main owns both sides, so the LVGL UI component never depends on sip_client.)
static void ui_action_answer(void) { if (g_sip_client) sip_client_answer_call(g_sip_client); }
static void ui_action_hangup(void) { if (g_sip_client) sip_client_terminate_call(g_sip_client); }

static void on_incoming_call(const char *caller_uri, const char *call_id) {
    (void)call_id;
    ui_lvgl_switch_screen(SCREEN_INCOMING, caller_uri);
}
static void on_call_answered(const char *call_id) { (void)call_id; ui_lvgl_switch_screen(SCREEN_ACTIVE, NULL); }
static void on_call_ended(const char *call_id)    { (void)call_id; ui_lvgl_switch_screen(SCREEN_IDLE, NULL); }
static void on_registration_status(bool registered) { ui_lvgl_set_registered(registered); }

static void init_sntp(void) {
    sntp_setoperatingmode(SNTP_OPMODE_POLL);
    sntp_setservername(0, NTP_SERVER);
    sntp_init();
    setenv("TZ", TIMEZONE, 1);
    tzset();
    ESP_LOGI(TAG, "SNTP started (%s, TZ=%s)", NTP_SERVER, TIMEZONE);
}

// =====================================================================
//  Factory reset via the control button
//  Hold the control button (BUTTON_GPIO, active low) during boot to wipe
//  every stored setting back to the compile-time defaults: Wi-Fi, SIP
//  account, web login, network mode, GPIO map, matrix pins and theme.
//  A short tap does not reset, so the same button keeps working as the
//  call button at runtime.
// =====================================================================
#define FACTORY_RESET_HOLD_MS 3000

static bool factory_reset_requested(void) {
    gpio_config_t io = {
        .pin_bit_mask = (1ULL << BUTTON_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io);

    if (gpio_get_level(BUTTON_GPIO) != 0) return false; // not pressed

    ESP_LOGW(TAG, "Button held at boot; keep holding %d s to factory reset...",
             FACTORY_RESET_HOLD_MS / 1000);
    for (int waited = 0; waited < FACTORY_RESET_HOLD_MS; waited += 100) {
        vTaskDelay(pdMS_TO_TICKS(100));
        if (gpio_get_level(BUTTON_GPIO) != 0) {
            ESP_LOGI(TAG, "Button released early; normal boot");
            return false;
        }
    }
    return true;
}

void app_main(void) {
    ESP_LOGI(TAG, "Starting ESP32 SIP Client Application");

    // Initialize NVS Flash & Load Config
    config_manager_init();

    // Boot-time factory reset: holding the control button wipes all settings,
    // then the compile-time defaults are loaded below.
    if (factory_reset_requested()) {
        config_manager_reset();
        ESP_LOGW(TAG, "Factory reset: settings restored to defaults");
    }

    config_manager_load(&g_app_settings);
    
    // Initialize Phonebook Storage
    phonebook_init();
    
    // Initialize HAL Components
    keypad_init();
    display_tft_init();
    touch_driver_init();
    
    // Initialize Graphic UI
    ui_lvgl_init();
    
    // Initialize Display
    display_init();
    display_update_status("Starting...", "Init", "");

    // Initialize the LED matrix (caller ID / queue / alert). Off by default;
    // enabled per-device on the web Display tab.
    matrix_cfg_t mx_cfg;
    if (build_matrix_cfg(&mx_cfg)) {
        g_matrix_display = matrix_display_init(&mx_cfg);
        if (g_matrix_display) {
            // Startup test pattern so wiring can be verified at boot.
            matrix_draw_text_center(g_matrix_display, 4, "SIP VOICE", 2, 0x00A0FF);
            matrix_draw_text_center(g_matrix_display, 22, "READY", 1, 0x00FF00);
            matrix_flush(g_matrix_display);
        } else {
            ESP_LOGW(TAG, "Matrix init failed (type %d); continuing without it",
                     mx_cfg.type);
        }
    }

    // Create Event Group
    app_event_group = xEventGroupCreate();
    if (app_event_group == NULL) {
        ESP_LOGE(TAG, "Failed to create event group");
        return; // Or handle error appropriately
    }

    // Initialize TCP/IP stack and event loop
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    // Initialize networking: Wi-Fi and/or ENC28J60 Ethernet according to the
    // stored network mode (AUTO prefers a wired link). Starts the captive-
    // portal AP when Wi-Fi credentials are missing.
    net_manager_start(app_event_group, WIFI_CONNECTED_BIT, IP_ACQUIRED_BIT, &g_app_settings);

    if (net_is_setup_ap()) {
        ESP_LOGI(TAG, "Running in AP Mode (Captive Portal). SIP disabled.");
        display_update_status("192.168.4.1", "AP Setup Mode", "ESP-SIP-Setup");
        ui_controller_init(NULL, &g_app_settings);
        return; // Stop initialization here
    }

    esp_ip4_addr_t my_ip = {0};
    if (net_get_ip(&my_ip) != ESP_OK) {
        ESP_LOGW(TAG, "No IP address yet; SIP will register once one is acquired.");
    }
    char ip_str[16];
    snprintf(ip_str, sizeof(ip_str), IPSTR, IP2STR(&my_ip));
    display_update_status(ip_str, "Connecting SIP...", "");

    // Initialize Audio Pipeline (I2S, Codec)
    // This needs the actual codec driver implementation
    g_audio_pipeline = audio_pipeline_init();
    if (!g_audio_pipeline) {
        ESP_LOGE(TAG, "Failed to initialize audio pipeline");
        // Handle error - maybe cannot proceed
    }

    // Initialize SIP Client
    // Pass necessary handles/configs
    g_sip_client = sip_client_init(app_event_group, SIP_REGISTERED_BIT, g_audio_pipeline, &g_app_settings);
     if (!g_sip_client) {
        ESP_LOGE(TAG, "Failed to initialize SIP client");
        // Handle error
    } else {
        // Link audio pipeline back to SIP client if needed for RTP data
         if (g_audio_pipeline) {
            audio_pipeline_set_sip_handle(g_audio_pipeline, g_sip_client);
         }
    }

    // Initialize UI Controller
    if (g_sip_client) {
        ui_controller_init(g_sip_client, &g_app_settings);

        // Wire the SIP <-> LVGL UI bridges and the on-screen call buttons.
        sip_callbacks_t cbs = {
            .on_incoming_call       = on_incoming_call,
            .on_call_answered       = on_call_answered,
            .on_call_ended          = on_call_ended,
            .on_registration_status = on_registration_status,
        };
        sip_client_register_callbacks(g_sip_client, &cbs);
        ui_lvgl_set_action_cb(ui_action_answer, ui_action_hangup);
    }

    if (g_audio_pipeline) {
        audio_pipeline_set_wake_word_cb(g_audio_pipeline, ui_controller_wake_word_trigger);
    }

    // Start NTP so the clock themes show real time.
    init_sntp();

    // Create application control task (optional, but good for managing overall state)
    xTaskCreate(app_control_task, "app_ctrl", 4096, NULL, 6, NULL);


    ESP_LOGI(TAG, "Initialization Complete. Waiting for Wi-Fi connection...");

    // Tasks for WiFi events, SIP, Audio are created within their respective init functions usually.
    // The system now runs on FreeRTOS tasks. app_main finishes here.
}
