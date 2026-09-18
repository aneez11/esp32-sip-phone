#ifndef APP_CONFIG_H
#define APP_CONFIG_H

// Pull in the generated build config first, so CONFIG_IDF_TARGET_* (used by the
// target-specific GPIO pin map below) are defined regardless of the include
// order in the file that includes us. (Absent in the PC simulator.)
#ifdef ESP_PLATFORM
#include "sdkconfig.h"
#endif

// =====================================================================
//  ESP32-SIP-Voice — central configuration
//  (Defaults below are compiled-in fallbacks; the Captive Portal / Web UI
//   override Wi-Fi + SIP credentials and GPIO pins at runtime via NVS.)
// =====================================================================

// --- Firmware identity (shown in the web UI footer) ---
#define APP_VERSION            "v2.4.0"

// --- Wi-Fi Configuration ---
#define WIFI_SSID              "your_wifi_ssid"
#define WIFI_PASSWORD          "your_wifi_password"
#define WIFI_MAX_RETRY         5

// --- SIP Configuration ---
#define SIP_SERVER_IP          "192.168.1.100"
#define SIP_SERVER_PORT        5060
#define SIP_USER               "1000"
#define SIP_PASSWORD           "your_sip_password"
#define SIP_DISPLAY_NAME       "ESP32 Phone"
#define SIP_DOMAIN             ""         // realm/domain; empty = use SIP_SERVER_IP
#define SIP_LOCAL_PORT         5060
#define SIP_REGISTRATION_EXPIRY 3600
#define SIP_RETRY_INTERVAL_MS  5000
#define USE_SIPS               0          // 1 = SIP over TLS (experimental)
#define SIP_TARGET_URI         "sip:1001@192.168.1.100" // Default call target

// =====================================================================
//  Device role — how the phone behaves when somebody calls it.
//  Chosen at runtime on the web "Mode" card; values are stored in NVS.
// =====================================================================
#define DEVICE_ROLE_PHONE      0   // rings until a person answers (button / screen / web)
#define DEVICE_ROLE_SPEAKER    1   // auto-answers incoming calls: intercom, paging, doorbell

#ifdef CTRL_METHOD_AUTO
#  define DEVICE_ROLE_DEFAULT  DEVICE_ROLE_SPEAKER
#else
#  define DEVICE_ROLE_DEFAULT  DEVICE_ROLE_PHONE
#endif
#define AUTO_ANSWER_DELAY_DEFAULT 0    // seconds; 0 = pick up immediately
#define AUTO_ANSWER_DELAY_MAX     30

// --- AI & Voice Activation ---
#define USE_WAKE_WORD          0          // 4 MB board: wake word disabled to avoid the 8 MB model partition
// Keyword must match a WakeNet model flashed into the `model` partition and
// selectable via `idf.py menuconfig` (ESP Speech Recognition). Free options
// include "computer", "hiesp", "hilexin", "alexa".
#define WAKE_WORD_MODEL        "computer"

// =====================================================================
//  Audio codec selection (driven by the Kconfig hardware profile).
//  AUDIO_SAMPLE_RATE must be resolved BEFORE the frame-size math below,
//  so this block comes first.
// =====================================================================
#if defined(CONFIG_SIP_PROFILE_PRO)
    #define USE_CODEC_OPUS
    #define USE_FULL_DUPLEX_AEC
#elif defined(CONFIG_SIP_PROFILE_STANDARD)
    #define USE_CODEC_G722
#else
    // LITE profile -> G.711 only (8 kHz)
#endif

#if defined(USE_CODEC_OPUS)
    #define AUDIO_SAMPLE_RATE   48000      // OPUS full-band
    #define RTP_CLOCK_RATE      48000
    #define RTP_DYN_PAYLOAD_TYPE 96        // dynamic PT advertised for OPUS
#elif defined(USE_CODEC_G722)
    #define AUDIO_SAMPLE_RATE   16000      // G.722 wideband audio
    #define RTP_CLOCK_RATE      8000       // RFC 3551 quirk: G.722 RTP clock = 8 kHz
#else
    #define AUDIO_SAMPLE_RATE   8000       // G.711 narrowband
    #define RTP_CLOCK_RATE      8000
#endif

// G.711 variant used by the LITE path / fallback: 0 = PCMU (u-law), 8 = PCMA (a-law)
#define AUDIO_CODEC_PAYLOAD_TYPE 0

// --- Frame / RTP timing (derived) ---
#define AUDIO_FRAME_MS          20
#define AUDIO_SAMPLES_PER_FRAME (AUDIO_SAMPLE_RATE * AUDIO_FRAME_MS / 1000) // PCM samples / frame
#define RTP_TS_INCREMENT        (RTP_CLOCK_RATE * AUDIO_FRAME_MS / 1000)    // RTP timestamp step / frame

#define RTP_LOCAL_PORT_BASE     16384      // even base port for RTP
#define RTP_MAX_PAYLOAD         512        // worst-case encoded frame size
#define JITTER_BUFFER_SIZE      8          // SRAM fallback jitter depth (packets)

#define I2S_NUM                 I2S_NUM_0

// --- Hardware feature selection (uncomment one per group) ---
#define USE_KEYPAD_4X4_GPIO
//#define USE_KEYPAD_I2C_PCF8574
#define KEYPAD_I2C_ADDR 0x20
#define USE_DISPLAY_ST7789
//#define USE_DISPLAY_ILI9341
#define USE_TOUCH_XPT2046

// --- Default GPIO pin map (compile-time fallback; overridable at runtime via
//     the Web "HW Config" page). Pin numbers are TARGET-SPECIFIC because, e.g.,
//     GPIO22-25 do not exist on ESP32-S3 and only GPIO0-21 exist on ESP32-C3. ---
#if defined(CONFIG_IDF_TARGET_ESP32S3) || defined(__esp32s3__)
  #define I2S_BCK_PIN        5
  #define I2S_WS_PIN         6
  #define I2S_DATA_OUT_PIN   7
  #define I2S_DATA_IN_PIN    4
  #define CODEC_I2C_SCL_PIN  8
  #define CODEC_I2C_SDA_PIN  9
  #define KEYPAD_R1 10
  #define KEYPAD_R2 11
  #define KEYPAD_R3 12
  #define KEYPAD_R4 13
  #define KEYPAD_C1 14
  #define KEYPAD_C2 15
  #define KEYPAD_C3 16
  #define KEYPAD_C4 17
  #define KEYPAD_I2C_SDA 9
  #define KEYPAD_I2C_SCL 8
  #define TFT_MOSI 35
  #define TFT_SCLK 36
  #define TFT_CS   37
  #define TFT_DC   38
  #define TFT_RST  39
  #define TOUCH_CS  40
  #define TOUCH_IRQ 41
#elif defined(CONFIG_IDF_TARGET_ESP32C3) || defined(__esp32c3__)
  #define I2S_BCK_PIN        4
  #define I2S_WS_PIN         5
  #define I2S_DATA_OUT_PIN   6
  #define I2S_DATA_IN_PIN    7
  #define CODEC_I2C_SCL_PIN  8
  #define CODEC_I2C_SDA_PIN  9
  #define KEYPAD_R1 0
  #define KEYPAD_R2 1
  #define KEYPAD_R3 2
  #define KEYPAD_R4 3
  #define KEYPAD_C1 10
  #define KEYPAD_C2 11
  #define KEYPAD_C3 18
  #define KEYPAD_C4 19
  #define KEYPAD_I2C_SDA 9
  #define KEYPAD_I2C_SCL 8
  #define TFT_MOSI 6
  #define TFT_SCLK 7
  #define TFT_CS   10
  #define TFT_DC   20
  #define TFT_RST  21
  #define TOUCH_CS  1
  #define TOUCH_IRQ 0
#else // ESP32 (classic) — default WROOM/WROVER pin map
  #define I2S_BCK_PIN        26
  #define I2S_WS_PIN         25
  #define I2S_DATA_OUT_PIN   22
  #define I2S_DATA_IN_PIN    21
  #define CODEC_I2C_SCL_PIN  18
  #define CODEC_I2C_SDA_PIN  19
  #define KEYPAD_R1 32
  #define KEYPAD_R2 33
  #define KEYPAD_R3 27
  #define KEYPAD_R4 14
  #define KEYPAD_C1 12
  #define KEYPAD_C2 13
  #define KEYPAD_C3 4
  #define KEYPAD_C4 5
  #define KEYPAD_I2C_SDA 21
  #define KEYPAD_I2C_SCL 22
  #define TFT_MOSI 23
  #define TFT_SCLK 18
  #define TFT_CS   15
  #define TFT_DC   2
  #define TFT_RST  4
  #define TOUCH_CS  14
  #define TOUCH_IRQ 27
#endif

// --- Task configuration ---
#define WIFI_TASK_PRIORITY      5
#define SIP_TASK_PRIORITY       8
#define AUDIO_IO_TASK_PRIORITY  10
// The SIP task builds every outgoing message (INVITE+SDP, REGISTER, BYE), which
// needs ~3 KB of stack buffers on top of its 2 KB receive buffer.
#define SIP_TASK_STACK_SIZE     10240
#define AUDIO_TASK_STACK_SIZE   6144

// --- Audio codec hardware driver ---
#define USE_CODEC_ES8388
//#define USE_CODEC_INMP441_MAX98357A

// =====================================================================
//  Audio output hardware — chosen at runtime on the web Settings page.
//  A MAX98357A / PCM5102 style amp is a plain I2S DAC with no control bus,
//  an ES8388/ES8311 needs its registers configured over I2C. AUTO probes the
//  I2C codec only when I2C pins are configured and falls back to plain I2S.
// =====================================================================
// Playback volume (0-100). Stored in NVS and adjustable at runtime from the web
// UI; amps without a volume register (MAX98357A) are scaled in software.
#define AUDIO_VOLUME_DEFAULT 80
#define AUDIO_VOLUME_MAX     100
#define AUDIO_VOLUME_STEP    5

#define AUDIO_OUT_AUTO      0
#define AUDIO_OUT_I2S_AMP   1
#define AUDIO_OUT_ES8388    2

#if defined(USE_CODEC_INMP441_MAX98357A)
#  define AUDIO_OUT_DEFAULT AUDIO_OUT_I2S_AMP
#elif defined(USE_CODEC_ES8388)
#  define AUDIO_OUT_DEFAULT AUDIO_OUT_AUTO
#else
#  define AUDIO_OUT_DEFAULT AUDIO_OUT_AUTO
#endif

// --- NAT Traversal (STUN) ---
#define USE_STUN 1
#define STUN_SERVER_IP         "stun.l.google.com"
#define STUN_SERVER_PORT       19302

// --- Call Management Interface ---
#define CTRL_METHOD_BUTTONS
//#define CTRL_METHOD_WEB
//#define CTRL_METHOD_AUTO
#define BUTTON_GPIO            0

// --- Time / NTP (for the on-screen clock themes) ---
#define NTP_SERVER             "pool.ntp.org"
#define TIMEZONE               "GMT0"   // POSIX TZ, e.g. "MSK-3", "GMT0", "EST5EDT,M3.2.0,M11.1.0"

// --- Web interface login ---
// The web interface always starts with a login page ("/login"); every page
// behind it requires a valid session cookie. These are the factory defaults,
// they are overridable at runtime from the web "Settings" page (NVS).
#define WEB_UI_USER            "admin"
#define WEB_UI_PASSWORD        "esp32sip"
#define WEB_SESSION_TIMEOUT_S  28800    // 8 h session lifetime (sliding)

// --- Shared application event-group bits ---
// (Defined centrally so wifi_manager, sip_client and main agree on them.
//  Plain literals so this header has no FreeRTOS dependency and can be included
//  by low-level driver components.)
#ifndef WIFI_CONNECTED_BIT
#define WIFI_CONNECTED_BIT  (1 << 0)
#endif
#ifndef SIP_REGISTERED_BIT
#define SIP_REGISTERED_BIT  (1 << 1)
#endif
#ifndef IP_ACQUIRED_BIT
#define IP_ACQUIRED_BIT     (1 << 2)
#endif

#endif // APP_CONFIG_H
