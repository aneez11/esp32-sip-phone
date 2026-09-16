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

// ---- runtime device role (chosen on the Settings page) ----
static bool speaker_mode(void) {
    return g_settings && g_settings->device_role == DEVICE_ROLE_SPEAKER;
}

static const char *role_name(void) {
    return speaker_mode() ? "Speaker" : "Phone";
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

#define PAGE_MIN_CAP 1024
#define PAGE_MAX_CAP 24576

typedef struct {
    char  *buf;
    size_t cap;
    size_t len;
    bool   ok;
} page_t;

static void pg_init(page_t *p, size_t cap) {
    if (cap < PAGE_MIN_CAP) cap = PAGE_MIN_CAP;
    p->buf = calloc(1, cap);
    p->cap = p->buf ? cap : 0;
    p->len = 0;
    p->ok  = (p->buf != NULL);
}

// Grow the buffer only as far as the page actually needs, so short pages stay
// cheap on the device heap and long ones cannot be truncated.
static bool pg_reserve(page_t *p, size_t extra) {
    if (p->cap - p->len > extra) return true;
    size_t cap = p->cap ? p->cap : PAGE_MIN_CAP;
    while (cap - p->len <= extra) {
        if (cap >= PAGE_MAX_CAP) return false;
        cap = (cap * 2 > PAGE_MAX_CAP) ? PAGE_MAX_CAP : cap * 2;
    }
    char *grown = realloc(p->buf, cap);
    if (!grown) return false;
    p->buf = grown;
    p->cap = cap;
    return true;
}

static void pg_printf(page_t *p, const char *fmt, ...) {
    if (!p->ok) return;

    va_list ap, ap2;
    va_start(ap, fmt);
    va_copy(ap2, ap);
    int need = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);

    if (need < 0 || !pg_reserve(p, (size_t)need + 1)) {
        va_end(ap2);
        p->ok = false;
        return;
    }
    vsnprintf(p->buf + p->len, p->cap - p->len, fmt, ap2);
    va_end(ap2);
    p->len += (size_t)need;
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

// Single small stylesheet for every page: mobile first, two columns on wider
// screens, no external fonts or assets (the device serves it for every request).
static const char PAGE_CSS[] =
    ":root{color-scheme:dark;}*{box-sizing:border-box;}"
    "body{margin:0;min-height:100vh;padding:16px;display:flex;justify-content:center;"
    "font-family:\"Segoe UI\",system-ui,-apple-system,Roboto,Helvetica,Arial,sans-serif;"
    "color:#eaf4fb;background:linear-gradient(135deg,#0f2027,#203a43,#2c5364);}"
    ".wrap{width:100%;max-width:860px;}"
    ".grid{display:grid;gap:14px;align-items:start;}"
    ".grid>.wide{grid-column:1/-1;}"
    "@media(min-width:760px){.grid{grid-template-columns:1fr 1fr;}}"
    ".card{background:rgba(255,255,255,.07);border:1px solid rgba(255,255,255,.14);"
    "border-radius:16px;padding:20px;box-shadow:0 8px 32px rgba(0,0,0,.32);margin:0 0 14px;}"
    ".grid>.card{margin:0;}"
    "h1{font-size:21px;margin:0 0 4px;letter-spacing:.3px;}"
    "h2{font-size:12.5px;text-transform:uppercase;letter-spacing:1.1px;color:#8fd6ef;margin:0 0 4px;}"
    "h3{font-size:14px;margin:0 0 4px;color:#dff0fa;}"
    ".sub{font-size:12.5px;color:#9fb3c6;margin:0 0 14px;line-height:1.5;}"
    ".hint{display:block;font-size:11.5px;line-height:1.4;color:#8fa6ba;margin:5px 0 0;}"
    "label{display:block;font-size:11.5px;letter-spacing:.4px;text-transform:uppercase;color:#a8bccd;margin:0 0 5px;}"
    "input,select{width:100%;padding:10px 11px;border-radius:9px;font-size:14.5px;color:#fff;"
    "border:1px solid rgba(255,255,255,.14);background:rgba(255,255,255,.06);}"
    "input::placeholder{color:#7d90a3;}"
    "input:focus,select:focus{outline:none;border-color:#00d2ff;box-shadow:0 0 10px rgba(0,210,255,.4);}"
    ".fields{display:grid;gap:12px;}"
    ".fields.pins{grid-template-columns:1fr 1fr;}"
    "@media(min-width:540px){.fields{grid-template-columns:repeat(auto-fit,minmax(170px,1fr));}}"
    ".field{min-width:0;}"
    ".btn{display:block;width:100%;padding:12px;border:0;border-radius:10px;color:#fff;font-size:14.5px;"
    "font-weight:600;text-align:center;text-decoration:none;cursor:pointer;"
    "background:linear-gradient(90deg,#00d2ff,#3a7bd5);}"
    ".btn:hover{filter:brightness(1.08);}"
    ".btn:disabled{opacity:.38;cursor:not-allowed;filter:none;}"
    ".btn.call{background:linear-gradient(90deg,#11998e,#38ef7d);}"
    ".btn.answer{background:linear-gradient(90deg,#2193b0,#6dd5ed);}"
    ".btn.hangup{background:linear-gradient(90deg,#cb2d3e,#ef473a);}"
    ".actions{display:flex;gap:10px;}.actions form{flex:1;}"
    ".savebar{position:sticky;bottom:0;z-index:5;display:flex;flex-wrap:wrap;gap:12px;align-items:center;"
    "background:rgba(11,26,33,.94);border:1px solid rgba(255,255,255,.16);border-radius:14px;"
    "padding:12px 14px;margin-top:2px;box-shadow:0 -6px 24px rgba(0,0,0,.35);}"
    ".savebar .btn{margin:0;flex:1;min-width:150px;}"
    ".savebar .hint{margin:0;flex:1.5;min-width:170px;}"
    ".nav{display:flex;flex-wrap:wrap;gap:8px;align-items:center;justify-content:center;margin-bottom:14px;}"
    ".nav a{color:#cfe9f5;text-decoration:none;font-size:12.5px;padding:7px 12px;border-radius:999px;"
    "background:rgba(255,255,255,.07);border:1px solid rgba(255,255,255,.12);}"
    ".nav a.on{background:linear-gradient(90deg,#00d2ff,#3a7bd5);border-color:transparent;color:#04121c;font-weight:700;}"
    ".navmode{font-size:10.5px;font-weight:700;letter-spacing:.6px;text-transform:uppercase;padding:7px 11px;"
    "border-radius:999px;border:1px solid rgba(255,255,255,.16);background:rgba(255,255,255,.07);color:#cfe9f5;}"
    ".navmode.ok{color:#8bf3b6;}.navmode.offline{color:#ffb3b8;}.navmode.wait{color:#ffd28a;}"
    ".navmode.ring,.navmode.call,.navmode.setup{color:#9fe8ff;}"
    ".banner{border-radius:16px;padding:18px;border:1px solid rgba(255,255,255,.16);"
    "box-shadow:0 8px 32px rgba(0,0,0,.3);}"
    ".banner h1{margin:0 0 6px;}"
    ".banner p{margin:0;font-size:13px;line-height:1.5;color:#e6f0f8;opacity:.93;}"
    ".banner.setup{background:linear-gradient(120deg,rgba(0,210,255,.22),rgba(58,123,213,.18));}"
    ".banner.offline{background:linear-gradient(120deg,rgba(203,45,62,.3),rgba(239,71,58,.16));}"
    ".banner.wait{background:linear-gradient(120deg,rgba(255,184,0,.24),rgba(255,140,0,.14));}"
    ".banner.ok{background:linear-gradient(120deg,rgba(17,153,142,.3),rgba(56,239,125,.18));}"
    ".banner.call{background:linear-gradient(120deg,rgba(0,210,255,.24),rgba(58,123,213,.2));}"
    ".banner.ring{background:linear-gradient(120deg,rgba(33,147,176,.36),rgba(109,213,237,.22));"
    "animation:pulse 1.5s ease-in-out infinite;}"
    "@keyframes pulse{0%,100%{box-shadow:0 8px 32px rgba(0,0,0,.3);}"
    "50%{box-shadow:0 0 24px rgba(0,210,255,.55);}}"
    ".pill{display:inline-block;font-size:11.5px;padding:4px 10px;border-radius:999px;"
    "background:rgba(255,255,255,.1);margin:0 4px 6px 0;color:#cfe9f5;}"
    ".pill.good{background:rgba(56,239,125,.18);color:#8bf3b6;}"
    ".pill.bad{background:rgba(203,45,62,.22);color:#ffb3b8;}"
    ".err{background:rgba(203,45,62,.2);border:1px solid #cb2d3e;border-radius:9px;"
    "padding:10px 12px;font-size:12.5px;margin:0 0 12px;}"
    ".ok{background:rgba(56,239,125,.14);border:1px solid #38ef7d;border-radius:9px;"
    "padding:10px 12px;font-size:12.5px;}"
    ".info{background:rgba(0,210,255,.1);border:1px solid rgba(0,210,255,.45);border-radius:9px;"
    "padding:10px 12px;font-size:12.5px;line-height:1.5;margin-bottom:12px;}"
    ".warn{background:rgba(255,184,0,.12);border:1px solid rgba(255,184,0,.45);border-radius:9px;"
    "padding:10px 12px;font-size:12.5px;line-height:1.5;margin-bottom:12px;}"
    "table{width:100%;border-collapse:collapse;font-size:13.5px;}"
    "td{padding:9px 4px;border-bottom:1px solid rgba(255,255,255,.09);vertical-align:middle;}"
    ".del{background:none;border:1px solid rgba(255,255,255,.2);color:#ff9aa2;border-radius:8px;"
    "padding:5px 10px;cursor:pointer;font-size:12px;}"
    ".foot{text-align:center;font-size:11.5px;color:#8ea4b8;padding:12px 0 2px;}"
    "a.link{color:#7fe3ff;font-size:12.5px;text-decoration:none;}";

static void pg_head(page_t *p, const char *title, int refresh_s) {
    pg_puts(p, "<!DOCTYPE html><html lang='en'><head><meta charset='UTF-8'>"
               "<meta name='viewport' content='width=device-width,initial-scale=1'>");
    if (refresh_s > 0) pg_printf(p, "<meta http-equiv='refresh' content='%d'>", refresh_s);
    pg_printf(p, "<title>%s</title><style>%s</style></head><body><div class='wrap'>", title, PAGE_CSS);
}

// ---- form field helpers: label + input + inline help ----
static void field_label(page_t *p, const char *name, const char *label) {
    pg_puts(p, "<div class='field'><label for='");
    pg_puts(p, name);
    pg_puts(p, "'>");
    pg_puts(p, label);
    pg_puts(p, "</label>");
}

static void field_end(page_t *p, const char *hint) {
    if (hint) {
        pg_puts(p, "<span class='hint'>");
        pg_puts(p, hint);
        pg_puts(p, "</span>");
    }
    pg_puts(p, "</div>");
}

// type is "text" or "password"; 0/negative maxlen omits the attribute.
static void field_text(page_t *p, const char *name, const char *label, const char *value,
                       const char *placeholder, const char *type, int maxlen, const char *hint) {
    field_label(p, name, label);
    pg_puts(p, "<input id='");
    pg_puts(p, name);
    pg_puts(p, "' name='");
    pg_puts(p, name);
    pg_printf(p, "' type='%s'", type);
    if (maxlen > 0) pg_printf(p, " maxlength='%d'", maxlen);
    pg_puts(p, " value='");
    pg_escape(p, value ? value : "");
    pg_puts(p, "' placeholder='");
    pg_escape(p, placeholder ? placeholder : "");
    pg_puts(p, "'>");
    field_end(p, hint);
}

// Password inputs are never pre-filled: empty means "keep the stored value".
static void field_password(page_t *p, const char *name, const char *label, const char *hint) {
    field_label(p, name, label);
    pg_puts(p, "<input id='");
    pg_puts(p, name);
    pg_puts(p, "' name='");
    pg_puts(p, name);
    pg_puts(p, "' type='password' maxlength='63' autocomplete='new-password'"
               " placeholder='Leave empty to keep current'>");
    field_end(p, hint);
}

static void field_select(page_t *p, const char *name, const char *label, const char *const *options,
                         int count, int selected, const char *hint) {
    field_label(p, name, label);
    pg_puts(p, "<select id='");
    pg_puts(p, name);
    pg_puts(p, "' name='");
    pg_puts(p, name);
    pg_puts(p, "'>");
    for (int i = 0; i < count; i++) {
        pg_printf(p, "<option value='%d'%s>%s</option>", i, (i == selected) ? " selected" : "", options[i]);
    }
    pg_puts(p, "</select>");
    field_end(p, hint);
}

static void field_num(page_t *p, const char *name, const char *label, int value,
                      int min, int max, const char *hint) {
    field_label(p, name, label);
    pg_puts(p, "<input id='");
    pg_puts(p, name);
    pg_puts(p, "' name='");
    pg_puts(p, name);
    pg_printf(p, "' type='number' min='%d' max='%d' value='%d'>", min, max, value);
    field_end(p, hint);
}

// Shared page footer: identifies the device and its firmware.
static void pg_device_footer(page_t *p) {
    esp_ip4_addr_t ip = {0};
    char ip_str[16] = "0.0.0.0";
    if (get_my_ip(&ip) == ESP_OK) snprintf(ip_str, sizeof(ip_str), IPSTR, IP2STR(&ip));
    pg_puts(p, "<div class='foot'>ESP32 SIP Voice " APP_VERSION " &middot; ");
    pg_printf(p, "%s", ip_str);
    pg_puts(p, " &middot; ");
    pg_printf(p, "%s", mode_name(device_mode()));
    pg_puts(p, "</div>");
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
    pg_init(&p, PAGE_MIN_CAP);
    pg_head(&p, "Sign in - ESP32 SIP", 0);
    pg_puts(&p, "<div class='card'><h1>ESP32 SIP Voice</h1>"
                "<p class='sub'>Sign in to check the phone status or change its settings.</p>");
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
    pg_puts(&p, "<form method='POST' action='/login'><div class='fields'>"
                "<div class='field'><label for='user'>Username</label>"
                "<input id='user' name='user' type='text' maxlength='31' placeholder='admin' "
                "autocomplete='username' autofocus></div>"
                "<div class='field'><label for='pass'>Password</label>"
                "<input id='pass' name='pass' type='password' maxlength='63' "
                "autocomplete='current-password'></div>"
                "</div><button class='btn' type='submit' style='margin-top:14px'>Sign in"
                "</button></form></div>");
    pg_device_footer(&p);
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
            pg_puts(p, speaker_mode()
                           ? "Registered and idle. Speaker mode: incoming calls are picked up "
                             "automatically."
                           : "Registered and idle - ready to place or receive a call.");
            break;
        case MODE_INCOMING:
            pg_puts(p, "An inbound call is ringing");
            if (remote_uri && remote_uri[0]) {
                pg_puts(p, " from ");
                pg_escape(p, remote_uri);
            }
            if (speaker_mode()) {
                uint8_t delay = g_settings ? g_settings->auto_answer_delay_s : 0;
                if (delay == 0) {
                    pg_puts(p, " - picking it up now (speaker mode).");
                } else {
                    pg_printf(p, " - picking it up in %u s (speaker mode).", (unsigned)delay);
                }
            } else {
                pg_puts(p, ".");
            }
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
    pg_init(&p, PAGE_MIN_CAP);
    pg_head(&p, "ESP32 SIP Phone", (mode == MODE_INCOMING) ? 3 : 5);
    pg_nav(&p, "call");

    pg_puts(&p, "<div class='grid'>");
    pg_puts(&p, "<div class='wide'>");
    render_mode_card(&p, mode, remote);
    pg_puts(&p, "</div>");

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

    pg_puts(&p, "</div>");

    pg_puts(&p, "<div class='card'><h2>Device</h2>"
                "<p class='hint' style='margin:0 0 10px'>Capabilities compiled into this firmware."
                "</p>");
    pg_printf(&p, "<span class='pill'>Mode: %s</span>", role_name());
    if (speaker_mode() && g_settings && g_settings->auto_answer_delay_s > 0) {
        pg_printf(&p, "<span class='pill'>Auto-answer +%u s</span>",
                  (unsigned)g_settings->auto_answer_delay_s);
    }
    pg_printf(&p, "<span class='pill'>%s</span>", profile_name());
    pg_printf(&p, "<span class='pill'>%s</span>", audio_name());
    pg_printf(&p, "<span class='pill'>%s</span>", control_name());
#if defined(USE_WAKE_WORD) && USE_WAKE_WORD
    pg_puts(&p, "<span class='pill good'>Wake word on</span>");
#else
    pg_puts(&p, "<span class='pill'>Wake word off</span>");
#endif
    pg_puts(&p, "</div>");
    pg_puts(&p, "</div>"); // .grid

    pg_device_footer(&p);
    pg_foot(&p);
    send_page(req, &p);
    return ESP_OK;
}

// Settings: Wi-Fi, SIP account and web credentials.
static esp_err_t setup_get_handler(httpd_req_t *req) {
    if (!require_login(req)) return ESP_OK;

    page_t p;
    pg_init(&p, PAGE_MIN_CAP);
    pg_head(&p, "Settings - ESP32 SIP", 0);
    pg_nav(&p, "setup");

    pg_puts(&p, "<h1>Settings</h1>"
                "<p class='sub'>Network and SIP account for this phone. Password fields are never "
                "shown again: leave them empty to keep the stored value.</p>");

    if (wifi_is_ap_mode()) {
        pg_puts(&p, "<div class='info'><b>Setup mode.</b> This device is broadcasting its own open "
                    "access point. Fill in Wi-Fi and the SIP account, save, and it will connect. "
                    "Web login credentials can only be changed once it is on your own network."
                    "</div>");
    }

    pg_puts(&p, "<form method='POST' action='/setup'><div class='grid'>");

    // --- Mode (device role) + audio hardware ---
    static const char *roles[] = {
        "Phone - ring and answer manually",
        "Speaker - answer incoming calls automatically",
    };
    static const char *audio_outs[] = {
        "Auto - detect the I2C codec, else plain I2S",
        "Plain I2S amp - MAX98357A / PCM5102 (no control bus)",
        "I2C codec - ES8388 / ES8311",
    };
    pg_puts(&p, "<div class='card'><h2>Mode &amp; audio</h2>"
                "<p class='hint' style='margin:0 0 12px'>How this device behaves when somebody "
                "calls it, and which audio hardware it drives.</p>"
                "<div class='fields'>");
    field_select(&p, "role", "Device mode", roles, 2, g_settings->device_role,
                 "<b>Phone</b> rings until somebody answers on the keypad, screen or this page. "
                 "<b>Speaker</b> picks up by itself: ideal for paging, intercom and doorbell use.");
    field_num(&p, "auto_answer_delay", "Auto-answer delay (s)",
              g_settings->auto_answer_delay_s, 0, AUTO_ANSWER_DELAY_MAX,
              "Speaker mode only: 0 answers immediately, up to 30 s gives people time to move away "
              "from the speaker.");
    field_select(&p, "audio_out", "Audio output", audio_outs, 3, g_settings->audio_out,
                 "<b>Plain I2S amp</b> for MAX98357A, PCM5102, UDA1334 and similar DACs that have "
                 "no control bus (volume is then scaled in software). <b>I2C codec</b> for ES8388/"
                 "ES8311 modules. <b>Auto</b> picks the codec when I2C pins are wired and it "
                 "answers, otherwise a plain amp. Changing this restarts the device.");
    pg_puts(&p, "</div></div>");

    // --- Wi-Fi ---
    pg_puts(&p, "<div class='card'><h2>Wi-Fi</h2>"
                "<p class='hint' style='margin:0 0 12px'>The network this phone joins. "
                "Changing it restarts the device.</p>"
                "<div class='fields'>");
    field_text(&p, "ssid", "Network name (SSID)", g_settings->wifi_ssid,
               "e.g. HomeNetwork", "text", 31,
               "2.4 GHz network name. The ESP32 cannot join 5 GHz-only networks.");
    field_password(&p, "wifi_pass", "Wi-Fi password",
                   "Stored in NVS on the device. Empty keeps the current one.");
    pg_puts(&p, "</div></div>");

    // --- SIP server ---
    pg_puts(&p, "<div class='card'><h2>SIP server</h2>"
                "<p class='hint' style='margin:0 0 12px'>The PBX or provider this phone registers "
                "with (Asterisk, FreePBX, antisip, ...).</p>"
                "<div class='fields'>");
    field_text(&p, "sip_server", "Server", g_settings->sip_server,
               "192.168.1.100 or sip.provider.com", "text", 63,
               "IP address or hostname of the SIP server.");
    field_num(&p, "sip_port", "Port",
              (int)(g_settings->sip_port ? g_settings->sip_port : SIP_SERVER_PORT), 1, 65535,
              "Usually 5060 (UDP).");
    field_text(&p, "sip_domain", "Domain / realm", g_settings->sip_domain,
               "Defaults to the server", "text", 63,
               "Used in the From/To headers and digest auth. Empty = use the server.");
    pg_puts(&p, "</div></div>");

    // --- SIP account ---
    pg_puts(&p, "<div class='card'><h2>SIP account</h2>"
                "<p class='hint' style='margin:0 0 12px'>The credentials this phone authenticates "
                "with. Saving re-registers immediately, no restart.</p>"
                "<div class='fields'>");
    field_text(&p, "sip_user", "Auth username", g_settings->sip_user, "1000", "text", 63,
               "Extension or account name assigned by the server.");
    field_password(&p, "sip_pass", "Auth password",
                   "Leave empty to keep the stored password.");
    field_text(&p, "display_name", "Display name", g_settings->sip_display_name,
               "ESP32 Phone", "text", 31,
               "Name shown to the other party as the caller.");
    pg_puts(&p, "</div></div>");

    // --- Calling ---
    pg_puts(&p, "<div class='card'><h2>Calling</h2>"
                "<p class='hint' style='margin:0 0 12px'>What the physical button, the wake word "
                "and the web <b>Call</b> button dial.</p>"
                "<div class='fields'>");
    field_text(&p, "sip_target", "Default call target", g_settings->sip_target,
               "sip:1001@192.168.1.100", "text", 63,
               "Full SIP URI, e.g. sip:1001@192.168.1.100 or sip:reception@provider.com.");
    pg_puts(&p, "</div></div>");

    // --- Web access ---
    if (!wifi_is_ap_mode()) {
        pg_puts(&p, "<div class='card'><h2>Web access</h2>"
                    "<p class='hint' style='margin:0 0 12px'>Credentials for this web interface. "
                    "Changing them does not sign you out here.</p>"
                    "<div class='fields'>");
        field_text(&p, "web_user", "Login username", g_settings->web_user, "admin", "text", 31,
                   "Used together with the login password on the sign-in page.");
        field_password(&p, "web_pass", "Login password",
                       "Change the factory default as soon as the device is on your network.");
        pg_puts(&p, "</div></div>");
    }

    // --- Save ---
    pg_puts(&p, "<div class='wide'><div class='savebar'>"
                "<button class='btn' type='submit'>Save settings</button>"
                "<span class='hint'>SIP account and login changes apply immediately. Wi-Fi changes "
                "restart the device.</span></div></div>");
    pg_puts(&p, "</div></form>"); // .grid

    pg_device_footer(&p);
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
    bool mode_changed = false, audio_changed = false, too_long = false;

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

    // Device role: affects the next incoming call, no restart.
    if (form_get(buf, "role", v, sizeof(v)) && v[0]) {
        int role = atoi(v);
        if (role >= DEVICE_ROLE_PHONE && role <= DEVICE_ROLE_SPEAKER &&
            (uint8_t)role != updated.device_role) {
            updated.device_role = (uint8_t)role;
            mode_changed = true;
        }
    }
    if (form_get(buf, "auto_answer_delay", v, sizeof(v)) && v[0]) {
        int delay = atoi(v);
        if (delay >= 0 && delay <= AUTO_ANSWER_DELAY_MAX &&
            (uint8_t)delay != updated.auto_answer_delay_s) {
            updated.auto_answer_delay_s = (uint8_t)delay;
            mode_changed = true;
        }
    }

    // Audio backend: needs a fresh codec/I2S init, so this one restarts too.
    if (form_get(buf, "audio_out", v, sizeof(v)) && v[0]) {
        int ao = atoi(v);
        if (ao >= AUDIO_OUT_AUTO && ao <= AUDIO_OUT_ES8388 && (uint8_t)ao != updated.audio_out) {
            updated.audio_out = (uint8_t)ao;
            audio_changed = true;
        }
    }

    bool changed = wifi_changed || sip_changed || web_changed || mode_changed || audio_changed;

    if (too_long) {
        page_t p;
        pg_init(&p, PAGE_MIN_CAP);
        pg_head(&p, "Settings - ESP32 SIP", 0);
        pg_nav(&p, "setup");
        pg_puts(&p, "<div class='card'><h1>Value too long</h1>"
                    "<p class='sub'>One of the values you entered does not fit its field, so "
                    "nothing was saved. Shorten it and try again: usernames, passwords, server "
                    "and call target allow 63 characters, the SSID and display name 31.</p>"
                    "<a class='link' href='/setup'>&laquo; Back to settings</a></div>");
        pg_device_footer(&p);
        pg_foot(&p);
        send_page(req, &p);
        free(buf);
        return ESP_OK;
    }
    if (!changed) {
        page_t p;
        pg_init(&p, PAGE_MIN_CAP);
        pg_head(&p, "Settings - ESP32 SIP", 0);
        pg_nav(&p, "setup");
        pg_puts(&p, "<div class='card'><h1>No changes</h1>"
                    "<p class='sub'>Everything you submitted already matches the saved "
                    "configuration, so nothing was written and the device keeps running.</p>"
                    "<a class='link' href='/setup'>&laquo; Back to settings</a></div>");
        pg_device_footer(&p);
        pg_foot(&p);
        send_page(req, &p);
        free(buf);
        return ESP_OK;
    }

    *g_settings = updated;
    config_manager_save(g_settings);
    ESP_LOGI(TAG, "Settings updated from web UI (sip_server='%s', sip_user='%s')",
             g_settings->sip_server, g_settings->sip_user);

    // Wi-Fi needs a fresh association, the audio backend a fresh codec/I2S init,
    // and the AP/setup path has no SIP client, so those still restart.
    // SIP account, device mode and web credentials apply in place.
    bool need_reboot = wifi_changed || audio_changed || wifi_is_ap_mode();
    bool sip_applied = false;
    if (!need_reboot && sip_changed && g_sip_client) {
        sip_applied = (sip_client_reload(g_sip_client) == ESP_OK);
    }

    page_t p;
    if (need_reboot) {
        pg_init(&p, PAGE_MIN_CAP);
        pg_head(&p, "Restarting - ESP32 SIP", 0);
        pg_nav(&p, "setup");
        pg_puts(&p, "<div class='card'><h1>Settings saved</h1>"
                    "<p class='sub'>The network settings changed, so the device restarts to join "
                    "the new Wi-Fi. This page can be closed; reconnect to the device on its new "
                    "address once it is back.</p>"
                    "<div class='ok'>Rebooting...</div></div>");
        pg_foot(&p);
        send_page(req, &p);
        free(buf);
        vTaskDelay(pdMS_TO_TICKS(1500));
        esp_restart();
        return ESP_OK;
    }

    pg_init(&p, PAGE_MIN_CAP);
    pg_head(&p, "Settings - ESP32 SIP", 0);
    pg_nav(&p, "setup");
    pg_puts(&p, "<div class='card'><h1>Settings saved</h1>");
    if (mode_changed) {
        pg_puts(&p, "<p class='sub'>Device mode updated: ");
        pg_escape(&p, role_name());
        pg_puts(&p, ". It applies to the next incoming call.</p>");
    }
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
                "&laquo; Back to phone status</a></div>");
    pg_device_footer(&p);
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
    pg_init(&p, PAGE_MIN_CAP);
    pg_head(&p, "Phonebook - ESP32 SIP", 0);
    pg_nav(&p, "phonebook");
    pg_puts(&p, "<h1>Phonebook</h1>"
                "<p class='sub'>Speed dial slots 0-9, stored in NVS on the device. The physical "
                "button and the keypad dial the entry matching the slot number.</p>"
                "<div class='grid'>");

    pg_puts(&p, "<div class='card'><h2>Saved entries</h2>");
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
    if (!any) pg_puts(&p, "<p class='sub' style='margin:0'>No entries saved yet.</p>");
    pg_puts(&p, "</div>");

    pg_puts(&p, "<div class='card'><h2>Add or replace</h2>"
                "<p class='hint' style='margin:0 0 12px'>Saving into an occupied slot overwrites "
                "that entry.</p>"
                "<form method='POST' action='/pb_add'><div class='fields'>");
    field_text(&p, "name", "Name", "", "Front door", "text", MAX_NAME_LEN - 1,
               "Shown on the screen and in this list.");
    field_text(&p, "uri", "SIP URI", "", "sip:1000@192.168.1.10", "text", MAX_URI_LEN - 1,
               "Full SIP URI to dial for this contact.");
    field_num(&p, "id", "Speed dial slot", 0, 0, MAX_PHONEBOOK_ENTRIES - 1,
              "Slot number 0-9.");
    pg_puts(&p, "</div><button class='btn' type='submit' style='margin-top:14px'>Save entry"
                "</button></form></div>");
    pg_puts(&p, "</div>"); // .grid

    pg_device_footer(&p);
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

// One editable GPIO: label, number input and a short explanation.
static void hw_pin_field(page_t *p, const char *name, const char *label, int8_t value, const char *hint) {
    field_num(p, name, label, value, -1, 48, hint);
}

static esp_err_t hardware_get_handler(httpd_req_t *req) {
    if (!require_login(req)) return ESP_OK;

    hardware_settings_t hw;
    config_manager_load_hw(&hw);

    page_t p;
    pg_init(&p, PAGE_MIN_CAP);
    pg_head(&p, "Hardware - ESP32 SIP", 0);
    pg_nav(&p, "hardware");
    pg_puts(&p, "<h1>Hardware</h1>"
                "<p class='sub'>GPIO wiring for this board. Every value is a GPIO number; use "
                "<b>-1</b> for anything that is not wired. Saving restarts the device.</p>"
                "<div class='warn'>Wrong pins leave the display or the audio silent. Change one "
                "group at a time and check the serial log if a peripheral stops working. Blank "
                "fields keep the stored value.</div>"
                "<form method='POST' action='/hardware'><div class='grid'>");

    pg_puts(&p, "<div class='card'><h2>I2S audio</h2>"
                "<p class='hint' style='margin:0 0 12px'>Digital audio link to the microphone and "
                "speaker or codec (INMP441 + MAX98357A, ES8388, ...).</p>"
                "<div class='fields pins'>");
    hw_pin_field(&p, "bck", "Bit clock (BCLK)", hw.pin_i2s_bck, NULL);
    hw_pin_field(&p, "ws", "Frame clock (LRCLK/WS)", hw.pin_i2s_ws, NULL);
    hw_pin_field(&p, "dout", "Data out (DAC)", hw.pin_i2s_dout, "Audio towards the speaker/codec.");
    hw_pin_field(&p, "din", "Data in (MIC)", hw.pin_i2s_din, "Audio from the microphone.");
    hw_pin_field(&p, "mclk", "Master clock (MCLK)", hw.pin_i2s_mclk,
                 "Only codecs that need MCLK (e.g. ES8388); otherwise -1.");
    pg_puts(&p, "</div></div>");

    pg_puts(&p, "<div class='card'><h2>I2C control</h2>"
                "<p class='hint' style='margin:0 0 12px'>Control bus used by I2C audio codecs "
                "(ES8388/ES8311) and I2C keypads or OLEDs.</p>"
                "<div class='fields pins'>");
    hw_pin_field(&p, "sda", "Data (SDA)", hw.pin_i2c_sda, NULL);
    hw_pin_field(&p, "scl", "Clock (SCL)", hw.pin_i2c_scl, NULL);
    pg_puts(&p, "</div></div>");

    pg_puts(&p, "<div class='card'><h2>Display (SPI)</h2>"
                "<p class='hint' style='margin:0 0 12px'>SPI bus for the ST7789, ILI9341 or "
                "GC9A01 panel.</p>"
                "<div class='fields pins'>");
    hw_pin_field(&p, "spi_mosi", "Data (MOSI)", hw.pin_spi_mosi,
                 "Panel data input - usually the SDI/SDA pad of the module.");
    hw_pin_field(&p, "spi_miso", "Data (MISO)", hw.pin_spi_miso,
                 "Most panels do not drive this line; keep -1.");
    hw_pin_field(&p, "spi_clk", "Clock (SCLK)", hw.pin_spi_clk, NULL);
    hw_pin_field(&p, "tft_cs", "Chip select (CS)", hw.pin_tft_cs, NULL);
    hw_pin_field(&p, "tft_dc", "Data/command (DC)", hw.pin_tft_dc,
                 "Sometimes labelled RS or A0 on the module.");
    hw_pin_field(&p, "tft_rst", "Reset (RST)", hw.pin_tft_rst,
                 "Reset line of the panel; -1 if tied to EN.");
    pg_puts(&p, "</div></div>");

    pg_puts(&p, "<div class='card'><h2>Touch</h2>"
                "<p class='hint' style='margin:0 0 12px'>XPT2046 resistive touch controller "
                "(shares the display SPI bus).</p>"
                "<div class='fields pins'>");
    hw_pin_field(&p, "touch_cs", "Touch CS", hw.pin_touch_cs,
                 "Separate chip select for the touch controller.");
    hw_pin_field(&p, "touch_irq", "Touch IRQ", hw.pin_touch_irq,
                 "Pen-down interrupt; optional (-1 polls instead).");
    pg_puts(&p, "</div></div>");

    pg_puts(&p, "<div class='card wide'><h2>Display theme</h2>"
                "<p class='hint' style='margin:0 0 12px'>Look of the on-device screen. The theme "
                "is drawn by LVGL and applied after the restart.</p><div class='fields'>");
    static const char *themes[] = { "Voice Assistant", "Mobile OS", "Smart Speaker" };
    field_select(&p, "ui_theme", "Theme", themes, 3, hw.ui_theme,
                 "Voice Assistant shows a clock and a glowing orb, Mobile OS looks like a phone "
                 "call screen, Smart Speaker fills the screen with a neon ring.");
    pg_puts(&p, "</div></div>"); // .fields .card

    pg_puts(&p, "<div class='wide'><div class='savebar'>"
                "<button class='btn' type='submit'>Save GPIO map &amp; restart</button>"
                "<span class='hint'>Every value is a GPIO number, -1 means not connected. "
                "The device restarts so the drivers re-initialise.</span></div></div>");
    pg_puts(&p, "</div></form>"); // .grid

    pg_device_footer(&p);
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
    pg_init(&p, PAGE_MIN_CAP);
    pg_head(&p, "Hardware - ESP32 SIP", 0);
    pg_nav(&p, "hardware");
    pg_puts(&p, "<div class='card'><h1>");
    if (changed) {
        pg_puts(&p, "Hardware saved</h1>"
                    "<p class='sub'>The device is restarting so the audio, display and touch "
                    "drivers re-initialise on the new pins.</p>"
                    "<div class='ok'>Rebooting...</div>");
    } else {
        pg_puts(&p, "No changes</h1>"
                    "<p class='sub'>The GPIO map already matched what you submitted, so nothing "
                    "was written and the device keeps running.</p>"
                    "<div class='ok'>No restart required</div>");
    }
    pg_puts(&p, "<a class='link' href='/hardware' style='display:inline-block;margin-top:12px'>"
                "&laquo; Back to hardware</a></div>");
    pg_device_footer(&p);
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
    // Handlers call into the SIP client, which only queues requests; keep some
    // headroom anyway (the stock 4 KB is tight once page building is involved).
    config.stack_size = 6144;

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
