#include "ui_controller.h"
#include "app_config.h"
#include "config_manager.h"
#include "wifi_manager.h"
#include "phonebook.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_http_server.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__has_include)
#  if __has_include("esp_random.h")
#    include "esp_random.h"
#  endif
#endif

static const char *TAG = "UI_CTRL";
static sip_client_handle_t g_sip_client = NULL;
static app_settings_t *g_settings = NULL;

// Owned by main.c; used to report the SIP registration state on the web page.
extern EventGroupHandle_t app_event_group;

// =====================================================================
//  Session handling (cookie based)
//  The web interface always starts at /login. A successful login issues an
//  ESPAUTH cookie holding a random 128-bit token; every other page validates
//  that cookie before it is served.
// =====================================================================

#define WEB_MAX_SESSIONS 4
#define WEB_TOKEN_LEN    32
#define COOKIE_NAME      "ESPAUTH"

typedef struct {
    char    token[WEB_TOKEN_LEN + 1];
    int64_t expires_us;
    bool    used;
} web_session_t;

static web_session_t s_sessions[WEB_MAX_SESSIONS];

// Login throttle: after a few failures the login endpoint is locked out for a
// while. Tracked with timestamps instead of sleeps so the single HTTP task is
// never blocked (an unauthenticated POST /login must not stall the server).
#define WEB_LOGIN_MAX_FAILURES 5
#define WEB_LOGIN_LOCKOUT_S    30
static int     s_login_fails = 0;
static int64_t s_login_blocked_until = 0;

static void session_prune(void) {
    int64_t now = esp_timer_get_time();
    for (int i = 0; i < WEB_MAX_SESSIONS; i++) {
        if (s_sessions[i].used && s_sessions[i].expires_us <= now) {
            s_sessions[i].used = false;
        }
    }
}

static const char *session_create(void) {
    session_prune();

    int slot = -1;
    for (int i = 0; i < WEB_MAX_SESSIONS; i++) {
        if (!s_sessions[i].used) { slot = i; break; }
    }
    if (slot < 0) { // all in use: recycle the one expiring first
        slot = 0;
        for (int i = 1; i < WEB_MAX_SESSIONS; i++) {
            if (s_sessions[i].expires_us < s_sessions[slot].expires_us) slot = i;
        }
    }

    uint8_t rnd[WEB_TOKEN_LEN / 2];
    esp_fill_random(rnd, sizeof(rnd));
    for (size_t i = 0; i < sizeof(rnd); i++) {
        snprintf(s_sessions[slot].token + i * 2, 3, "%02x", rnd[i]);
    }
    s_sessions[slot].expires_us = esp_timer_get_time() + (int64_t)WEB_SESSION_TIMEOUT_S * 1000000;
    s_sessions[slot].used = true;
    return s_sessions[slot].token;
}

// Extract the ESPAUTH value from the request's Cookie header.
static bool cookie_token(httpd_req_t *req, char *out, size_t out_len) {
    if (!out || out_len == 0) return false;
    out[0] = '\0';

    char hdr[256];
    if (httpd_req_get_hdr_value_str(req, "Cookie", hdr, sizeof(hdr)) != ESP_OK) return false;

    const char *p = strstr(hdr, COOKIE_NAME "=");
    if (!p) return false;
    p += strlen(COOKIE_NAME) + 1;

    size_t n = 0;
    while (p[n] && p[n] != ';' && p[n] != ' ' && p[n] != '\r' && p[n] != '\n' && n + 1 < out_len) {
        out[n] = p[n];
        n++;
    }
    out[n] = '\0';
    return n > 0;
}

static bool session_valid(httpd_req_t *req) {
    char tok[WEB_TOKEN_LEN + 8];
    if (!cookie_token(req, tok, sizeof(tok))) return false;

    session_prune();
    for (int i = 0; i < WEB_MAX_SESSIONS; i++) {
        if (s_sessions[i].used && strcmp(s_sessions[i].token, tok) == 0) {
            // Sliding expiry: keep active tabs signed in.
            s_sessions[i].expires_us = esp_timer_get_time() + (int64_t)WEB_SESSION_TIMEOUT_S * 1000000;
            return true;
        }
    }
    return false;
}

static void session_destroy(httpd_req_t *req) {
    char tok[WEB_TOKEN_LEN + 8];
    if (!cookie_token(req, tok, sizeof(tok))) return;
    for (int i = 0; i < WEB_MAX_SESSIONS; i++) {
        if (s_sessions[i].used && strcmp(s_sessions[i].token, tok) == 0) {
            s_sessions[i].used = false;
        }
    }
}

static void redirect_to(httpd_req_t *req, const char *location) {
    httpd_resp_set_status(req, "303 See Other");
    httpd_resp_set_hdr(req, "Location", location);
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_send(req, NULL, 0);
}

// Guard for every non-public page: unauthenticated requests go to /login.
static bool require_login(httpd_req_t *req) {
    if (session_valid(req)) return true;
    redirect_to(req, "/login");
    return false;
}

// =====================================================================
//  Device modes
//  Single place that answers "what is this device doing right now". The web
//  UI uses it to show the matching status card and enable only the actions
//  that make sense in that mode.
// =====================================================================

typedef enum {
    MODE_SETUP = 0,   // captive-portal AP, no Wi-Fi link yet
    MODE_WIFI_DOWN,   // station mode, no link / no IP
    MODE_REGISTERING, // online, SIP account not registered yet
    MODE_IDLE,        // registered and ready
    MODE_INCOMING,    // inbound call ringing
    MODE_OUTGOING,    // we are dialling
    MODE_ACTIVE,      // call in progress
} device_mode_t;

static device_mode_t device_mode(void) {
    if (wifi_is_ap_mode()) return MODE_SETUP;

    EventBits_t bits = app_event_group ? xEventGroupGetBits(app_event_group) : 0;
    if (!(bits & (WIFI_CONNECTED_BIT | IP_ACQUIRED_BIT))) return MODE_WIFI_DOWN;

    switch (g_sip_client ? sip_client_get_call_state(g_sip_client) : SIP_CALL_STATE_IDLE) {
        case SIP_CALL_STATE_INCOMING:
        case SIP_CALL_STATE_RINGING:      return MODE_INCOMING; // we sent 180 Ringing
        case SIP_CALL_STATE_INVITING:     return MODE_OUTGOING;
        case SIP_CALL_STATE_CONNECTING:
        case SIP_CALL_STATE_ACTIVE:
        case SIP_CALL_STATE_TERMINATING:  return MODE_ACTIVE;
        default:                          break;
    }
    return (bits & SIP_REGISTERED_BIT) ? MODE_IDLE : MODE_REGISTERING;
}

static const char *mode_class(device_mode_t m) {
    switch (m) {
        case MODE_SETUP:       return "setup";
        case MODE_WIFI_DOWN:   return "offline";
        case MODE_REGISTERING: return "wait";
        case MODE_INCOMING:    return "ring";
        case MODE_OUTGOING:    return "call";
        case MODE_ACTIVE:      return "call";
        default:               return "ok";
    }
}

static const char *mode_name(device_mode_t m) {
    switch (m) {
        case MODE_SETUP:       return "Setup mode";
        case MODE_WIFI_DOWN:   return "Offline";
        case MODE_REGISTERING: return "Registering";
        case MODE_INCOMING:    return "Incoming call";
        case MODE_OUTGOING:    return "Calling";
        case MODE_ACTIVE:      return "In call";
        default:               return "Ready";
    }
}

// ---- compile-time capability profile (shown as device info) ----
static const char *profile_name(void) {
#if defined(CONFIG_SIP_PROFILE_PRO)
    return "Profile: PRO / AI";
#elif defined(CONFIG_SIP_PROFILE_STANDARD)
    return "Profile: STANDARD";
#else
    return "Profile: LITE";
#endif
}

static const char *audio_name(void) {
#if defined(USE_CODEC_OPUS)
    return "Audio: OPUS 48 kHz";
#elif defined(USE_CODEC_G722)
    return "Audio: G.722 16 kHz";
#else
    return "Audio: G.711 8 kHz";
#endif
}

static const char *control_name(void) {
#if defined(CTRL_METHOD_AUTO)
    return "Control: auto-answer";
#elif defined(CTRL_METHOD_BUTTONS)
    return "Control: button + web";
#else
    return "Control: web";
#endif
}

// =====================================================================
//  Tiny HTML page builder (heap backed, truncation safe)
// =====================================================================

typedef struct {
    char  *buf;
    size_t cap;
    size_t len;
    bool   ok;
} page_t;

static void pg_init(page_t *p, size_t cap) {
    p->buf = calloc(1, cap);
    p->cap = p->buf ? cap : 0;
    p->len = 0;
    p->ok  = (p->buf != NULL);
}

static void pg_printf(page_t *p, const char *fmt, ...) {
    if (!p->ok || p->cap == 0 || p->len + 1 >= p->cap) {
        p->ok = false;
        return;
    }
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(p->buf + p->len, p->cap - p->len, fmt, ap);
    va_end(ap);
    if (n < 0) {
        p->ok = false;
    } else if ((size_t)n >= p->cap - p->len) {
        p->len = p->cap - 1; // keeps a NUL terminator
        p->ok = false;
    } else {
        p->len += (size_t)n;
    }
}

static void pg_puts(page_t *p, const char *s) { pg_printf(p, "%s", s); }

// Escape a string for safe inclusion inside HTML text/attributes.
static void pg_escape(page_t *p, const char *s) {
    if (!s) return;
    for (; *s; s++) {
        switch (*s) {
            case '&':  pg_puts(p, "&amp;");  break;
            case '<':  pg_puts(p, "&lt;");   break;
            case '>':  pg_puts(p, "&gt;");   break;
            case '"':  pg_puts(p, "&quot;"); break;
            case '\'': pg_puts(p, "&#39;");  break;
            default:   pg_printf(p, "%c", *s); break;
        }
    }
}

static const char PAGE_CSS[] =
    ":root{color-scheme:dark;}"
    "*{box-sizing:border-box;}"
    "body{margin:0;min-height:100vh;font-family:" "\"Segoe UI\",system-ui,-apple-system,Roboto,Helvetica,Arial,sans-serif;"
    "color:#eaf4fb;background:linear-gradient(135deg,#0f2027,#203a43,#2c5364);"
    "display:flex;justify-content:center;padding:18px;}"
    ".wrap{width:100%;max-width:440px;}"
    ".card{background:rgba(255,255,255,.08);backdrop-filter:blur(12px);"
    "border:1px solid rgba(255,255,255,.15);border-radius:16px;padding:22px;"
    "box-shadow:0 8px 32px rgba(0,0,0,.35);margin-bottom:14px;}"
    "h1{font-size:22px;margin:0 0 4px;letter-spacing:.3px;}"
    "h2{font-size:14px;text-transform:uppercase;letter-spacing:1px;color:#8fd6ef;margin:0 0 10px;}"
    ".sub{font-size:13px;color:#9fb3c6;margin:0 0 16px;line-height:1.45;}"
    "label{display:block;font-size:12px;letter-spacing:.4px;text-transform:uppercase;"
    "color:#a8bccd;margin:12px 0 5px;}"
    "input,select{width:100%;padding:11px;border-radius:9px;font-size:15px;"
    "border:1px solid rgba(255,255,255,.14);background:rgba(255,255,255,.06);color:#fff;}"
    "input::placeholder{color:#7d90a3;}"
    "input:focus,select:focus{outline:none;border-color:#00d2ff;box-shadow:0 0 10px rgba(0,210,255,.45);}"
    ".btn{display:block;width:100%;padding:13px;margin-top:14px;border:0;border-radius:10px;"
    "color:#fff;font-size:15px;font-weight:600;cursor:pointer;text-align:center;text-decoration:none;"
    "background:linear-gradient(90deg,#00d2ff,#3a7bd5);}"
    ".btn:hover{filter:brightness(1.1);}"
    ".btn:disabled{opacity:.38;cursor:not-allowed;filter:none;}"
    ".btn.call{background:linear-gradient(90deg,#11998e,#38ef7d);}"
    ".btn.answer{background:linear-gradient(90deg,#2193b0,#6dd5ed);}"
    ".btn.hangup{background:linear-gradient(90deg,#cb2d3e,#ef473a);}"
    ".actions{display:flex;gap:10px;}.actions form{flex:1;}.actions .btn{margin-top:0;}"
    ".banner{border-radius:16px;padding:18px;margin-bottom:14px;"
    "border:1px solid rgba(255,255,255,.16);box-shadow:0 8px 32px rgba(0,0,0,.3);}"
    ".banner h1{margin:0 0 6px;}"
    ".banner p{margin:0;font-size:13px;line-height:1.5;color:#e6f0f8;opacity:.92;}"
    ".banner.setup{background:linear-gradient(120deg,rgba(0,210,255,.22),rgba(58,123,213,.18));}"
    ".banner.offline{background:linear-gradient(120deg,rgba(203,45,62,.30),rgba(239,71,58,.16));}"
    ".banner.wait{background:linear-gradient(120deg,rgba(255,184,0,.24),rgba(255,140,0,.14));}"
    ".banner.ok{background:linear-gradient(120deg,rgba(17,153,142,.30),rgba(56,239,125,.18));}"
    ".banner.call{background:linear-gradient(120deg,rgba(0,210,255,.24),rgba(58,123,213,.20));}"
    ".banner.ring{background:linear-gradient(120deg,rgba(33,147,176,.36),rgba(109,213,237,.22));"
    "animation:pulse 1.5s ease-in-out infinite;}"
    "@keyframes pulse{0%,100%{box-shadow:0 8px 32px rgba(0,0,0,.3);}"
    "50%{box-shadow:0 0 24px rgba(0,210,255,.55);}}"
    ".navmode{font-size:11px;font-weight:700;letter-spacing:.6px;text-transform:uppercase;"
    "padding:7px 11px;border-radius:999px;border:1px solid rgba(255,255,255,.16);"
    "background:rgba(255,255,255,.08);color:#cfe9f5;}"
    ".navmode.ok{color:#8bf3b6;}.navmode.offline{color:#ffb3b8;}.navmode.wait{color:#ffd28a;}"
    ".navmode.ring,.navmode.call,.navmode.setup{color:#9fe8ff;}"
    ".nav{display:flex;flex-wrap:wrap;gap:8px;justify-content:center;margin-bottom:14px;}"
    ".nav a{color:#cfe9f5;text-decoration:none;font-size:13px;padding:7px 12px;"
    "border-radius:999px;background:rgba(255,255,255,.08);border:1px solid rgba(255,255,255,.12);}"
    ".nav a.on{background:linear-gradient(90deg,#00d2ff,#3a7bd5);border-color:transparent;"
    "color:#04121c;font-weight:700;}"
    ".status{font-size:17px;font-weight:600;color:#7fe3ff;margin:0 0 14px;}"
    ".pill{display:inline-block;font-size:12px;padding:4px 10px;border-radius:999px;"
    "background:rgba(255,255,255,.1);margin:0 4px 6px 0;color:#cfe9f5;}"
    ".pill.good{background:rgba(56,239,125,.18);color:#8bf3b6;}"
    ".pill.bad{background:rgba(203,45,62,.22);color:#ffb3b8;}"
    ".err{background:rgba(203,45,62,.22);border:1px solid #cb2d3e;border-radius:9px;"
    "padding:10px 12px;font-size:13px;margin:0 0 12px;}"
    ".ok{background:rgba(56,239,125,.15);border:1px solid #38ef7d;border-radius:9px;"
    "padding:10px 12px;font-size:13px;}"
    ".info{background:rgba(0,210,255,.12);border:1px solid rgba(0,210,255,.5);"
    "border-radius:9px;padding:10px 12px;font-size:13px;margin-bottom:12px;}"
    "table{width:100%;border-collapse:collapse;font-size:14px;}"
    "td{padding:9px 4px;border-bottom:1px solid rgba(255,255,255,.1);vertical-align:middle;}"
    ".row{display:flex;gap:10px;}.row>div{flex:1;}"
    ".del{background:none;border:1px solid rgba(255,255,255,.2);color:#ff9aa2;"
    "border-radius:8px;padding:5px 10px;cursor:pointer;font-size:12px;}"
    "a.link{color:#7fe3ff;font-size:13px;}";

static void pg_head(page_t *p, const char *title, int refresh_s) {
    pg_puts(p, "<!DOCTYPE html><html lang='en'><head><meta charset='UTF-8'>"
               "<meta name='viewport' content='width=device-width,initial-scale=1'>");
    if (refresh_s > 0) pg_printf(p, "<meta http-equiv='refresh' content='%d'>", refresh_s);
    pg_printf(p, "<title>%s</title><style>%s</style></head><body><div class='wrap'>", title, PAGE_CSS);
}

static void nav_link(page_t *p, const char *href, const char *label, bool active) {
    pg_printf(p, "<a href='%s'%s>%s</a>", href, active ? " class='on'" : "", label);
}

static void pg_nav(page_t *p, const char *active) {
    bool ap = wifi_is_ap_mode();
    device_mode_t m = device_mode();
    pg_puts(p, "<nav class='nav'>");
    if (!ap) {
        nav_link(p, "/", "Call", strcmp(active, "call") == 0);
        nav_link(p, "/phonebook", "Phonebook", strcmp(active, "phonebook") == 0);
    }
    nav_link(p, "/setup", "Settings", strcmp(active, "setup") == 0);
    if (!ap) nav_link(p, "/hardware", "Hardware", strcmp(active, "hardware") == 0);
    nav_link(p, "/logout", "Logout", false);
    // Current device mode, visible on every page.
    pg_printf(p, "<span class='navmode %s'>%s</span>", mode_class(m), mode_name(m));
    pg_puts(p, "</nav>");
}

static void pg_foot(page_t *p) { pg_puts(p, "</div></body></html>"); }

static void send_page(httpd_req_t *req, page_t *p) {
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_send(req, p->buf ? p->buf : "<html><body>Out of memory</body></html>",
                    HTTPD_RESP_USE_STRLEN);
    free(p->buf);
    p->buf = NULL;
    p->cap = p->len = 0;
}

// =====================================================================
//  Form helpers
// =====================================================================

static void url_decode(char *dst, size_t dst_len, const char *src) {
    if (!dst || dst_len == 0) return;
    size_t o = 0;
    while (*src && o + 1 < dst_len) {
        if (*src == '%' && isxdigit((unsigned char)src[1]) && isxdigit((unsigned char)src[2])) {
            char a = src[1], b = src[2];
            int hi = isdigit((unsigned char)a) ? a - '0' : (tolower((unsigned char)a) - 'a' + 10);
            int lo = isdigit((unsigned char)b) ? b - '0' : (tolower((unsigned char)b) - 'a' + 10);
            dst[o++] = (char)((hi << 4) | lo);
            src += 3;
        } else if (*src == '+') {
            dst[o++] = ' ';
            src++;
        } else {
            dst[o++] = *src++;
        }
    }
    dst[o] = '\0';
}

// Look up a field in an application/x-www-form-urlencoded body.
static bool form_get(const char *body, const char *key, char *out, size_t out_len) {
    if (!body || !key || !out || out_len == 0) return false;
    out[0] = '\0';

    const size_t klen = strlen(key);
    const char *p = body;
    while (*p) {
        const char *amp = strchr(p, '&');
        const char *seg_end = amp ? amp : p + strlen(p);
        const char *eq = memchr(p, '=', (size_t)(seg_end - p));

        if (eq && (size_t)(eq - p) == klen && strncmp(p, key, klen) == 0) {
            const char *val = eq + 1;
            size_t vlen = (size_t)(seg_end - val);
            char enc[192];
            if (vlen > sizeof(enc) - 1) vlen = sizeof(enc) - 1;
            memcpy(enc, val, vlen);
            enc[vlen] = '\0';
            url_decode(out, out_len, enc);
            return true;
        }
        if (!amp) break;
        p = amp + 1;
    }
    return false;
}

typedef enum {
    APPLY_UNCHANGED = 0, // field absent / blank / identical to the stored value
    APPLY_CHANGED,       // stored value replaced
    APPLY_TOO_LONG,      // value does not fit: rejected, never truncated
} apply_result_t;

// Copy a form field into a fixed-size setting. Blank means "keep the stored
// value" unless allow_empty (used for the optional domain/target/name fields).
// Values that do not fit are rejected so a saved secret can never be truncated.
static apply_result_t apply_str(const char *body, const char *key, char *dst, size_t dst_size,
                                bool allow_empty) {
    char v[192];
    if (!form_get(body, key, v, sizeof(v))) return APPLY_UNCHANGED;
    if (v[0] == '\0' && !allow_empty) return APPLY_UNCHANGED;
    if (strlen(v) >= dst_size) return APPLY_TOO_LONG;
    if (strcmp(v, dst) == 0) return APPLY_UNCHANGED;
    snprintf(dst, dst_size, "%s", v);
    return APPLY_CHANGED;
}

// Apply one field and record which settings group it belongs to.
static void apply_field(const char *body, const char *key, char *dst, size_t dst_size,
                        bool allow_empty, bool *group_changed, bool *too_long) {
    switch (apply_str(body, key, dst, dst_size, allow_empty)) {
        case APPLY_CHANGED:  *group_changed = true; break;
        case APPLY_TOO_LONG: *too_long = true;      break;
        default: break;
    }
}

static int8_t parse_pin(const char *v) {
    int p = atoi(v);
    if (p < -1) p = -1;
    if (p > 48) p = 48;
    return (int8_t)p;
}

// Number dialled by the on-device button / wake word / web "Call" button.
static const char *call_target(void) {
    if (g_settings && g_settings->sip_target[0]) return g_settings->sip_target;
    return SIP_TARGET_URI;
}

// =====================================================================
//  Physical call button (optional control method)
// =====================================================================
#ifdef CTRL_METHOD_BUTTONS
#include "driver/gpio.h"

static void button_task(void *arg) {
    (void)arg;
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << BUTTON_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = 1,
        .pull_down_en = 0,
        .intr_type = GPIO_INTR_DISABLE
    };
    gpio_config(&io_conf);

    int last_state = 1;
    while (1) {
        int state = gpio_get_level(BUTTON_GPIO);
        if (state == 0 && last_state == 1) {
            ESP_LOGI(TAG, "Button Pressed!");
            if (g_sip_client) {
                sip_call_state_t st = sip_client_get_call_state(g_sip_client);
                if (st == SIP_CALL_STATE_IDLE) {
                    sip_client_initiate_call(g_sip_client, call_target());
                } else if (st == SIP_CALL_STATE_INCOMING) {
                    sip_client_answer_call(g_sip_client);
                } else {
                    sip_client_terminate_call(g_sip_client);
                }
            }
            vTaskDelay(pdMS_TO_TICKS(500));
        }
        last_state = state;
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}
#endif

// =====================================================================
//  Pages
// =====================================================================

static void render_login(httpd_req_t *req, const char *error) {
    page_t p;
    pg_init(&p, 6144);
    pg_head(&p, "Sign in - ESP32 SIP", 0);
    pg_puts(&p, "<div class='card'><h1>ESP32 SIP</h1>"
                "<p class='sub'>Sign in to manage this device.</p>");
    if (error) {
        pg_puts(&p, "<div class='err'>");
        pg_escape(&p, error);
        pg_puts(&p, "</div>");
    }
    if (wifi_is_ap_mode()) {
        pg_puts(&p, "<div class='info'>Setup mode (open access point). The factory login is "
                    "admin / esp32sip - change it on the Settings page once this device is on "
                    "your own network, because anyone in radio range can reach this page.</div>");
    }
    pg_puts(&p, "<form method='POST' action='/login'>"
                "<label>Username</label>"
                "<input type='text' name='user' autocomplete='username' autofocus>"
                "<label>Password</label>"
                "<input type='password' name='pass' autocomplete='current-password'>"
                "<button class='btn' type='submit'>Sign in</button></form></div>");
    pg_foot(&p);
    send_page(req, &p);
}

static esp_err_t login_get_handler(httpd_req_t *req) {
    if (session_valid(req)) {
        redirect_to(req, "/");
        return ESP_OK;
    }
    render_login(req, NULL);
    return ESP_OK;
}

static esp_err_t login_post_handler(httpd_req_t *req) {
    // Non-blocking throttle: the HTTP server runs in a single task, so we must
    // never sleep here (a blocking delay would be an easy unauthenticated DoS).
    int64_t now = esp_timer_get_time();
    if (now < s_login_blocked_until) {
        char msg[96];
        snprintf(msg, sizeof(msg), "Too many failed attempts. Try again in %d seconds.",
                 (int)((s_login_blocked_until - now) / 1000000) + 1);
        render_login(req, msg);
        return ESP_OK;
    }

    if (req->content_len <= 0 || req->content_len > 512) return ESP_FAIL;

    char buf[512];
    int len = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (len <= 0) {
        render_login(req, "Login request was empty, please try again.");
        return ESP_OK;
    }
    buf[len] = '\0';

    char user[64] = {0}, pass[128] = {0};
    form_get(buf, "user", user, sizeof(user));
    form_get(buf, "pass", pass, sizeof(pass));

    const char *want_user = (g_settings && g_settings->web_user[0])     ? g_settings->web_user     : WEB_UI_USER;
    const char *want_pass = (g_settings && g_settings->web_password[0]) ? g_settings->web_password : WEB_UI_PASSWORD;

    if (user[0] && pass[0] && strcmp(user, want_user) == 0 && strcmp(pass, want_pass) == 0) {
        s_login_fails = 0;
        s_login_blocked_until = 0;
        const char *token = session_create();
        char cookie[128];
        snprintf(cookie, sizeof(cookie),
                 COOKIE_NAME "=%s; Path=/; Max-Age=%d; HttpOnly; SameSite=Lax",
                 token, WEB_SESSION_TIMEOUT_S);
        httpd_resp_set_hdr(req, "Set-Cookie", cookie);
        ESP_LOGI(TAG, "Web login OK (user='%s')", user);
        redirect_to(req, "/");
        return ESP_OK;
    }

    s_login_fails++;
    ESP_LOGW(TAG, "Web login FAILED (user='%s', %d consecutive failures)", user, s_login_fails);
    if (s_login_fails >= WEB_LOGIN_MAX_FAILURES) {
        s_login_fails = 0;
        s_login_blocked_until = esp_timer_get_time() + (int64_t)WEB_LOGIN_LOCKOUT_S * 1000000;
        ESP_LOGW(TAG, "Login locked for %d s after repeated failures", WEB_LOGIN_LOCKOUT_S);
        render_login(req, "Too many failed attempts. Login temporarily locked.");
        return ESP_OK;
    }
    render_login(req, "Invalid username or password.");
    return ESP_OK;
}

static esp_err_t logout_get_handler(httpd_req_t *req) {
    session_destroy(req);
    httpd_resp_set_hdr(req, "Set-Cookie",
                       COOKIE_NAME "=; Path=/; Max-Age=0; HttpOnly; SameSite=Lax");
    redirect_to(req, "/login");
    return ESP_OK;
}

static void action_button(page_t *p, const char *action, const char *label,
                          const char *cls, bool enabled) {
    pg_printf(p, "<form method='POST' action='%s'>"
                 "<button class='btn %s' type='submit'%s>%s</button></form>",
              action, cls, enabled ? "" : " disabled", label);
}

// The mode card: what the device is doing and what can be done about it.
static void render_mode_card(page_t *p, device_mode_t mode, const char *remote_uri) {
    pg_printf(p, "<div class='banner %s'><h1>%s</h1><p>", mode_class(mode), mode_name(mode));
    switch (mode) {
        case MODE_SETUP:
            pg_puts(p, "No Wi-Fi link yet - this device is broadcasting its own open setup access "
                       "point. Enter your Wi-Fi and SIP account to bring it online.");
            break;
        case MODE_WIFI_DOWN:
            pg_puts(p, "Wi-Fi is not connected. The device keeps retrying; if this persists, check "
                       "the network name and password.");
            break;
        case MODE_REGISTERING:
            pg_puts(p, "Online, but the SIP account is not registered yet. Check the server, "
                       "username and password.");
            break;
        case MODE_IDLE:
            pg_puts(p, "Registered and idle - ready to place or receive a call.");
            break;
        case MODE_INCOMING:
            pg_puts(p, "An inbound call is ringing");
            if (remote_uri && remote_uri[0]) {
                pg_puts(p, " from ");
                pg_escape(p, remote_uri);
            }
            pg_puts(p, ".");
            break;
        case MODE_OUTGOING:
            pg_puts(p, "Dialling ");
            if (remote_uri && remote_uri[0]) {
                pg_escape(p, remote_uri);
            } else {
                pg_escape(p, call_target());
            }
            pg_puts(p, " - waiting for the remote party to answer.");
            break;
        case MODE_ACTIVE:
            pg_puts(p, "A call is in progress");
            if (remote_uri && remote_uri[0]) {
                pg_puts(p, " with ");
                pg_escape(p, remote_uri);
            }
            pg_puts(p, ".");
            break;
        default:
            break;
    }
    pg_puts(p, "</p></div>");
}

// Main page: shows the current device mode, the connection/account state and
// the actions that are valid in that mode.
static esp_err_t index_get_handler(httpd_req_t *req) {
    if (!require_login(req)) return ESP_OK;

    device_mode_t mode = device_mode();
    bool calling = (mode == MODE_IDLE || mode == MODE_INCOMING ||
                    mode == MODE_OUTGOING || mode == MODE_ACTIVE);

    char remote[160] = {0};
    if (g_sip_client) sip_client_get_remote_uri(g_sip_client, remote, sizeof(remote));

    // Poll a bit faster while a call is ringing so Answer shows up promptly.
    page_t p;
    pg_init(&p, 12288);
    pg_head(&p, "ESP32 SIP Phone", (mode == MODE_INCOMING) ? 3 : 5);
    pg_nav(&p, "call");

    render_mode_card(&p, mode, remote);

    EventBits_t bits = app_event_group ? xEventGroupGetBits(app_event_group) : 0;
    bool wifi_up = (bits & WIFI_CONNECTED_BIT) != 0;
    bool registered = (bits & SIP_REGISTERED_BIT) != 0;

    esp_ip4_addr_t ip = {0};
    get_my_ip(&ip);
    char ip_str[16] = "0.0.0.0";
    snprintf(ip_str, sizeof(ip_str), IPSTR, IP2STR(&ip));

    int64_t up_s = esp_timer_get_time() / 1000000;

    pg_puts(&p, "<div class='card'><h2>Connection</h2>");
    pg_printf(&p, "<span class='pill %s'>Wi-Fi %s</span>", wifi_up ? "good" : "bad",
              wifi_up ? "connected" : "disconnected");
    pg_printf(&p, "<span class='pill %s'>SIP %s</span>", registered ? "good" : "bad",
              registered ? "registered" : "not registered");
    pg_printf(&p, "<span class='pill'>IP %s</span>", ip_str);
    pg_printf(&p, "<span class='pill'>Up %d:%02d:%02d</span>",
              (int)(up_s / 3600), (int)((up_s / 60) % 60), (int)(up_s % 60));
    if (g_settings) {
        pg_puts(&p, "<span class='pill'>Account ");
        pg_escape(&p, g_settings->sip_user);
        pg_puts(&p, "</span><span class='pill'>Server ");
        pg_escape(&p, g_settings->sip_server);
        pg_printf(&p, ":%u</span>",
                  (unsigned)(g_settings->sip_port ? g_settings->sip_port : SIP_SERVER_PORT));
    }
    if (calling) {
        pg_puts(&p, "<p class='sub' style='margin:12px 0 0'>Call target: ");
        pg_escape(&p, call_target());
        pg_puts(&p, "</p>");
    }

    if (calling) {
        pg_puts(&p, "<h2 style='margin-top:18px'>Call control</h2><div class='actions'>");
        action_button(&p, "/call", "Call", "call", mode == MODE_IDLE);
        action_button(&p, "/answer", "Answer", "answer", mode == MODE_INCOMING);
        action_button(&p, "/hangup", (mode == MODE_OUTGOING) ? "Cancel" : "Hang up", "hangup",
                      mode == MODE_INCOMING || mode == MODE_OUTGOING || mode == MODE_ACTIVE);
        pg_puts(&p, "</div>");
    } else {
        pg_puts(&p, "<h2 style='margin-top:18px'>Call control</h2>"
                    "<p class='sub'>Call buttons unlock once the phone is registered.</p>"
                    "<div class='actions'>");
        action_button(&p, "/call", "Call", "call", false);
        action_button(&p, "/answer", "Answer", "answer", false);
        action_button(&p, "/hangup", "Hang up", "hangup", false);
        pg_puts(&p, "</div><a class='btn' href='/setup'>Open settings</a>");
    }

    pg_puts(&p, "<h2 style='margin-top:18px'>Device</h2>");
    pg_printf(&p, "<span class='pill'>%s</span>", profile_name());
    pg_printf(&p, "<span class='pill'>%s</span>", audio_name());
    pg_printf(&p, "<span class='pill'>%s</span>", control_name());
#if defined(USE_WAKE_WORD) && USE_WAKE_WORD
    pg_puts(&p, "<span class='pill good'>Wake word on</span>");
#else
    pg_puts(&p, "<span class='pill'>Wake word off</span>");
#endif
    pg_puts(&p, "</div>");

    pg_foot(&p);
    send_page(req, &p);
    return ESP_OK;
}

// Settings: Wi-Fi, SIP account and web credentials.
static esp_err_t setup_get_handler(httpd_req_t *req) {
    if (!require_login(req)) return ESP_OK;

    page_t p;
    pg_init(&p, 12288);
    pg_head(&p, "Settings - ESP32 SIP", 0);
    pg_nav(&p, "setup");

    pg_puts(&p, "<div class='card'><h1>Settings</h1>"
                "<p class='sub'>Leave a password field empty to keep the stored value. "
                "SIP and login changes apply immediately (no restart); only Wi-Fi changes "
                "restart the device.</p>");
    if (wifi_is_ap_mode()) {
        pg_puts(&p, "<div class='info'>Setup mode: the device is not connected to Wi-Fi yet, so "
                    "Wi-Fi changes here restart it. Web access credentials can only be changed "
                    "once the device is on your own network.</div>");
    }
    pg_puts(&p, "<form method='POST' action='/setup'>");

    // --- Wi-Fi ---
    pg_puts(&p, "<h2>Wi-Fi</h2><label>Network name (SSID)</label>");
    pg_puts(&p, "<input type='text' name='ssid' maxlength='31' value='");
    pg_escape(&p, g_settings->wifi_ssid);
    pg_puts(&p, "' placeholder='Your Wi-Fi SSID'>");
    pg_puts(&p, "<label>Wi-Fi password</label>"
                "<input type='password' name='wifi_pass' maxlength='63' placeholder='Leave empty to keep current'>");

    // --- SIP account ---
    pg_puts(&p, "<h2 style='margin-top:22px'>SIP account</h2>"
                "<label>SIP server (IP or domain)</label><input type='text' name='sip_server' maxlength='63' value='");
    pg_escape(&p, g_settings->sip_server);
    pg_puts(&p, "' placeholder='sip.provider.com'>");
    pg_puts(&p, "<div class='row'><div><label>SIP port</label><input type='number' name='sip_port' min='1' max='65535' value='");
    pg_printf(&p, "%u", (unsigned)(g_settings->sip_port ? g_settings->sip_port : SIP_SERVER_PORT));
    pg_puts(&p, "'></div><div><label>Domain / realm</label><input type='text' name='sip_domain' maxlength='63' value='");
    pg_escape(&p, g_settings->sip_domain);
    pg_puts(&p, "' placeholder='Defaults to server'></div></div>");
    pg_puts(&p, "<label>Auth username</label><input type='text' name='sip_user' maxlength='63' value='");
    pg_escape(&p, g_settings->sip_user);
    pg_puts(&p, "' placeholder='1000'>");
    pg_puts(&p, "<label>Auth password</label>"
                "<input type='password' name='sip_pass' maxlength='63' placeholder='Leave empty to keep current'>");
    pg_puts(&p, "<label>Display name</label><input type='text' name='display_name' maxlength='31' value='");
    pg_escape(&p, g_settings->sip_display_name);
    pg_puts(&p, "' placeholder='ESP32 Phone'>");
    pg_puts(&p, "<label>Default call target</label><input type='text' name='sip_target' maxlength='63' value='");
    pg_escape(&p, g_settings->sip_target);
    pg_puts(&p, "' placeholder='sip:1001@192.168.1.100'>");

    // --- Web access ---
    if (!wifi_is_ap_mode()) {
        pg_puts(&p, "<h2 style='margin-top:22px'>Web access</h2>"
                    "<label>Login username</label><input type='text' name='web_user' maxlength='31' value='");
        pg_escape(&p, g_settings->web_user);
        pg_puts(&p, "' placeholder='admin'>");
        pg_puts(&p, "<label>Login password</label>"
                    "<input type='password' name='web_pass' maxlength='63' placeholder='Leave empty to keep current'>");
    } else {
        pg_puts(&p, "<div class='info'><b>Web access</b> is disabled in setup mode: finish "
                    "this setup, then change the default login from your own network.</div>");
    }

    pg_puts(&p, "<button class='btn' type='submit'>Save settings</button></form></div>");
    pg_foot(&p);
    send_page(req, &p);
    return ESP_OK;
}

static esp_err_t setup_post_handler(httpd_req_t *req) {
    if (!require_login(req)) return ESP_OK;
    if (req->content_len <= 0 || req->content_len > 2048) return ESP_FAIL;

    char *buf = malloc(req->content_len + 1);
    if (!buf) return ESP_FAIL;

    int ret = httpd_req_recv(req, buf, req->content_len);
    if (ret <= 0) {
        free(buf);
        return ESP_FAIL;
    }
    buf[ret] = '\0';

    app_settings_t updated = *g_settings;
    // Which group of settings changed decides how (and whether) we restart.
    bool wifi_changed = false, sip_changed = false, web_changed = false;
    bool too_long = false;

    apply_field(buf, "ssid",         updated.wifi_ssid,        sizeof(updated.wifi_ssid),        false, &wifi_changed, &too_long);
    apply_field(buf, "wifi_pass",    updated.wifi_password,    sizeof(updated.wifi_password),    false, &wifi_changed, &too_long);
    apply_field(buf, "sip_server",   updated.sip_server,       sizeof(updated.sip_server),       false, &sip_changed,  &too_long);
    apply_field(buf, "sip_user",     updated.sip_user,         sizeof(updated.sip_user),         false, &sip_changed,  &too_long);
    apply_field(buf, "sip_pass",     updated.sip_password,     sizeof(updated.sip_password),     false, &sip_changed,  &too_long);
    apply_field(buf, "display_name", updated.sip_display_name, sizeof(updated.sip_display_name), true,  &sip_changed,  &too_long);
    apply_field(buf, "sip_domain",   updated.sip_domain,       sizeof(updated.sip_domain),       true,  &sip_changed,  &too_long);
    apply_field(buf, "sip_target",   updated.sip_target,       sizeof(updated.sip_target),       true,  &sip_changed,  &too_long);

    // Web credentials are only changeable once the device is on a trusted
    // network: on the open setup AP anyone in range could otherwise take over
    // the login and lock the owner out.
    if (!wifi_is_ap_mode()) {
        apply_field(buf, "web_user", updated.web_user,     sizeof(updated.web_user),     false, &web_changed, &too_long);
        apply_field(buf, "web_pass", updated.web_password, sizeof(updated.web_password), false, &web_changed, &too_long);
    }

    char v[192];
    if (form_get(buf, "sip_port", v, sizeof(v)) && v[0]) {
        int port = atoi(v);
        if (port > 0 && port < 65536 && (uint16_t)port != updated.sip_port) {
            updated.sip_port = (uint16_t)port;
            sip_changed = true;
        }
    }

    bool changed = wifi_changed || sip_changed || web_changed;

    if (too_long) {
        page_t p;
        pg_init(&p, 2048);
        pg_head(&p, "Settings - ESP32 SIP", 0);
        pg_nav(&p, "setup");
        pg_puts(&p, "<div class='card'><h1>Value too long</h1>"
                    "<p class='sub'>One of the values you entered is too long for its field, so "
                    "nothing was saved. Shorten it and try again (usernames and passwords must "
                    "fit in 63 characters, the SSID in 31).</p>"
                    "<a class='link' href='/setup'>&laquo; Back to settings</a></div>");
        pg_foot(&p);
        send_page(req, &p);
        free(buf);
        return ESP_OK;
    }
    if (!changed) {
        page_t p;
        pg_init(&p, 2048);
        pg_head(&p, "Settings - ESP32 SIP", 0);
        pg_nav(&p, "setup");
        pg_puts(&p, "<div class='card'><h1>Nothing to save</h1>"
                    "<p class='sub'>No field was filled in, so the stored configuration "
                    "was left untouched.</p><a class='link' href='/setup'>&laquo; Back to settings</a></div>");
        pg_foot(&p);
        send_page(req, &p);
        free(buf);
        return ESP_OK;
    }

    *g_settings = updated;
    config_manager_save(g_settings);
    ESP_LOGI(TAG, "Settings updated from web UI (sip_server='%s', sip_user='%s')",
             g_settings->sip_server, g_settings->sip_user);

    // Wi-Fi needs a fresh association and the AP/setup path has no SIP client,
    // so those cases still restart. SIP + web credentials apply in place.
    bool need_reboot = wifi_changed || wifi_is_ap_mode();
    bool sip_applied = false;
    if (!need_reboot && sip_changed && g_sip_client) {
        sip_applied = (sip_client_reload(g_sip_client) == ESP_OK);
    }

    page_t p;
    if (need_reboot) {
        pg_init(&p, 3072);
        pg_head(&p, "Restarting - ESP32 SIP", 0);
        pg_nav(&p, "setup");
        pg_puts(&p, "<div class='card'><h1>Settings saved</h1>"
                    "<p class='sub'>Wi-Fi changes need a restart. The device will reconnect "
                    "with the new configuration...</p>"
                    "<div class='ok'>Rebooting...</div></div>");
        pg_foot(&p);
        send_page(req, &p);
        free(buf);
        vTaskDelay(pdMS_TO_TICKS(1500));
        esp_restart();
        return ESP_OK;
    }

    pg_init(&p, 3072);
    pg_head(&p, "Settings - ESP32 SIP", 0);
    pg_nav(&p, "setup");
    pg_puts(&p, "<div class='card'><h1>Settings saved</h1>");
    if (sip_applied) {
        pg_puts(&p, "<p class='sub'>The SIP account was updated and the device is "
                    "re-registering now. If a call was in progress, the new account is "
                    "applied as soon as the line is idle.</p>");
    } else if (sip_changed) {
        pg_puts(&p, "<p class='sub'>The SIP account was stored and takes effect once the "
                    "device is online.</p>");
    }
    if (web_changed) {
        pg_puts(&p, "<p class='sub'>The web login was updated. It is required from the next "
                    "sign-in; this session stays signed in.</p>");
    }
    pg_puts(&p, "<div class='ok'>No restart required</div>"
                "<a class='link' href='/' style='display:inline-block;margin-top:12px'>"
                "&laquo; Back to phone</a></div>");
    pg_foot(&p);
    send_page(req, &p);
    free(buf);
    return ESP_OK;
}

static esp_err_t action_post_handler(httpd_req_t *req) {
    if (!require_login(req)) return ESP_OK;
    if (!g_sip_client) return ESP_FAIL;

    if (strcmp(req->uri, "/call") == 0) {
        sip_client_initiate_call(g_sip_client, call_target());
    } else if (strcmp(req->uri, "/answer") == 0) {
        sip_client_answer_call(g_sip_client);
    } else if (strcmp(req->uri, "/hangup") == 0) {
        sip_client_terminate_call(g_sip_client);
    }
    redirect_to(req, "/");
    return ESP_OK;
}

static esp_err_t phonebook_get_handler(httpd_req_t *req) {
    if (!require_login(req)) return ESP_OK;

    page_t p;
    pg_init(&p, 8192);
    pg_head(&p, "Phonebook - ESP32 SIP", 0);
    pg_nav(&p, "phonebook");
    pg_puts(&p, "<div class='card'><h1>Phonebook</h1>"
                "<p class='sub'>Speed dial slots 0-9, stored in NVS.</p>");

    phonebook_entry_t entry;
    bool any = false;
    pg_puts(&p, "<table>");
    for (int i = 0; i < MAX_PHONEBOOK_ENTRIES; i++) {
        if (!phonebook_load_entry(i, &entry)) continue;
        any = true;
        pg_printf(&p, "<tr><td><b>%d</b></td><td>", i);
        pg_escape(&p, entry.name);
        pg_puts(&p, "<br><span class='sub' style='margin:0'>");
        pg_escape(&p, entry.uri);
        pg_puts(&p, "</span></td><td style='text-align:right'>"
                    "<form method='POST' action='/pb_del' style='display:inline'>");
        pg_printf(&p, "<input type='hidden' name='id' value='%d'>", i);
        pg_puts(&p, "<button class='del' type='submit'>Delete</button></form></td></tr>");
    }
    pg_puts(&p, "</table>");
    if (!any) pg_puts(&p, "<p class='sub'>No entries yet.</p>");

    pg_puts(&p, "<h2 style='margin-top:22px'>Add / replace entry</h2>"
                "<form method='POST' action='/pb_add'>"
                "<label>Name</label><input type='text' name='name' placeholder='Front door'>"
                "<div class='row'><div><label>SIP URI</label>"
                "<input type='text' name='uri' placeholder='sip:1000@192.168.1.10'></div>"
                "<div><label>Slot</label><input type='number' name='id' min='0' max='9' value='0'></div></div>"
                "<button class='btn' type='submit'>Save entry</button></form></div>");
    pg_foot(&p);
    send_page(req, &p);
    return ESP_OK;
}

static esp_err_t pb_add_post_handler(httpd_req_t *req) {
    if (!require_login(req)) return ESP_OK;
    if (req->content_len <= 0 || req->content_len > 512) return ESP_FAIL;

    char buf[512];
    int ret = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (ret > 0) {
        buf[ret] = '\0';
        char name[MAX_NAME_LEN] = {0}, uri[MAX_URI_LEN] = {0}, id_str[8] = {0};
        form_get(buf, "name", name, sizeof(name));
        form_get(buf, "uri", uri, sizeof(uri));
        form_get(buf, "id", id_str, sizeof(id_str));
        int id = atoi(id_str);
        if (id < 0 || id >= MAX_PHONEBOOK_ENTRIES) id = 0;
        if (uri[0]) phonebook_save_entry((uint8_t)id, name, uri);
    }
    redirect_to(req, "/phonebook");
    return ESP_OK;
}

static esp_err_t pb_del_post_handler(httpd_req_t *req) {
    if (!require_login(req)) return ESP_OK;

    char buf[64];
    int ret = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (ret > 0) {
        buf[ret] = '\0';
        char id_str[8] = {0};
        form_get(buf, "id", id_str, sizeof(id_str));
        int id = atoi(id_str);
        if (id >= 0 && id < MAX_PHONEBOOK_ENTRIES) phonebook_clear_entry((uint8_t)id);
    }
    redirect_to(req, "/phonebook");
    return ESP_OK;
}

static void hw_pin_field(page_t *p, const char *label, const char *name, int8_t value) {
    pg_puts(p, "<label>");
    pg_puts(p, label);
    pg_puts(p, "</label>");
    pg_printf(p, "<input type='number' name='%s' min='-1' max='48' value='%d'>", name, value);
}

static esp_err_t hardware_get_handler(httpd_req_t *req) {
    if (!require_login(req)) return ESP_OK;

    hardware_settings_t hw;
    config_manager_load_hw(&hw);

    page_t p;
    pg_init(&p, 12288);
    pg_head(&p, "Hardware - ESP32 SIP", 0);
    pg_nav(&p, "hardware");
    pg_puts(&p, "<div class='card'><h1>Hardware</h1>"
                "<p class='sub'>GPIO map and display theme. Use -1 for \"not connected\". "
                "Saving restarts the device.</p><form method='POST' action='/hardware'>");

    pg_puts(&p, "<h2>I2S audio</h2>");
    hw_pin_field(&p, "BCLK", "bck", hw.pin_i2s_bck);
    hw_pin_field(&p, "LRCLK / WS", "ws", hw.pin_i2s_ws);
    hw_pin_field(&p, "Data out (DAC)", "dout", hw.pin_i2s_dout);
    hw_pin_field(&p, "Data in (MIC)", "din", hw.pin_i2s_din);
    hw_pin_field(&p, "Master clock (MCLK)", "mclk", hw.pin_i2s_mclk);

    pg_puts(&p, "<h2 style='margin-top:22px'>I2C control</h2>");
    hw_pin_field(&p, "SDA", "sda", hw.pin_i2c_sda);
    hw_pin_field(&p, "SCL", "scl", hw.pin_i2c_scl);

    pg_puts(&p, "<h2 style='margin-top:22px'>SPI display &amp; touch</h2>");
    hw_pin_field(&p, "SPI MOSI", "spi_mosi", hw.pin_spi_mosi);
    hw_pin_field(&p, "SPI MISO", "spi_miso", hw.pin_spi_miso);
    hw_pin_field(&p, "SPI clock", "spi_clk", hw.pin_spi_clk);
    hw_pin_field(&p, "TFT CS", "tft_cs", hw.pin_tft_cs);
    hw_pin_field(&p, "TFT DC", "tft_dc", hw.pin_tft_dc);
    hw_pin_field(&p, "TFT reset", "tft_rst", hw.pin_tft_rst);
    hw_pin_field(&p, "Touch CS", "touch_cs", hw.pin_touch_cs);
    hw_pin_field(&p, "Touch IRQ", "touch_irq", hw.pin_touch_irq);

    pg_puts(&p, "<h2 style='margin-top:22px'>Display theme</h2>"
                "<select name='ui_theme'>");
    static const char *themes[] = { "Voice Assistant", "Mobile OS", "Smart Speaker" };
    for (int i = 0; i < 3; i++) {
        pg_printf(&p, "<option value='%d'%s>%s</option>", i,
                  hw.ui_theme == i ? " selected" : "", themes[i]);
    }
    pg_puts(&p, "</select><button class='btn' type='submit'>Save &amp; restart</button></form></div>");
    pg_foot(&p);
    send_page(req, &p);
    return ESP_OK;
}

static esp_err_t hardware_post_handler(httpd_req_t *req) {
    if (!require_login(req)) return ESP_OK;
    if (req->content_len <= 0 || req->content_len > 2048) return ESP_FAIL;

    char *buf = malloc(req->content_len + 1);
    if (!buf) return ESP_FAIL;

    int ret = httpd_req_recv(req, buf, req->content_len);
    if (ret <= 0) {
        free(buf);
        return ESP_FAIL;
    }
    buf[ret] = '\0';

    hardware_settings_t hw;
    config_manager_load_hw(&hw); // Load current first

    char v[32];
    bool changed = false;
    struct { const char *key; int8_t *dst; } pin_map[] = {
        { "bck",       &hw.pin_i2s_bck   },
        { "ws",        &hw.pin_i2s_ws    },
        { "dout",      &hw.pin_i2s_dout  },
        { "din",       &hw.pin_i2s_din   },
        { "mclk",      &hw.pin_i2s_mclk  },
        { "sda",       &hw.pin_i2c_sda   },
        { "scl",       &hw.pin_i2c_scl   },
        { "spi_mosi",  &hw.pin_spi_mosi  },
        { "spi_miso",  &hw.pin_spi_miso  },
        { "spi_clk",   &hw.pin_spi_clk   },
        { "tft_cs",    &hw.pin_tft_cs    },
        { "tft_dc",    &hw.pin_tft_dc    },
        { "tft_rst",   &hw.pin_tft_rst   },
        { "touch_cs",  &hw.pin_touch_cs  },
        { "touch_irq", &hw.pin_touch_irq },
    };
    // A cleared field must mean "leave it alone" - never atoi("") == pin 0,
    // which would land on a strapping pin.
    for (size_t i = 0; i < sizeof(pin_map) / sizeof(pin_map[0]); i++) {
        if (form_get(buf, pin_map[i].key, v, sizeof(v)) && v[0]) {
            int8_t pin = parse_pin(v);
            if (pin != *pin_map[i].dst) {
                *pin_map[i].dst = pin;
                changed = true;
            }
        }
    }
    if (form_get(buf, "ui_theme", v, sizeof(v)) && v[0]) {
        int t = atoi(v);
        if (t >= 0 && t <= 2 && (uint8_t)t != hw.ui_theme) {
            hw.ui_theme = (uint8_t)t;
            changed = true;
        }
    }

    config_manager_save_hw(&hw);

    page_t p;
    pg_init(&p, 2048);
    pg_head(&p, "Hardware - ESP32 SIP", 0);
    pg_nav(&p, "hardware");
    pg_puts(&p, "<div class='card'><h1>Hardware saved</h1>"
                "<p class='sub'>The device is restarting to apply the new GPIO map.</p>"
                "<div class='ok'>Rebooting...</div></div>");
    pg_foot(&p);
    send_page(req, &p);

    free(buf);
    if (changed) {
        vTaskDelay(pdMS_TO_TICKS(1500));
        esp_restart();
    }
    return ESP_OK;
}

// =====================================================================
//  HTTP server
// =====================================================================

static httpd_handle_t server = NULL;

static void start_webserver(void) {
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    // Wildcard matching so the captive-portal catch-all ("/*") works, and a
    // raised handler limit (default 8 was too low for all our routes).
    config.uri_match_fn = httpd_uri_match_wildcard;
    config.max_uri_handlers = 20;
    config.lru_purge_enable = true;

    if (httpd_start(&server, &config) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTP server");
        return;
    }

    static const httpd_uri_t routes[] = {
        { .uri = "/",            .method = HTTP_GET,  .handler = index_get_handler    },
        { .uri = "/login",       .method = HTTP_GET,  .handler = login_get_handler    },
        { .uri = "/login",       .method = HTTP_POST, .handler = login_post_handler   },
        { .uri = "/logout",      .method = HTTP_GET,  .handler = logout_get_handler   },
        { .uri = "/setup",       .method = HTTP_GET,  .handler = setup_get_handler    },
        { .uri = "/setup",       .method = HTTP_POST, .handler = setup_post_handler   },
        { .uri = "/hardware",    .method = HTTP_GET,  .handler = hardware_get_handler },
        { .uri = "/hardware",    .method = HTTP_POST, .handler = hardware_post_handler },
        { .uri = "/call",        .method = HTTP_POST, .handler = action_post_handler  },
        { .uri = "/answer",      .method = HTTP_POST, .handler = action_post_handler  },
        { .uri = "/hangup",      .method = HTTP_POST, .handler = action_post_handler  },
        { .uri = "/phonebook",   .method = HTTP_GET,  .handler = phonebook_get_handler },
        { .uri = "/pb_add",      .method = HTTP_POST, .handler = pb_add_post_handler  },
        { .uri = "/pb_del",      .method = HTTP_POST, .handler = pb_del_post_handler  },
        // Captive-portal catch-all: any other GET (e.g. /generate_204,
        // /hotspot-detect.html) ends up on the login page. Registered LAST so
        // the specific routes above take precedence.
        { .uri = "/*",           .method = HTTP_GET,  .handler = index_get_handler    },
    };
    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); i++) {
        httpd_register_uri_handler(server, &routes[i]);
    }
}

esp_err_t ui_controller_init(sip_client_handle_t sip_client, app_settings_t *settings) {
    g_sip_client = sip_client;
    g_settings = settings;
    ESP_LOGI(TAG, "Initializing UI Controller");
#ifdef CTRL_METHOD_BUTTONS
    xTaskCreate(button_task, "button_task", 2048, NULL, 5, NULL);
#endif

    // Always start webserver for Captive Portal or Web Control
    start_webserver();

#ifdef CTRL_METHOD_AUTO
    ESP_LOGI(TAG, "Auto-answer enabled. Handled by SIP Client.");
#endif
    return ESP_OK;
}

void ui_controller_wake_word_trigger(void) {
    if (g_sip_client) {
        sip_call_state_t st = sip_client_get_call_state(g_sip_client);
        if (st == SIP_CALL_STATE_IDLE) {
            ESP_LOGI(TAG, "Wake word triggered! Initiating call...");
            sip_client_initiate_call(g_sip_client, call_target());
        }
    }
}
