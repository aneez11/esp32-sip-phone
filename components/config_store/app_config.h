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

// Minimum Wi-Fi auth mode the STA will associate with. The production default
// (WPA2) is overridden to WIFI_AUTH_OPEN by the Wokwi simulation build, because
// the simulator's virtual AP "Wokwi-GUEST" is an open network.
#ifndef WIFI_MIN_AUTHMODE
#define WIFI_MIN_AUTHMODE      WIFI_AUTH_WPA2_PSK
#endif

// =====================================================================
//  Network interface selection (runtime setting, stored in NVS, set on the
//  web Hardware -> Network tab).
//    AUTO      prefer wired Ethernet when a link is present, else Wi-Fi.
//    WIFI      Wi-Fi only (the ENC28J60 path is not started).
//    ETHERNET  wired only; the Wi-Fi station is never started.
// =====================================================================
#define NETWORK_MODE_AUTO      0
#define NETWORK_MODE_WIFI      1
#define NETWORK_MODE_ETHERNET  2
#define NETWORK_MODE_DEFAULT   NETWORK_MODE_AUTO

// =====================================================================
//  ENC28J60 SPI Ethernet (optional; CONFIG_SIP_ETH_ENC28J60).
//  Every pin defaults to -1 = "not wired": set them on the web
//  Hardware -> Network tab (like the other peripherals). INT is mandatory
//  because the driver is interrupt-driven; RST may be tied to VCC (-1).
//  SCK/MOSI/MISO use a dedicated SPI bus (SPI3 on ESP32/S3, SPI2 on C3).
//  The ESP32-S3 target overrides these with concrete pins below; other
//  targets keep -1 and are configured at runtime.
// =====================================================================
#ifndef ETH_PIN_CS
#define ETH_PIN_CS     (-1)
#endif
#ifndef ETH_PIN_INT
#define ETH_PIN_INT    (-1)
#endif
#ifndef ETH_PIN_RST
#define ETH_PIN_RST    (-1)
#endif
#ifndef ETH_PIN_SCK
#define ETH_PIN_SCK    (-1)
#endif
#ifndef ETH_PIN_MISO
#define ETH_PIN_MISO   (-1)
#endif
#ifndef ETH_PIN_MOSI
#define ETH_PIN_MOSI   (-1)
#endif

#if defined(CONFIG_IDF_TARGET_ESP32C3) || defined(__esp32c3__)
  #define ETH_SPI_HOST SPI2_HOST
#else
  #define ETH_SPI_HOST SPI3_HOST
#endif

#ifndef ETH_SPI_CLOCK_MHZ
#  if defined(CONFIG_SIP_ETH_SPI_CLOCK_MHZ)
#    define ETH_SPI_CLOCK_MHZ  CONFIG_SIP_ETH_SPI_CLOCK_MHZ
#  else
#    define ETH_SPI_CLOCK_MHZ  10
#  endif
#endif

// DHCP hostname advertised over Ethernet when none is configured.
#define ETH_HOSTNAME_DEFAULT   "esp32-sip"

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
  // --- PCM5102A I2S DAC (BCLK / DIN / LRCK) ---
  #define I2S_BCK_PIN        5
  #define I2S_WS_PIN         7
  #define I2S_DATA_OUT_PIN   6
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
  // On modules with Octal PSRAM (ESP32-S3-WROOM-1-N16R8 etc.) GPIO33-37 are
  // reserved for the PSRAM, so the display/touch defaults avoid them.
  #define TFT_MOSI 47
  #define TFT_SCLK 48
  #define TFT_CS   21
  #define TFT_DC   40
  #define TFT_RST  41
  #define TOUCH_CS  18
  #define TOUCH_IRQ 38
  // --- MAX7219 8x8 chain (the "8x8x4" ticker) ---
  #define MX_MAX7219_DIN  10
  #define MX_MAX7219_CLK  11
  #define MX_MAX7219_CS   12
  // --- ENC28J60 SPI Ethernet (own SPI bus, avoids TFT/touch pins) ---
  #undef ETH_PIN_SCK
  #undef ETH_PIN_MOSI
  #undef ETH_PIN_MISO
  #undef ETH_PIN_CS
  #undef ETH_PIN_INT
  #undef ETH_PIN_RST
  #define ETH_PIN_SCK    47
  #define ETH_PIN_MOSI   48
  #define ETH_PIN_MISO   21
  #define ETH_PIN_CS      3
  #define ETH_PIN_INT    14
  #define ETH_PIN_RST    45
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

// Optional SPI MISO for panels/touch controllers that drive it. The XPT2046
// touch controller needs MISO to return coordinates; most TFT panels do not.
// -1 = not wired (the Web "Hardware" page can override this at runtime).
#ifndef TFT_MISO
#define TFT_MISO (-1)
#endif

// =====================================================================
//  LED matrix display default pins (see docs/FEATURE-PLAN.md).
//  Compile-time fallbacks; the Web "Hardware -> Matrix" page overrides these
//  in NVS. HUB75 uses 13-14 pins; the S3 defaults re-use the (currently unused)
//  matrix-keypad GPIOs and free pins, avoiding I2S/I2C/TFT/USB/flash/strapping.
// =====================================================================
#if defined(CONFIG_IDF_TARGET_ESP32S3) || defined(__esp32s3__)
  #define MX_HUB75_R1   10
  #define MX_HUB75_G1   11
  #define MX_HUB75_B1   12
  #define MX_HUB75_R2   13
  #define MX_HUB75_G2   14
  #define MX_HUB75_B2   15
  #define MX_HUB75_A    16
  #define MX_HUB75_B    17
  #define MX_HUB75_C    18
  #define MX_HUB75_D    21
  #define MX_HUB75_E    (-1)   // 1/16 scan needs no E line
  #define MX_HUB75_CLK  1
  #define MX_HUB75_LAT  2
  #define MX_HUB75_OE   42
  // MAX7219 pins are defined per-target in the pin-map block above.
#else
  // Classic ESP32 / C3: no default HUB75 map (pins are tight and HUB75 shares
  // the I2S peripheral); configure at runtime or use the MAX7219 ticker.
  #define MX_HUB75_R1   (-1)
  #define MX_HUB75_G1   (-1)
  #define MX_HUB75_B1   (-1)
  #define MX_HUB75_R2   (-1)
  #define MX_HUB75_G2   (-1)
  #define MX_HUB75_B2   (-1)
  #define MX_HUB75_A    (-1)
  #define MX_HUB75_B    (-1)
  #define MX_HUB75_C    (-1)
  #define MX_HUB75_D    (-1)
  #define MX_HUB75_E    (-1)
  #define MX_HUB75_CLK  (-1)
  #define MX_HUB75_LAT  (-1)
  #define MX_HUB75_OE   (-1)
  #define MX_MAX7219_DIN  32
  #define MX_MAX7219_CLK  33
  #define MX_MAX7219_CS   27
#endif

// =====================================================================
//  Matrix display runtime defaults (stored in NVS, set from the web UI).
// =====================================================================
#define DISPLAY_MODE_OFF        0   // matrix disabled
#define DISPLAY_MODE_CALLER_ID  1   // show caller / room on call
#define DISPLAY_MODE_QUEUE      2   // show queue count
#define DISPLAY_MODE_ALERT      3   // alert + room until acknowledged
#define DISPLAY_MODE_ALL        4   // caller + queue + alert

#define MATRIX_TYPE_NONE     0
#define MATRIX_TYPE_HUB75    1
#define MATRIX_TYPE_MAX7219  2
#define MATRIX_TYPE_PREVIEW  3

#define MATRIX_COLOR_MONO_R  0
#define MATRIX_COLOR_MONO_G  1
#define MATRIX_COLOR_MONO_B  2
#define MATRIX_COLOR_RGB     3

#define MATRIX_LAYOUT_2D     0
#define MATRIX_LAYOUT_LINE   1

#define DISPLAY_MODE_DEFAULT    DISPLAY_MODE_CALLER_ID
#define MATRIX_BRIGHTNESS_DEFAULT 60
#define MATRIX_MODULES_DEFAULT    8

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
#define WEB_UI_PASSWORD        "infinityecho"
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

// =====================================================================
//  Wokwi simulation build overrides (CONFIG_SIP_WOKWI_SIM)
//  Enabled only by sdkconfig.wokwi.defaults; normal builds are untouched.
//  Wokwi emulates an ILI9341 panel + XPT2046 touch on an open Wokwi-GUEST
//  network, so those defaults are swapped here.
// =====================================================================
#if defined(CONFIG_SIP_WOKWI_SIM)
#undef  WIFI_SSID
#undef  WIFI_PASSWORD
#undef  WIFI_MIN_AUTHMODE
#define WIFI_SSID              "Wokwi-GUEST"
#define WIFI_PASSWORD          ""
#define WIFI_MIN_AUTHMODE      WIFI_AUTH_OPEN
#undef  WIFI_MAX_RETRY
#define WIFI_MAX_RETRY         10

#undef  USE_DISPLAY_ST7789
#undef  USE_DISPLAY_ILI9341
#undef  USE_DISPLAY_GC9A01
#define USE_DISPLAY_ILI9341    1

// XPT2046 MISO (GPIO19 is free on the classic ESP32 default map because the
// I2C codec bus is unused there).
#undef  TFT_MISO
#define TFT_MISO               19
#endif

#endif // APP_CONFIG_H
