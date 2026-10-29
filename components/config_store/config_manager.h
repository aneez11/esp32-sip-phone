#ifndef CONFIG_MANAGER_H
#define CONFIG_MANAGER_H

#include "esp_err.h"

typedef struct {
    char wifi_ssid[32];
    char wifi_password[64];
    // Wi-Fi addressing: 1 = DHCP (default), 0 = static
    uint8_t wifi_dhcp;
    char wifi_ip[16];
    char wifi_netmask[16];
    char wifi_gw[16];
    char wifi_dns[16];
    char sip_server[64];
    char sip_user[64];
    char sip_password[64];

    // Audio output hardware: AUDIO_OUT_AUTO / AUDIO_OUT_I2S_AMP / AUDIO_OUT_ES8388.
    uint8_t audio_out;
    uint8_t volume;                  // playback volume 0-100 (0 = mute)

    // Device role: DEVICE_ROLE_PHONE or DEVICE_ROLE_SPEAKER (auto-answer).
    uint8_t device_role;
    uint8_t auto_answer_delay_s;     // speaker mode only; 0 = answer immediately

    // Extended settings (managed from the web "Settings" page).
    uint16_t sip_port;               // 0 -> SIP_SERVER_PORT
    char sip_domain[64];             // empty -> sip_server
    char sip_display_name[32];       // empty -> SIP_DISPLAY_NAME
    char sip_target[64];             // default call target; empty -> SIP_TARGET_URI
    char web_user[32];               // web login username
    char web_password[64];           // web login password

    // Network interface: Wi-Fi / Ethernet selection and static IPv4 (ETH).
    uint8_t network_mode;            // NETWORK_MODE_*
    uint8_t eth_dhcp;                // 1 = DHCP, 0 = use the static address below
    char eth_ip[16];                 // static IPv4 address
    char eth_netmask[16];            // static netmask
    char eth_gw[16];                 // static gateway
    char eth_dns[16];                // static DNS server
    char eth_hostname[32];           // DHCP hostname

    // LED matrix display (caller ID / queue / alert). Values from app_config.h.
    uint8_t display_mode;            // DISPLAY_MODE_*
    uint8_t matrix_type;             // MATRIX_TYPE_*
    uint8_t matrix_channels;         // MATRIX_COLOR_*
    uint8_t matrix_layout;           // MATRIX_LAYOUT_*
    uint8_t matrix_brightness;       // 0-100
    uint8_t matrix_scan;             // 0=auto 1=1/4 2=1/8 3=1/16 4=1/32 (HUB only)
    uint8_t matrix_pixel_aspect;     // 0=1:1 1=2:1 wide 2=1:2 tall
    // Multi-line layout (up to 4 slots)
    uint8_t line_count;              // 1-4, capped by panel height
    uint8_t line_src[4];             // data source enum
    uint8_t line_scale[4];           // 0=auto, 1-4
    uint8_t line_span[4];            // 1-4 rows
    uint8_t line_align[4];           // 0=left,1=centre,2=right
    uint8_t line_scroll[4];          // bool
    // Screensaver
    uint8_t ss_enabled;              // bool
    uint8_t ss_mode;                 // 0=off,1=single,2=rotate,3=random
    uint8_t ss_mask;                 // bitmask of enabled effects
    char    ss_text[4][64];          // custom strings (scroll saver / Custom 1-4 slots)
    uint8_t ss_speed;                // 0=slow,1=normal,2=fast
    uint8_t ss_idle_s;               // 0=disabled
    uint8_t ss_bright;               // 0=follow main, else 0-100
    // Clock (idle screen + "Time"/"Date" line sources + Clock saver)
    uint8_t clock_mode;              // 0=off, 1=show clock instead of READY when idle
    uint8_t clock_12h;               // 0=24-hour, 1=12-hour
    uint8_t clock_date;              // 1=show the date under the time on tall panels
    int16_t clock_offset_min;        // minutes added to the system clock (-1440..1440)
    uint8_t matrix_modules;          // MAX7219 chain length
    uint16_t matrix_width;           // panel width (HUB75/preview)
    uint16_t matrix_height;          // panel height
} app_settings_t;

typedef struct {
    // I2S Audio Pins
    int8_t pin_i2s_bck;
    int8_t pin_i2s_ws;
    int8_t pin_i2s_dout;
    int8_t pin_i2s_din;
    int8_t pin_i2s_mclk; // For codecs that need MCLK (e.g. ES8388)

    // I2C Pins (For codec control or OLED)
    int8_t pin_i2c_sda;
    int8_t pin_i2c_scl;

    // SPI / TFT Pins
    int8_t pin_spi_mosi;
    int8_t pin_spi_miso;
    int8_t pin_spi_clk;
    int8_t pin_tft_cs;
    int8_t pin_tft_dc;
    int8_t pin_tft_rst;
    int8_t pin_touch_cs;
    int8_t pin_touch_irq;
    
    // LED matrix — HUB75 (13 used, E optional)
    int8_t pin_mx_r1;
    int8_t pin_mx_g1;
    int8_t pin_mx_b1;
    int8_t pin_mx_r2;
    int8_t pin_mx_g2;
    int8_t pin_mx_b2;
    int8_t pin_mx_a;
    int8_t pin_mx_b;
    int8_t pin_mx_c;
    int8_t pin_mx_d;
    int8_t pin_mx_e;
    int8_t pin_mx_clk;
    int8_t pin_mx_lat;
    int8_t pin_mx_oe;
    // LED matrix — MAX7219 8x8 chain
    int8_t pin_mx_din;
    int8_t pin_mx_mclk;
    int8_t pin_mx_cs;

    // ENC28J60 SPI Ethernet (-1 = not wired). INT is required.
    int8_t pin_eth_cs;
    int8_t pin_eth_int;
    int8_t pin_eth_rst;
    int8_t pin_eth_sck;
    int8_t pin_eth_miso;
    int8_t pin_eth_mosi;

    // UI Settings
    uint8_t ui_theme; // 0=Voice Assistant, 1=Mobile OS, 2=Smart Speaker
} hardware_settings_t;

esp_err_t config_manager_init(void);
esp_err_t config_manager_load(app_settings_t *settings);
esp_err_t config_manager_save(const app_settings_t *settings);

esp_err_t config_manager_load_hw(hardware_settings_t *hw_settings);
esp_err_t config_manager_save_hw(const hardware_settings_t *hw_settings);

void config_manager_reset(void); // Clear NVS

#endif // CONFIG_MANAGER_H
