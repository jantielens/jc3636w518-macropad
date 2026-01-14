#include "spotify_manager.h"

#include "board_config.h"

#include "spotify_config.h"
#include "spotify_store.h"

#include "log_manager.h"

#if defined(ARDUINO_ARCH_ESP32)

#include <Arduino.h>
#include <ArduinoJson.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>

#include <ctype.h>
#include <mbedtls/base64.h>
#include <mbedtls/sha256.h>

#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/portmacro.h>

// Decoder header gates on LV_USE_IMG internally.
#if HAS_DISPLAY
  #include "lvgl_jpeg_decoder.h"
#endif

namespace {

// ==== Allocation helpers (prefer PSRAM) ====

static void* alloc_any_8bit(size_t bytes) {
    if (bytes == 0) return nullptr;
    void* p = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM);
    if (p) return p;
    return heap_caps_malloc(bytes, MALLOC_CAP_8BIT);
}

static void free_caps(void* p) {
    if (!p) return;
    heap_caps_free(p);
}

// ==== Small helpers ====

static bool starts_with(const char* s, const char* prefix) {
    if (!s || !prefix) return false;
    while (*prefix) {
        if (*s++ != *prefix++) return false;
    }
    return true;
}

static bool starts_with_ci(const char* s, const char* prefix) {
    if (!s || !prefix) return false;
    while (*prefix) {
        if (!*s) return false;
        if (tolower((unsigned char)*s) != tolower((unsigned char)*prefix)) return false;
        s++;
        prefix++;
    }
    return true;
}

static const char* skip_ws(const char* s) {
    while (s && (*s == ' ' || *s == '\t')) s++;
    return s;
}

static bool contains_ci(const char* haystack, const char* needle) {
    if (!haystack || !needle || !needle[0]) return false;
    const size_t nlen = strlen(needle);
    for (const char* p = haystack; *p; p++) {
        size_t i = 0;
        while (i < nlen && p[i] && tolower((unsigned char)p[i]) == tolower((unsigned char)needle[i])) {
            i++;
        }
        if (i == nlen) return true;
    }
    return false;
}

static bool read_line_bounded(WiFiClientSecure& client, char* out, size_t out_len, unsigned long timeout_ms) {
    if (!out || out_len == 0) return false;
    out[0] = '\0';

    size_t used = 0;
    const unsigned long start = millis();
    while (client.connected()) {
        if (millis() - start > timeout_ms) break;
        if (!client.available()) {
            delay(1);
            continue;
        }

        int c = client.read();
        if (c < 0) {
            delay(1);
            continue;
        }
        if (c == '\n') break;
        if (c == '\r') continue;

        if (used + 1 < out_len) {
            out[used++] = (char)c;
            out[used] = '\0';
        }
    }

    while (used > 0 && (out[used - 1] == ' ' || out[used - 1] == '\t')) {
        out[--used] = '\0';
    }

    return (used > 0) || !client.connected();
}

static void base64url_from_bytes(const uint8_t* in, size_t in_len, char* out, size_t out_len) {
    if (!out || out_len == 0) return;
    out[0] = '\0';

    size_t olen = 0;
    char tmp[128];
    if (in_len > 32) return;

    if (mbedtls_base64_encode((unsigned char*)tmp, sizeof(tmp), &olen, in, in_len) != 0) {
        return;
    }

    size_t j = 0;
    for (size_t i = 0; i < olen && j + 1 < out_len; i++) {
        char c = tmp[i];
        if (c == '+') c = '-';
        else if (c == '/') c = '_';
        else if (c == '=') continue;
        out[j++] = c;
    }
    out[j] = '\0';
}

static void random_urlsafe_string(char* out, size_t out_len, size_t bytes) {
    if (!out || out_len == 0) return;
    out[0] = '\0';

    uint8_t buf[64];
    if (bytes > sizeof(buf)) bytes = sizeof(buf);
    for (size_t i = 0; i < bytes; i++) buf[i] = (uint8_t)esp_random();

    base64url_from_bytes(buf, bytes, out, out_len);
}

static void append_urlencoded(String& out, const char* value) {
    if (!value) return;

    static const char* kHex = "0123456789ABCDEF";
    const unsigned char* p = (const unsigned char*)value;
    while (*p) {
        const unsigned char c = *p++;
        const bool unreserved =
            (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' ||
            c == '.' || c == '~';

        if (unreserved) {
            out += (char)c;
        } else {
            out += '%';
            out += kHex[(c >> 4) & 0xF];
            out += kHex[c & 0xF];
        }
    }
}

static bool parse_https_url(const char* url, char* out_host, size_t host_len, char* out_path, size_t path_len) {
    if (!url || !out_host || !out_path || host_len == 0 || path_len == 0) return false;
    out_host[0] = '\0';
    out_path[0] = '\0';

    const char* p = url;
    if (!starts_with(p, "https://")) return false;
    p += strlen("https://");

    const char* slash = strchr(p, '/');
    if (!slash) {
        strlcpy(out_host, p, host_len);
        strlcpy(out_path, "/", path_len);
        return true;
    }

    const size_t hl = (size_t)(slash - p);
    if (hl == 0 || hl + 1 > host_len) return false;
    memcpy(out_host, p, hl);
    out_host[hl] = '\0';
    strlcpy(out_path, slash, path_len);
    return true;
}

static uint16_t* crop_center_and_scale_square_rgb565(
    const uint16_t* src,
    int src_w,
    int src_h,
    int out_size,
    char* err,
    size_t err_len
) {
    if (err && err_len) err[0] = '\0';
    if (!src || src_w <= 0 || src_h <= 0 || out_size <= 0) {
        if (err && err_len) snprintf(err, err_len, "Bad args");
        return nullptr;
    }

    const int side = (src_w < src_h) ? src_w : src_h;
    const int x0 = (src_w - side) / 2;
    const int y0 = (src_h - side) / 2;

    const size_t out_px = (size_t)out_size * (size_t)out_size;
    uint16_t* out = (uint16_t*)alloc_any_8bit(out_px * 2);
    if (!out) {
        if (err && err_len) snprintf(err, err_len, "OOM");
        return nullptr;
    }

    if (side == 1 || out_size == 1) {
        const uint16_t p = src[(y0 * src_w) + x0];
        for (size_t i = 0; i < out_px; i++) out[i] = p;
        return out;
    }

    // Bilinear resample (fixed-point 16.16), one-time per track.
    // NOTE: Use 64-bit intermediates: (out_size~360, side~640) would overflow 32-bit when <<16.
    const uint32_t denom = (uint32_t)(out_size - 1);
    const uint32_t src_max = (uint32_t)(side - 1);

    for (int oy = 0; oy < out_size; oy++) {
        const uint32_t v = (uint32_t)(((uint64_t)oy * (uint64_t)src_max * 65536ULL) / (uint64_t)denom);
        const int sy = (int)(v >> 16);
        const uint32_t fy = v & 0xFFFF;
        const int sy1 = (sy + 1 < side) ? (sy + 1) : sy;

        for (int ox = 0; ox < out_size; ox++) {
            const uint32_t u = (uint32_t)(((uint64_t)ox * (uint64_t)src_max * 65536ULL) / (uint64_t)denom);
            const int sx = (int)(u >> 16);
            const uint32_t fx = u & 0xFFFF;
            const int sx1 = (sx + 1 < side) ? (sx + 1) : sx;

            const uint16_t p00 = src[(y0 + sy) * src_w + (x0 + sx)];
            const uint16_t p10 = src[(y0 + sy) * src_w + (x0 + sx1)];
            const uint16_t p01 = src[(y0 + sy1) * src_w + (x0 + sx)];
            const uint16_t p11 = src[(y0 + sy1) * src_w + (x0 + sx1)];

            const int r00 = (p00 >> 11) & 0x1F;
            const int g00 = (p00 >> 5) & 0x3F;
            const int b00 = p00 & 0x1F;
            const int r10 = (p10 >> 11) & 0x1F;
            const int g10 = (p10 >> 5) & 0x3F;
            const int b10 = p10 & 0x1F;
            const int r01 = (p01 >> 11) & 0x1F;
            const int g01 = (p01 >> 5) & 0x3F;
            const int b01 = p01 & 0x1F;
            const int r11 = (p11 >> 11) & 0x1F;
            const int g11 = (p11 >> 5) & 0x3F;
            const int b11 = p11 & 0x1F;

            const int r0 = r00 + (int)(((int32_t)(r10 - r00) * (int32_t)fx) >> 16);
            const int g0 = g00 + (int)(((int32_t)(g10 - g00) * (int32_t)fx) >> 16);
            const int b0 = b00 + (int)(((int32_t)(b10 - b00) * (int32_t)fx) >> 16);
            const int r1 = r01 + (int)(((int32_t)(r11 - r01) * (int32_t)fx) >> 16);
            const int g1 = g01 + (int)(((int32_t)(g11 - g01) * (int32_t)fx) >> 16);
            const int b1 = b01 + (int)(((int32_t)(b11 - b01) * (int32_t)fx) >> 16);

            int r = r0 + (int)(((int32_t)(r1 - r0) * (int32_t)fy) >> 16);
            int g = g0 + (int)(((int32_t)(g1 - g0) * (int32_t)fy) >> 16);
            int b = b0 + (int)(((int32_t)(b1 - b0) * (int32_t)fy) >> 16);

            if (r < 0) r = 0;
            if (r > 31) r = 31;
            if (g < 0) g = 0;
            if (g > 63) g = 63;
            if (b < 0) b = 0;
            if (b > 31) b = 31;

            out[(size_t)oy * (size_t)out_size + (size_t)ox] = (uint16_t)((r << 11) | (g << 5) | b);
        }
    }

    return out;
}

// ==== HTTP helpers (avoid heap-heavy String parsing) ====

static bool read_http_response_body_any(
    WiFiClientSecure& client,
    int content_length,
    bool chunked,
    uint8_t** out_buf,
    size_t* out_len,
    char* err,
    size_t err_len
) {
    if (!out_buf || !out_len) return false;
    *out_buf = nullptr;
    *out_len = 0;

    const size_t kMaxBody = 256 * 1024;

    if (content_length >= 0 && !chunked) {
        if ((size_t)content_length > kMaxBody) {
            if (err && err_len) snprintf(err, err_len, "Body too large");
            return false;
        }

        uint8_t* buf = (uint8_t*)alloc_any_8bit((size_t)content_length);
        if (!buf) {
            if (err && err_len) snprintf(err, err_len, "Out of memory");
            return false;
        }

        size_t got = 0;
        const unsigned long start = millis();
        while (got < (size_t)content_length && client.connected()) {
            if (millis() - start > 12000) break;
            if (!client.available()) {
                delay(1);
                continue;
            }
            int n = client.read(buf + got, (size_t)content_length - got);
            if (n > 0) got += (size_t)n;
        }

        if (got != (size_t)content_length) {
            free_caps(buf);
            if (err && err_len) snprintf(err, err_len, "Short body");
            return false;
        }

        *out_buf = buf;
        *out_len = got;
        return true;
    }

    if (chunked) {
        uint8_t* buf = (uint8_t*)alloc_any_8bit(512);
        if (!buf) {
            if (err && err_len) snprintf(err, err_len, "Out of memory");
            return false;
        }

        size_t cap = 512;
        size_t used = 0;
        const unsigned long start = millis();

        while (client.connected()) {
            if (millis() - start > 12000) break;

            char line[64];
            if (!read_line_bounded(client, line, sizeof(line), 3000)) break;
            if (line[0] == '\0') continue;

            const int chunk_size = (int)strtol(line, nullptr, 16);
            if (chunk_size < 0) {
                free_caps(buf);
                if (err && err_len) snprintf(err, err_len, "Bad chunk size");
                return false;
            }

            if (chunk_size == 0) {
                // Consume trailing headers until blank line.
                while (client.connected()) {
                    char trailer[128];
                    if (!read_line_bounded(client, trailer, sizeof(trailer), 3000)) break;
                    if (trailer[0] == '\0') break;
                }
                *out_buf = buf;
                *out_len = used;
                return true;
            }

            if (used + (size_t)chunk_size > kMaxBody) {
                free_caps(buf);
                if (err && err_len) snprintf(err, err_len, "Body too large");
                return false;
            }

            while (used + (size_t)chunk_size > cap) {
                size_t next_cap = cap * 2;
                if (next_cap < 512) next_cap = 512;
                if (next_cap > kMaxBody) next_cap = kMaxBody;
                uint8_t* nb = (uint8_t*)alloc_any_8bit(next_cap);
                if (!nb) {
                    free_caps(buf);
                    if (err && err_len) snprintf(err, err_len, "Out of memory");
                    return false;
                }
                memcpy(nb, buf, used);
                free_caps(buf);
                buf = nb;
                cap = next_cap;
            }

            size_t got = 0;
            while (got < (size_t)chunk_size && client.connected()) {
                if (!client.available()) {
                    delay(1);
                    continue;
                }
                int n = client.read(buf + used + got, (size_t)chunk_size - got);
                if (n > 0) got += (size_t)n;
            }

            if (got != (size_t)chunk_size) {
                free_caps(buf);
                if (err && err_len) snprintf(err, err_len, "Short chunk");
                return false;
            }
            used += got;

            // Consume CRLF after chunk.
            if (client.available()) (void)client.read();
            if (client.available()) (void)client.read();
        }

        free_caps(buf);
        if (err && err_len) snprintf(err, err_len, "Disconnected");
        return false;
    }

    // Fallback: read until close (bounded).
    {
        uint8_t* buf = (uint8_t*)alloc_any_8bit(512);
        if (!buf) {
            if (err && err_len) snprintf(err, err_len, "Out of memory");
            return false;
        }

        size_t cap = 512;
        size_t used = 0;
        const unsigned long start = millis();

        while (client.connected()) {
            if (millis() - start > 12000) break;
            if (!client.available()) {
                delay(1);
                continue;
            }

            if (used == cap) {
                size_t next_cap = cap * 2;
                if (next_cap > kMaxBody) next_cap = kMaxBody;
                if (next_cap == cap) break;

                uint8_t* nb = (uint8_t*)alloc_any_8bit(next_cap);
                if (!nb) {
                    free_caps(buf);
                    if (err && err_len) snprintf(err, err_len, "Out of memory");
                    return false;
                }
                memcpy(nb, buf, used);
                free_caps(buf);
                buf = nb;
                cap = next_cap;
            }

            int n = client.read(buf + used, cap - used);
            if (n > 0) used += (size_t)n;
        }

        if (used == 0) {
            free_caps(buf);
            if (err && err_len) snprintf(err, err_len, "Empty body");
            return false;
        }

        *out_buf = buf;
        *out_len = used;
        return true;
    }
}

static bool http_post_form(
    const char* host,
    const char* path,
    const char* form,
    uint8_t** out_body,
    size_t* out_body_len,
    int* out_status,
    char* err,
    size_t err_len
) {
    if (!host || !path || !form) return false;

    WiFiClientSecure client;
    client.setInsecure();
    if (!client.connect(host, 443)) {
        if (err && err_len) snprintf(err, err_len, "Connect failed");
        return false;
    }

    const size_t form_len = strlen(form);

    client.print("POST ");
    client.print(path);
    client.print(" HTTP/1.1\r\nHost: ");
    client.print(host);
    client.print("\r\nUser-Agent: macropad-poc\r\nConnection: close\r\nContent-Type: application/x-www-form-urlencoded\r\nContent-Length: ");
    client.print((int)form_len);
    client.print("\r\n\r\n");
    client.write((const uint8_t*)form, form_len);

    char statusLine[96];
    (void)read_line_bounded(client, statusLine, sizeof(statusLine), 3000);

    int status = 0;
    if (starts_with_ci(statusLine, "HTTP/")) {
        const char* sp = strchr(statusLine, ' ');
        if (sp) status = atoi(skip_ws(sp + 1));
    }

    int content_length = -1;
    bool chunked = false;
    while (client.connected()) {
        char line[256];
        if (!read_line_bounded(client, line, sizeof(line), 3000)) break;
        if (line[0] == '\0') break;
        if (starts_with_ci(line, "Content-Length:")) {
            content_length = atoi(skip_ws(line + strlen("Content-Length:")));
        } else if (starts_with_ci(line, "Transfer-Encoding:")) {
            if (contains_ci(line, "chunked")) chunked = true;
        }
    }

    if (out_status) *out_status = status;

    uint8_t* body = nullptr;
    size_t body_len = 0;
    if (!read_http_response_body_any(client, content_length, chunked, &body, &body_len, err, err_len)) {
        return false;
    }

    if (out_body) *out_body = body;
    if (out_body_len) *out_body_len = body_len;
    return true;
}

static bool http_get(
    const char* host,
    const char* path,
    const char* bearer,
    uint8_t** out_body,
    size_t* out_body_len,
    int* out_status,
    int* out_content_length,
    char* err,
    size_t err_len
) {
    if (!host || !path) return false;

    WiFiClientSecure client;
    client.setInsecure();
    if (!client.connect(host, 443)) {
        if (err && err_len) snprintf(err, err_len, "Connect failed");
        return false;
    }

    client.print("GET ");
    client.print(path);
    client.print(" HTTP/1.1\r\nHost: ");
    client.print(host);
    client.print("\r\nUser-Agent: macropad-poc\r\nConnection: close\r\n");
    if (bearer && bearer[0]) {
        client.print("Authorization: Bearer ");
        client.print(bearer);
        client.print("\r\n");
    }
    client.print("\r\n");

    char statusLine[96];
    (void)read_line_bounded(client, statusLine, sizeof(statusLine), 3000);

    int status = 0;
    if (starts_with_ci(statusLine, "HTTP/")) {
        const char* sp = strchr(statusLine, ' ');
        if (sp) status = atoi(skip_ws(sp + 1));
    }

    int content_length = -1;
    bool chunked = false;
    while (client.connected()) {
        char line[256];
        if (!read_line_bounded(client, line, sizeof(line), 3000)) break;
        if (line[0] == '\0') break;
        if (starts_with_ci(line, "Content-Length:")) {
            content_length = atoi(skip_ws(line + strlen("Content-Length:")));
        } else if (starts_with_ci(line, "Transfer-Encoding:")) {
            if (contains_ci(line, "chunked")) chunked = true;
        }
    }

    if (out_status) *out_status = status;
    if (out_content_length) *out_content_length = content_length;

    uint8_t* body = nullptr;
    size_t body_len = 0;
    if (!read_http_response_body_any(client, content_length, chunked, &body, &body_len, err, err_len)) {
        return false;
    }

    if (out_body) *out_body = body;
    if (out_body_len) *out_body_len = body_len;
    return true;
}

static bool http_post_no_body(const char* host, const char* path, const char* bearer, int* out_status) {
    if (!host || !path) return false;

    WiFiClientSecure client;
    client.setInsecure();
    if (!client.connect(host, 443)) return false;

    client.print("POST ");
    client.print(path);
    client.print(" HTTP/1.1\r\nHost: ");
    client.print(host);
    client.print("\r\nUser-Agent: macropad-poc\r\nConnection: close\r\nContent-Length: 0\r\n");
    if (bearer && bearer[0]) {
        client.print("Authorization: Bearer ");
        client.print(bearer);
        client.print("\r\n");
    }
    client.print("\r\n");

    char statusLine[96];
    (void)read_line_bounded(client, statusLine, sizeof(statusLine), 3000);
    int status = 0;
    if (starts_with_ci(statusLine, "HTTP/")) {
        const char* sp = strchr(statusLine, ' ');
        if (sp) status = atoi(skip_ws(sp + 1));
    }
    if (out_status) *out_status = status;
    return true;
}

// ==== Manager state ====

static bool s_active = false;

static char s_pkce_state[64] = {0};
static char s_pkce_verifier[96] = {0};
static unsigned long s_pkce_started_ms = 0;

static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static SpotifyNowPlaying s_now;

static SpotifyImage s_img;
static char s_img_track_id[96] = {0};

static char s_access_token[256] = {0};
static unsigned long s_access_token_expires_ms = 0;

static bool s_pending_complete = false;
static char s_pending_code[1024] = {0};
static char s_pending_state[64] = {0};

static bool s_pending_prev = false;
static bool s_pending_next = false;

static unsigned long s_last_poll_ms = 0;

static char s_last_art_err_track_id[96] = {0};
static char s_last_art_ok_track_id[96] = {0};
static char s_last_art_skip_track_id[96] = {0};

static void clear_image_locked() {
    if (s_img.pixels) {
        free_caps(s_img.pixels);
        s_img.pixels = nullptr;
        s_img.w = 0;
        s_img.h = 0;
        s_img_track_id[0] = '\0';
    }
}

static bool ensure_access_token(char* err, size_t err_len) {
    const unsigned long now = millis();
    if (s_access_token[0] && now + 5000 < s_access_token_expires_ms) {
        return true;
    }

    if (!spotify_store::has_refresh_token()) {
        if (err && err_len) snprintf(err, err_len, "No refresh token");
        return false;
    }

    char refresh[256];
    if (!spotify_store::load_refresh_token(refresh, sizeof(refresh))) {
        if (err && err_len) snprintf(err, err_len, "Refresh token load failed");
        return false;
    }

    String form;
    form.reserve(768);
    form += "grant_type=refresh_token";
    form += "&client_id=";
    append_urlencoded(form, SPOTIFY_CLIENT_ID);
    form += "&refresh_token=";
    append_urlencoded(form, refresh);

    uint8_t* body = nullptr;
    size_t body_len = 0;
    int status = 0;
    char herr[96];
    if (!http_post_form("accounts.spotify.com", "/api/token", form.c_str(), &body, &body_len, &status, herr, sizeof(herr))) {
        if (err && err_len) snprintf(err, err_len, "%s", herr);
        return false;
    }

    if (status < 200 || status >= 300) {
        free_caps(body);
        if (err && err_len) snprintf(err, err_len, "Token HTTP %d", status);
        return false;
    }

    StaticJsonDocument<1024> doc;
    DeserializationError jerr = deserializeJson(doc, body, body_len);
    free_caps(body);
    if (jerr) {
        if (err && err_len) snprintf(err, err_len, "Token JSON parse failed");
        return false;
    }

    const char* access = doc["access_token"] | "";
    const int expires_in = doc["expires_in"] | 0;
    if (!access || access[0] == '\0' || expires_in <= 0) {
        if (err && err_len) snprintf(err, err_len, "Token response missing fields");
        return false;
    }

    strlcpy(s_access_token, access, sizeof(s_access_token));
    s_access_token_expires_ms = millis() + (unsigned long)expires_in * 1000UL;
    return true;
}

static void parse_now_playing_json(const uint8_t* body, size_t body_len) {
    StaticJsonDocument<4096> doc;
    DeserializationError jerr = deserializeJson(doc, body, body_len);
    if (jerr) return;

    SpotifyNowPlaying next;
    next.valid = true;
    next.is_playing = doc["is_playing"] | false;

    const char* track = doc["item"]["name"] | "";
    const char* track_id = doc["item"]["id"] | "";

    const char* artist = "";
    JsonArray artists = doc["item"]["artists"].as<JsonArray>();
    if (!artists.isNull() && artists.size() > 0) {
        artist = artists[0]["name"] | "";
    }

    strlcpy(next.track_name, track ? track : "", sizeof(next.track_name));
    strlcpy(next.artist_name, artist ? artist : "", sizeof(next.artist_name));
    strlcpy(next.track_id, track_id ? track_id : "", sizeof(next.track_id));

    const char* best_url = "";
    int best_w = 0;
    JsonArray images = doc["item"]["album"]["images"].as<JsonArray>();
    if (!images.isNull()) {
        // Prefer a higher-res source so the final 360x360 downscale is crisp.
        // Typical Spotify sizes are 640 / 300 / 64.
        for (JsonObject img : images) {
            const int w = img["width"] | 0;
            const char* url = img["url"] | "";
            if (!url || !url[0]) continue;
            if (w >= 640) {
                if (best_w == 0 || w < best_w) {
                    best_w = w;
                    best_url = url;
                }
            }
        }

        if (!best_url[0]) {
            // Fallback: pick the largest available.
            for (JsonObject img : images) {
                const int w = img["width"] | 0;
                const char* url = img["url"] | "";
                if (!url || !url[0]) continue;
                if (w > best_w) {
                    best_w = w;
                    best_url = url;
                }
            }
        }
    }
    strlcpy(next.art_url, best_url ? best_url : "", sizeof(next.art_url));

    portENTER_CRITICAL(&s_mux);
    s_now = next;
    portEXIT_CRITICAL(&s_mux);
}

static void maybe_update_album_art() {
#if HAS_DISPLAY && LV_USE_IMG
    SpotifyNowPlaying snap;
    portENTER_CRITICAL(&s_mux);
    snap = s_now;
    portEXIT_CRITICAL(&s_mux);

    if (!snap.valid) return;
    if (snap.track_id[0] == '\0') return;

    if (snap.art_url[0] == '\0') {
        if (strncmp(s_last_art_skip_track_id, snap.track_id, sizeof(s_last_art_skip_track_id)) != 0) {
            strlcpy(s_last_art_skip_track_id, snap.track_id, sizeof(s_last_art_skip_track_id));
            Logger.logMessage("Spotify", "Album art: no art_url in now-playing payload");
        }
        return;
    }

    if (strncmp(s_img_track_id, snap.track_id, sizeof(s_img_track_id)) == 0) {
        return;
    }

    char host[192];
    char path[512];
    if (!parse_https_url(snap.art_url, host, sizeof(host), path, sizeof(path))) {
        if (strncmp(s_last_art_err_track_id, snap.track_id, sizeof(s_last_art_err_track_id)) != 0) {
            strlcpy(s_last_art_err_track_id, snap.track_id, sizeof(s_last_art_err_track_id));
            Logger.logMessage("Spotify", "Album art URL parse failed");
        }
        return;
    }

    if (strncmp(s_last_art_skip_track_id, snap.track_id, sizeof(s_last_art_skip_track_id)) != 0) {
        strlcpy(s_last_art_skip_track_id, snap.track_id, sizeof(s_last_art_skip_track_id));
        Logger.logMessagef("Spotify", "Album art: downloading from %s%s", host, path);
    }

    char err[96];
    int status = 0;
    int content_length = 0;
    uint8_t* jpeg = nullptr;
    size_t jpeg_len = 0;

    if (!http_get(host, path, nullptr, &jpeg, &jpeg_len, &status, &content_length, err, sizeof(err))) {
        if (strncmp(s_last_art_err_track_id, snap.track_id, sizeof(s_last_art_err_track_id)) != 0) {
            strlcpy(s_last_art_err_track_id, snap.track_id, sizeof(s_last_art_err_track_id));
            Logger.logMessagef("Spotify", "Album art download failed: %s", err);
        }
        return;
    }

    if (status < 200 || status >= 300) {
        free_caps(jpeg);
        if (strncmp(s_last_art_err_track_id, snap.track_id, sizeof(s_last_art_err_track_id)) != 0) {
            strlcpy(s_last_art_err_track_id, snap.track_id, sizeof(s_last_art_err_track_id));
            Logger.logMessagef("Spotify", "Album art HTTP %d", status);
        }
        return;
    }

    if (strncmp(s_last_art_ok_track_id, snap.track_id, sizeof(s_last_art_ok_track_id)) != 0) {
        Logger.logMessagef(
            "Spotify",
            "Album art: downloaded %uB (Content-Length=%d)",
            (unsigned)jpeg_len,
            content_length
        );
    }

    uint16_t* pixels = nullptr;
    int w = 0, h = 0;
    int scale_used = -1;
    char derr[64];
    const bool ok = lvgl_jpeg_decode_to_rgb565(jpeg, jpeg_len, &pixels, &w, &h, &scale_used, derr, sizeof(derr));
    free_caps(jpeg);

    if (!ok || !pixels) {
        if (strncmp(s_last_art_err_track_id, snap.track_id, sizeof(s_last_art_err_track_id)) != 0) {
            strlcpy(s_last_art_err_track_id, snap.track_id, sizeof(s_last_art_err_track_id));
            Logger.logMessagef("Spotify", "Album art decode failed (%uB): %s", (unsigned)jpeg_len, derr);
        }
        return;
    }

    // Resize to exactly 360x360 so the UI can render it as full-screen art.
    // Crop to center square before scaling.
    const int kTarget = 360;
    if (w != kTarget || h != kTarget) {
        char serr[64];
        uint16_t* scaled = crop_center_and_scale_square_rgb565(pixels, w, h, kTarget, serr, sizeof(serr));
        free_caps(pixels);
        pixels = nullptr;
        if (!scaled) {
            if (strncmp(s_last_art_err_track_id, snap.track_id, sizeof(s_last_art_err_track_id)) != 0) {
                strlcpy(s_last_art_err_track_id, snap.track_id, sizeof(s_last_art_err_track_id));
                Logger.logMessagef("Spotify", "Album art scale failed (%dx%d -> %dx%d): %s", w, h, kTarget, kTarget, serr);
            }
            return;
        }
        pixels = scaled;
        w = kTarget;
        h = kTarget;
    }

    if (strncmp(s_last_art_ok_track_id, snap.track_id, sizeof(s_last_art_ok_track_id)) != 0) {
        strlcpy(s_last_art_ok_track_id, snap.track_id, sizeof(s_last_art_ok_track_id));
        Logger.logMessagef("Spotify", "Album art: ready %dx%d (jpeg scale=%d)", w, h, scale_used);
    }

    portENTER_CRITICAL(&s_mux);
    clear_image_locked();
    s_img.pixels = pixels;
    s_img.w = w;
    s_img.h = h;
    strlcpy(s_img_track_id, snap.track_id, sizeof(s_img_track_id));
    s_last_art_err_track_id[0] = '\0';
    portEXIT_CRITICAL(&s_mux);
#endif
}

static void poll_now_playing() {
    char err[96];
    if (!ensure_access_token(err, sizeof(err))) return;
    if (WiFi.status() != WL_CONNECTED) return;

    uint8_t* body = nullptr;
    size_t body_len = 0;
    int status = 0;
    int content_length = 0;
    if (!http_get("api.spotify.com", "/v1/me/player/currently-playing", s_access_token, &body, &body_len, &status, &content_length, err, sizeof(err))) {
        return;
    }

    if (status == 204) {
        free_caps(body);
        SpotifyNowPlaying blank;
        blank.valid = true;
        portENTER_CRITICAL(&s_mux);
        s_now = blank;
        portEXIT_CRITICAL(&s_mux);
        return;
    }

    if (status >= 200 && status < 300) {
        parse_now_playing_json(body, body_len);
    }
    free_caps(body);

    maybe_update_album_art();
}

static void do_controls_if_needed() {
    if (!s_pending_prev && !s_pending_next) return;
    if (WiFi.status() != WL_CONNECTED) {
        s_pending_prev = false;
        s_pending_next = false;
        return;
    }

    char err[96];
    if (!ensure_access_token(err, sizeof(err))) {
        s_pending_prev = false;
        s_pending_next = false;
        return;
    }

    if (s_pending_prev) {
        int st = 0;
        (void)http_post_no_body("api.spotify.com", "/v1/me/player/previous", s_access_token, &st);
    }
    if (s_pending_next) {
        int st = 0;
        (void)http_post_no_body("api.spotify.com", "/v1/me/player/next", s_access_token, &st);
    }

    s_pending_prev = false;
    s_pending_next = false;
}

static void do_complete_auth_if_needed() {
    if (!s_pending_complete) return;

    if (strcmp(s_pending_state, s_pkce_state) != 0) {
        Logger.logMessage("Spotify", "OAuth complete: state mismatch");
        s_pending_complete = false;
        return;
    }

    const unsigned long now = millis();
    if (now - s_pkce_started_ms > 10UL * 60UL * 1000UL) {
        Logger.logMessage("Spotify", "OAuth complete: expired");
        s_pending_complete = false;
        return;
    }

    String form;
    form.reserve(2048);
    form += "grant_type=authorization_code";
    form += "&client_id=";
    append_urlencoded(form, SPOTIFY_CLIENT_ID);
    form += "&code=";
    append_urlencoded(form, s_pending_code);
    form += "&redirect_uri=";
    append_urlencoded(form, SPOTIFY_REDIRECT_URI);
    form += "&code_verifier=";
    append_urlencoded(form, s_pkce_verifier);

    uint8_t* body = nullptr;
    size_t body_len = 0;
    int status = 0;
    char err[96];
    if (!http_post_form("accounts.spotify.com", "/api/token", form.c_str(), &body, &body_len, &status, err, sizeof(err))) {
        Logger.logMessagef("Spotify", "OAuth token exchange failed: %s", err);
        s_pending_complete = false;
        return;
    }

    if (status < 200 || status >= 300) {
        const size_t preview_len = (body_len > 120) ? 120 : body_len;
        char preview[128];
        memset(preview, 0, sizeof(preview));
        if (body && preview_len) {
            memcpy(preview, body, preview_len);
            preview[preview_len] = '\0';
        }
        free_caps(body);
        Logger.logMessagef("Spotify", "OAuth token HTTP %d: %s", status, preview);
        s_pending_complete = false;
        return;
    }

    StaticJsonDocument<2048> doc;
    DeserializationError jerr = deserializeJson(doc, body, body_len);
    free_caps(body);
    if (jerr) {
        Logger.logMessage("Spotify", "OAuth token JSON parse failed");
        s_pending_complete = false;
        return;
    }

    const char* access = doc["access_token"] | "";
    const char* refresh = doc["refresh_token"] | "";
    const int expires_in = doc["expires_in"] | 0;
    if (!access || !access[0] || expires_in <= 0) {
        Logger.logMessage("Spotify", "OAuth token missing access_token");
        s_pending_complete = false;
        return;
    }

    strlcpy(s_access_token, access, sizeof(s_access_token));
    const unsigned long secs = (expires_in > 0) ? (unsigned long)expires_in : 3600UL;
    s_access_token_expires_ms = millis() + secs * 1000UL;

    if (refresh && refresh[0]) {
        (void)spotify_store::save_refresh_token(refresh);
    } else {
        Logger.logMessage("Spotify", "OAuth token missing refresh_token (session only)");
    }

    portENTER_CRITICAL(&s_mux);
    s_now = SpotifyNowPlaying{};
    clear_image_locked();
    portEXIT_CRITICAL(&s_mux);

    Logger.logMessage("Spotify", "OAuth complete: connected");
    s_pending_complete = false;
}

} // namespace

namespace spotify_manager {

void init() {
    Logger.logMessagef(
        "Spotify",
        "Album art support: HAS_DISPLAY=%d HAS_IMAGE_API=%d LV_USE_IMG=%d",
        (int)HAS_DISPLAY,
        (int)HAS_IMAGE_API,
        (int)LV_USE_IMG
    );
}

void loop() {
    do_complete_auth_if_needed();
    do_controls_if_needed();

    if (!s_active) return;
    if (!is_connected()) return;
    if (WiFi.status() != WL_CONNECTED) return;

    const unsigned long now = millis();
    if (now - s_last_poll_ms < 2000) return;
    s_last_poll_ms = now;

    poll_now_playing();
}

void set_active(bool active) {
    s_active = active;
}

bool begin_auth(char* out_authorize_url, size_t out_len, char* out_state, size_t state_len) {
    if (!out_authorize_url || out_len == 0 || !out_state || state_len == 0) return false;

    random_urlsafe_string(s_pkce_state, sizeof(s_pkce_state), 16);
    random_urlsafe_string(s_pkce_verifier, sizeof(s_pkce_verifier), 32);
    s_pkce_started_ms = millis();

    uint8_t hash[32];
    mbedtls_sha256((const unsigned char*)s_pkce_verifier, strlen(s_pkce_verifier), hash, 0);
    char code_challenge[64];
    base64url_from_bytes(hash, sizeof(hash), code_challenge, sizeof(code_challenge));

    String url;
    url.reserve(768);
    url += "https://accounts.spotify.com/authorize?response_type=code";
    url += "&client_id=";
    append_urlencoded(url, SPOTIFY_CLIENT_ID);
    url += "&redirect_uri=";
    append_urlencoded(url, SPOTIFY_REDIRECT_URI);
    url += "&state=";
    append_urlencoded(url, s_pkce_state);
    url += "&scope=";
    append_urlencoded(url, SPOTIFY_SCOPES);
    url += "&code_challenge_method=S256";
    url += "&code_challenge=";
    append_urlencoded(url, code_challenge);
    url += "&show_dialog=true";

    strlcpy(out_authorize_url, url.c_str(), out_len);
    strlcpy(out_state, s_pkce_state, state_len);
    return true;
}

bool queue_complete_auth(const char* code, const char* state) {
    if (!code || !state || !code[0] || !state[0]) return false;
    strlcpy(s_pending_code, code, sizeof(s_pending_code));
    strlcpy(s_pending_state, state, sizeof(s_pending_state));
    s_pending_complete = true;
    return true;
}

bool is_connected() {
    const unsigned long now = millis();
    if (s_access_token[0] && now + 5000 < s_access_token_expires_ms) return true;
    return spotify_store::has_refresh_token();
}

SpotifyNowPlaying get_now_playing() {
    SpotifyNowPlaying snap;
    portENTER_CRITICAL(&s_mux);
    snap = s_now;
    portEXIT_CRITICAL(&s_mux);
    return snap;
}

bool take_image(SpotifyImage* out) {
    if (!out) return false;
    portENTER_CRITICAL(&s_mux);
    if (!s_img.pixels) {
        portEXIT_CRITICAL(&s_mux);
        return false;
    }
    out->pixels = s_img.pixels;
    out->w = s_img.w;
    out->h = s_img.h;
    s_img.pixels = nullptr;
    s_img.w = 0;
    s_img.h = 0;
    portEXIT_CRITICAL(&s_mux);
    return true;
}

void request_prev() { s_pending_prev = true; }
void request_next() { s_pending_next = true; }

void disconnect() {
    spotify_store::clear_refresh_token();
    s_access_token[0] = '\0';
    s_access_token_expires_ms = 0;
    portENTER_CRITICAL(&s_mux);
    s_now = SpotifyNowPlaying{};
    clear_image_locked();
    portEXIT_CRITICAL(&s_mux);
}

} // namespace spotify_manager

#else

namespace spotify_manager {
void init() {}
void loop() {}
void set_active(bool) {}
bool begin_auth(char*, size_t, char*, size_t) { return false; }
bool queue_complete_auth(const char*, const char*) { return false; }
bool is_connected() { return false; }
SpotifyNowPlaying get_now_playing() { return SpotifyNowPlaying{}; }
bool take_image(SpotifyImage*) { return false; }
void request_prev() {}
void request_next() {}
void disconnect() {}
}

#endif
