/*
 * Web Configuration Portal Implementation
 * 
 * Async web server with captive portal support.
 * Serves static files and provides REST API for configuration.
 */

// AsyncTCP task stack size (FreeRTOS task stack lives in internal RAM).
// Boards may override this in their board_overrides.h.
// Historical runs show CONFIG_ASYNC_TCP_STACK_SIZE(raw)=10240 for this project;
// keep a conservative default here for boards without overrides.
#ifndef CONFIG_ASYNC_TCP_STACK_SIZE
#define CONFIG_ASYNC_TCP_STACK_SIZE 10240
#endif

#include "web_portal.h"
#include "web_assets.h"
#include "config_manager.h"
#include "log_manager.h"
#include "board_config.h"
#include "macros_config.h"
#include "macro_templates.h"
#include "device_telemetry.h"
#include "github_release_config.h"
#include "../version.h"

#if HAS_DISPLAY && HAS_ICONS
#include "icon_registry.h"
#include "icon_store.h"
#include <FFat.h>
#endif

#if HAS_DISPLAY
#include "display_manager.h"
#include "screen_saver_manager.h"
#endif

#if HAS_IMAGE_API
#include "image_api.h"
#endif

#include <ESPAsyncWebServer.h>
#include <DNSServer.h>
#include <WiFi.h>
#include <ArduinoJson.h>
#include <Update.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>

#include <esp_heap_caps.h>
#include <esp_system.h>
#include <soc/soc_caps.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>


// Forward declarations
void handleRoot(AsyncWebServerRequest *request);
void handleHome(AsyncWebServerRequest *request);
void handleNetwork(AsyncWebServerRequest *request);
void handleFirmware(AsyncWebServerRequest *request);
void handleCSS(AsyncWebServerRequest *request);
void handleJS(AsyncWebServerRequest *request);
void handleGetConfig(AsyncWebServerRequest *request);
void handlePostConfig(AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total);
void handleDeleteConfig(AsyncWebServerRequest *request);
void handleSetDisplayBrightness(AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total);
void handleGetVersion(AsyncWebServerRequest *request);
void handleGetMode(AsyncWebServerRequest *request);
void handleGetHealth(AsyncWebServerRequest *request);
void handleReboot(AsyncWebServerRequest *request);
void handleOTAUpload(AsyncWebServerRequest *request, String filename, size_t index, uint8_t *data, size_t len, bool final);
void handleGetDisplaySleep(AsyncWebServerRequest *request);
void handlePostDisplaySleep(AsyncWebServerRequest *request);
void handlePostDisplayWake(AsyncWebServerRequest *request);
void handlePostDisplayActivity(AsyncWebServerRequest *request);

void handleGetMacros(AsyncWebServerRequest *request);
void handlePostMacros(AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total);

void handleGetIcons(AsyncWebServerRequest *request);
void handleGetInstalledIcons(AsyncWebServerRequest *request);
void handlePostIconInstall(AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total);
void handlePostIconGC(AsyncWebServerRequest *request);

void handleGetFirmwareLatest(AsyncWebServerRequest *request);
void handlePostFirmwareUpdate(AsyncWebServerRequest *request);
void handleGetFirmwareUpdateStatus(AsyncWebServerRequest *request);

// Web server on port 80 (pointer to avoid constructor issues)
AsyncWebServer *server = nullptr;

// DNS server for captive portal (port 53)
DNSServer dnsServer;

// AP configuration
#define DNS_PORT 53
#define CAPTIVE_PORTAL_IP IPAddress(192, 168, 4, 1)

// State
static bool ap_mode_active = false;
static DeviceConfig *current_config = nullptr;
static bool ota_in_progress = false;
static size_t ota_progress = 0;
static size_t ota_total = 0;

// ===== Macros Config (screens × buttons) =====
static bool g_macros_loaded = false;
static uint8_t* g_macros_body = nullptr;
static size_t g_macros_body_total = 0;
static bool g_macros_body_in_progress = false;

// MVP: macros editor posts the full screens × buttons config. Keep the parsing document bounded.
// If MACROS_* dimensions or max string sizes are increased, adjust this accordingly.
static constexpr size_t kMacrosJsonDocCapacity = 65536;

struct MacrosJsonAllocator {
    void* allocate(size_t size) {
#if SOC_SPIRAM_SUPPORTED
        if (psramFound()) {
            void* p = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (p) return p;
        }
#endif
        return heap_caps_malloc(size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }

    void deallocate(void* ptr) {
        heap_caps_free(ptr);
    }

    void* reallocate(void* ptr, size_t new_size) {
#if SOC_SPIRAM_SUPPORTED
        if (psramFound()) {
            void* p = heap_caps_realloc(ptr, new_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (p) return p;
        }
#endif
        return heap_caps_realloc(ptr, new_size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
};

// Protect macros upload globals from theoretical cross-task interleaving.
static portMUX_TYPE g_macros_body_mux = portMUX_INITIALIZER_UNLOCKED;

static void macros_body_reset() {
    uint8_t* toFree = nullptr;
    portENTER_CRITICAL(&g_macros_body_mux);
    toFree = g_macros_body;
    g_macros_body = nullptr;
    g_macros_body_total = 0;
    g_macros_body_in_progress = false;
    portEXIT_CRITICAL(&g_macros_body_mux);

    if (toFree) {
        free(toFree);
    }
}

// ===== Icon install body (binary) =====
static uint8_t* g_icon_body = nullptr;
static size_t g_icon_body_total = 0;
static bool g_icon_body_in_progress = false;
static portMUX_TYPE g_icon_body_mux = portMUX_INITIALIZER_UNLOCKED;

static void icon_body_reset() {
    uint8_t* toFree = nullptr;
    portENTER_CRITICAL(&g_icon_body_mux);
    toFree = g_icon_body;
    g_icon_body = nullptr;
    g_icon_body_total = 0;
    g_icon_body_in_progress = false;
    portEXIT_CRITICAL(&g_icon_body_mux);
    if (toFree) free(toFree);
}

// ===== Chunked response helpers =====
static size_t chunk_copy_out(uint8_t* dst, size_t maxLen, const char* src, size_t srcLen, size_t& srcOff) {
    if (!src || srcOff >= srcLen || maxLen == 0) return 0;
    const size_t n = (srcLen - srcOff) < maxLen ? (srcLen - srcOff) : maxLen;
    memcpy(dst, src + srcOff, n);
    srcOff += n;
    return n;
}

template <typename State>
static void send_chunked_state(AsyncWebServerRequest* request, const char* contentType, State* st) {
    AsyncWebServerResponse* response = request->beginChunkedResponse(
        contentType,
        [st](uint8_t* buffer, size_t maxLen, size_t /*index*/) mutable -> size_t {
            if (!st) return 0;
            const size_t n = st->fill(buffer, maxLen);
            if (n == 0) {
                delete st;
                st = nullptr;
            }
            return n;
        }
    );
    request->send(response);
}

struct JsonStringChunker {
    String payload;
    size_t off = 0;

    size_t fill(uint8_t* buffer, size_t maxLen) {
        if (maxLen == 0) return 0;
        const size_t len = payload.length();
        if (off >= len) return 0;
        const size_t n = (len - off) < maxLen ? (len - off) : maxLen;
        memcpy(buffer, payload.c_str() + off, n);
        off += n;
        return n;
    }
};

template <typename JsonDoc>
static bool send_json_doc_chunked(AsyncWebServerRequest* request, JsonDoc& doc, int oom_http_status) {
    JsonStringChunker* st = new JsonStringChunker();
    if (!st) {
        request->send(oom_http_status, "application/json", "{\"success\":false,\"message\":\"Out of memory\"}");
        return false;
    }

    const size_t requiredCapacity = measureJson(doc) + 1;
    if (!st->payload.reserve(requiredCapacity)) {
        delete st;
        request->send(oom_http_status, "application/json", "{\"success\":false,\"message\":\"Out of memory\"}");
        return false;
    }
    serializeJson(doc, st->payload);
    send_chunked_state(request, "application/json", st);
    return true;
}

// The runtime macro screen UI reads from this instance (defined in app.ino).
extern MacroConfig macro_config;

// ===== Basic Auth (optional; STA/full mode only) =====
static bool portal_auth_required() {
    if (ap_mode_active) return false;
    if (!current_config) return false;
    return current_config->basic_auth_enabled;
}

static bool portal_auth_gate(AsyncWebServerRequest *request) {
    // AsyncWebServer handlers execute on the AsyncTCP task.
    // Log stack margin once so we can safely tune CONFIG_ASYNC_TCP_STACK_SIZE.
    static bool logged_async_stack = false;
    if (!logged_async_stack) {
        const UBaseType_t remaining_words = uxTaskGetStackHighWaterMark(nullptr);
        const uint32_t unit_bytes = (uint32_t)sizeof(StackType_t);
        const uint32_t remaining_bytes = (uint32_t)remaining_words * unit_bytes;

        // NOTE: uxTaskGetStackHighWaterMark() returns *words*.
        // The actual configured AsyncTCP stack size depends on how the AsyncTCP library is compiled.
        // CONFIG_ASYNC_TCP_STACK_SIZE here is the preprocessor value seen by this translation unit
        // (it may or may not match the value used inside the AsyncTCP library).
        const char* taskName = pcTaskGetName(nullptr);
        Logger.logMessagef(
            "Portal",
            "AsyncTCP stack watermark: task=%s rem=%u units (%u B), unit=%u B, CONFIG_ASYNC_TCP_STACK_SIZE(raw)=%u",
            taskName ? taskName : "(null)",
            (unsigned)remaining_words,
            (unsigned)remaining_bytes,
            (unsigned)unit_bytes,
            (unsigned)CONFIG_ASYNC_TCP_STACK_SIZE);
        logged_async_stack = true;
    }

    if (!portal_auth_required()) return true;

    const char *user = current_config->basic_auth_username;
    const char *pass = current_config->basic_auth_password;

    if (request->authenticate(user, pass)) {
        return true;
    }

    request->requestAuthentication(PROJECT_DISPLAY_NAME);
    return false;
}

// ===== /api/macros helpers =====
static const char* macro_action_to_string(MacroButtonAction a) {
    switch (a) {
        case MacroButtonAction::None: return "none";
        case MacroButtonAction::SendKeys: return "send_keys";
        case MacroButtonAction::NavPrevScreen: return "nav_prev";
        case MacroButtonAction::NavNextScreen: return "nav_next";
        case MacroButtonAction::NavToScreen: return "nav_to";
        case MacroButtonAction::GoBack: return "go_back";
        case MacroButtonAction::MqttSend: return "mqtt_send";
        default: return "none";
    }
}

static const char* macro_icon_type_to_string(MacroIconType t) {
    switch (t) {
        case MacroIconType::None: return "none";
        case MacroIconType::Builtin: return "builtin";
        case MacroIconType::Emoji: return "emoji";
        case MacroIconType::Asset: return "asset";
        default: return "none";
    }
}

static MacroIconType macro_icon_type_from_string(const char* s) {
    if (!s || !*s) return MacroIconType::None;
    if (strcasecmp(s, "none") == 0) return MacroIconType::None;
    if (strcasecmp(s, "builtin") == 0) return MacroIconType::Builtin;
    if (strcasecmp(s, "emoji") == 0) return MacroIconType::Emoji;
    if (strcasecmp(s, "asset") == 0) return MacroIconType::Asset;
    return MacroIconType::None;
}

static MacroButtonAction macro_action_from_string(const char* s) {
    if (!s || !*s) return MacroButtonAction::None;
    if (strcasecmp(s, "none") == 0) return MacroButtonAction::None;
    if (strcasecmp(s, "send_keys") == 0) return MacroButtonAction::SendKeys;
    if (strcasecmp(s, "nav_prev") == 0) return MacroButtonAction::NavPrevScreen;
    if (strcasecmp(s, "nav_next") == 0) return MacroButtonAction::NavNextScreen;
    if (strcasecmp(s, "nav_to") == 0) return MacroButtonAction::NavToScreen;
    if (strcasecmp(s, "go_back") == 0) return MacroButtonAction::GoBack;
    if (strcasecmp(s, "mqtt_send") == 0) return MacroButtonAction::MqttSend;
    return MacroButtonAction::None;
}

static uint32_t clamp_rgb24(uint32_t v) {
    // Colors are RGB-only: 0xRRGGBB
    return v & 0x00FFFFFFu;
}

static void macros_cache_load_if_needed() {
    if (g_macros_loaded) return;
    g_macros_loaded = true;
    if (!macros_config_load(&macro_config)) {
        macros_config_set_defaults(&macro_config);
    }
}

// GET /api/macros
void handleGetMacros(AsyncWebServerRequest *request) {
    if (!portal_auth_gate(request)) return;

    macros_cache_load_if_needed();

    // Stream the response in chunks to avoid building the full JSON in RAM.
    // This reduces transient allocations and keeps TTFB low.
    struct MacrosChunker {
        // Output cursors
        const char* cur = nullptr;
        size_t cur_len = 0;
        size_t cur_off = 0;

        // Iteration
        int tpl_i = 0;
        bool tpl_first = true;
        int screen_i = 0;
        int btn_i = 0;

        enum class Phase : uint8_t {
            Header,
            Templates,
            AfterTemplates,
            ScreenStart,
            Buttons,
            ScreenEnd,
            AfterScreens,
            Done,
        } phase = Phase::Header;

        String scratch;
        String item_json;

        MacrosChunker() {
            scratch.reserve(2048);
            item_json.reserve(1024);
        }

        void set_piece(const char* p, size_t n) {
            cur = p;
            cur_len = n;
            cur_off = 0;
        }

        bool next_piece() {
            static const char* kTemplateIds[] = {
                macro_templates::kTemplateRoundRing9,
                macro_templates::kTemplateRoundPie8,
                macro_templates::kTemplateStackSides5,
                macro_templates::kTemplateWideSides3,
                macro_templates::kTemplateSplitSides4,
            };

            scratch.remove(0);
            item_json.remove(0);

            switch (phase) {
                case Phase::Header: {
                    // Keep this in sync with src/app/macros_config.cpp (MACROS_VERSION).
                    char hdr[256];
                    snprintf(
                        hdr,
                        sizeof(hdr),
                        "{\"success\":true,\"version\":9,\"buttons_per_screen\":%u,\"defaults\":{\"screen_bg\":%u,\"button_bg\":%u,\"icon_color\":%u,\"label_color\":%u},\"templates\":[",
                        (unsigned)MACROS_BUTTONS_PER_SCREEN,
                        (unsigned)macro_config.default_screen_bg,
                        (unsigned)macro_config.default_button_bg,
                        (unsigned)macro_config.default_icon_color,
                        (unsigned)macro_config.default_label_color);
                    scratch += hdr;
                    set_piece(scratch.c_str(), scratch.length());
                    phase = Phase::Templates;
                    return true;
                }

                case Phase::Templates: {
                    while (tpl_i < (int)(sizeof(kTemplateIds) / sizeof(kTemplateIds[0]))) {
                        const char* id = kTemplateIds[tpl_i++];
                        if (!id || !*id) continue;
                        const char* layout = macro_templates::selector_layout_json(id);
                        if (!layout) continue;

                        if (!tpl_first) scratch += ",";
                        tpl_first = false;
                        scratch += "{\"id\":\"";
                        scratch += id;
                        scratch += "\",\"name\":\"";
                        scratch += macro_templates::display_name(id);
                        scratch += "\",\"selector_layout\":";
                        scratch += layout;
                        scratch += "}";

                        set_piece(scratch.c_str(), scratch.length());
                        return true;
                    }
                    phase = Phase::AfterTemplates;
                    return next_piece();
                }

                case Phase::AfterTemplates: {
                    set_piece("],\"screens\":[", strlen("],\"screens\":["));
                    phase = Phase::ScreenStart;
                    screen_i = 0;
                    btn_i = 0;
                    return true;
                }

                case Phase::ScreenStart: {
                    if (screen_i >= MACROS_SCREEN_COUNT) {
                        phase = Phase::AfterScreens;
                        return next_piece();
                    }

                    if (screen_i > 0) scratch += ",";
                    const char* tpl = macro_config.template_id[screen_i];
                    if (!macro_templates::is_valid(tpl)) {
                        tpl = macro_templates::default_id();
                    }

                    scratch += "{\"template\":\"";
                    scratch += tpl;
                    scratch += "\"";

                    if (macro_config.screen_bg[screen_i] != MACROS_COLOR_UNSET) {
                        scratch += ",\"screen_bg\":";
                        scratch += (unsigned)macro_config.screen_bg[screen_i];
                    }

                    scratch += ",\"buttons\":[";
                    set_piece(scratch.c_str(), scratch.length());
                    phase = Phase::Buttons;
                    btn_i = 0;
                    return true;
                }

                case Phase::Buttons: {
                    if (btn_i >= MACROS_BUTTONS_PER_SCREEN) {
                        phase = Phase::ScreenEnd;
                        return next_piece();
                    }

                    const MacroButtonConfig* btn = &macro_config.buttons[screen_i][btn_i];

                    // Use ArduinoJson to correctly escape strings.
                    StaticJsonDocument<768> item;
                    item["label"] = btn->label;
                    item["action"] = macro_action_to_string(btn->action);
                    item["payload"] = btn->payload;
                    item["mqtt_topic"] = btn->mqtt_topic;

                    JsonObject icon = item.createNestedObject("icon");
                    icon["type"] = macro_icon_type_to_string(btn->icon.type);
                    icon["id"] = btn->icon.id;
                    icon["display"] = btn->icon.display;

                    if (btn->button_bg != MACROS_COLOR_UNSET) item["button_bg"] = btn->button_bg;
                    if (btn->icon_color != MACROS_COLOR_UNSET) item["icon_color"] = btn->icon_color;
                    if (btn->label_color != MACROS_COLOR_UNSET) item["label_color"] = btn->label_color;

                    // serializeJson(doc, String&) overwrites the destination, so build the
                    // delimiter + object into scratch and stream that.
                    if (btn_i > 0) scratch += ",";
                    serializeJson(item, item_json);
                    scratch += item_json;
                    btn_i++;

                    set_piece(scratch.c_str(), scratch.length());
                    return true;
                }

                case Phase::ScreenEnd: {
                    set_piece("]}", strlen("]}"));
                    phase = Phase::ScreenStart;
                    screen_i++;
                    return true;
                }

                case Phase::AfterScreens: {
                    set_piece("]}", strlen("]}"));
                    phase = Phase::Done;
                    return true;
                }

                case Phase::Done:
                default:
                    return false;
            }
        }

        size_t fill(uint8_t* buffer, size_t maxLen) {
            size_t wrote = 0;
            while (wrote < maxLen) {
                if (!cur || cur_off >= cur_len) {
                    if (!next_piece()) {
                        break;
                    }
                }
                const size_t n = chunk_copy_out(buffer + wrote, maxLen - wrote, cur, cur_len, cur_off);
                if (n == 0) {
                    // Safety: avoid infinite loops if something goes wrong.
                    cur = nullptr;
                    cur_len = 0;
                    cur_off = 0;
                    continue;
                }
                wrote += n;
            }
            return wrote;
        }
    };

    MacrosChunker* st = new MacrosChunker();
    if (!st) {
        request->send(500, "application/json", "{\"success\":false,\"message\":\"Out of memory\"}");
        return;
    }

    send_chunked_state(request, "application/json", st);
}

// GET /api/icons
// Returns the compiled icon IDs so the portal can offer an autocomplete list.
void handleGetIcons(AsyncWebServerRequest *request) {
    if (!portal_auth_gate(request)) return;

    // Stream response to keep AsyncTCP task snappy.
    struct IconsChunker {
        const char* cur = nullptr;
        size_t cur_len = 0;
        size_t cur_off = 0;

        enum class Phase : uint8_t { Header, Items, Done } phase = Phase::Header;

        size_t i = 0;
        bool first = true;

        String scratch;

        IconsChunker() { scratch.reserve(256); }

        void set_piece(const char* p, size_t n) {
            cur = p;
            cur_len = n;
            cur_off = 0;
        }

        bool next_piece() {
            scratch.remove(0);

            switch (phase) {
                case Phase::Header:
                    set_piece("{\"icons\":[", strlen("{\"icons\":["));
                    phase = Phase::Items;
                    return true;

                case Phase::Items: {
#if HAS_DISPLAY && HAS_ICONS
                    const size_t n = icon_registry_count();
                    while (i < n) {
                        const char* id = icon_registry_id_at(i);
                        const IconKind kind = icon_registry_kind_at(i);
                        i++;

                        if (!id || !*id) continue;

                        if (!first) scratch += ",";
                        first = false;

                        // icon_id is expected to be a safe identifier ([a-z0-9_]+), so we don't do JSON escaping here.
                        scratch += "{\"id\":\"";
                        scratch += id;
                        scratch += "\",\"kind\":\"";
                        scratch += (kind == IconKind::Color ? "color" : "mask");
                        scratch += "\"}";

                        set_piece(scratch.c_str(), scratch.length());
                        return true;
                    }
#endif

                    set_piece("]}", strlen("]}"));
                    phase = Phase::Done;
                    return true;
                }

                case Phase::Done:
                default:
                    return false;
            }
        }

        size_t fill(uint8_t* buffer, size_t maxLen) {
            size_t wrote = 0;
            while (wrote < maxLen) {
                if (!cur || cur_off >= cur_len) {
                    if (!next_piece()) break;
                }
                const size_t n = chunk_copy_out(buffer + wrote, maxLen - wrote, cur, cur_len, cur_off);
                if (n == 0) {
                    cur = nullptr;
                    cur_len = 0;
                    cur_off = 0;
                    break;
                }
                wrote += n;
            }
            return wrote;
        }
    };

    IconsChunker* st = new IconsChunker();
    if (!st) {
        request->send(500, "application/json", "{\"success\":false,\"message\":\"Out of memory\"}");
        return;
    }

    send_chunked_state(request, "application/json", st);
}

// GET /api/icons/installed
// Returns installed (FFat) icon IDs so the portal can offer them too.
void handleGetInstalledIcons(AsyncWebServerRequest *request) {
    if (!portal_auth_gate(request)) return;

    #if HAS_DISPLAY && HAS_ICONS
    // Stream installed icons directly from FFat to avoid fixed-size buffers.
    struct InstalledIconsChunker {
        const char* cur = nullptr;
        size_t cur_len = 0;
        size_t cur_off = 0;

        enum class Phase : uint8_t { Header, Items, Footer, Done } phase = Phase::Header;

        bool first = true;
        bool ffat_ok = false;
        File dir;
        File f;

        String scratch;

        InstalledIconsChunker() {
            scratch.reserve(256);

            ffat_ok = icon_store_ffat_ready() && FFat.exists("/icons");
            if (ffat_ok) {
                dir = FFat.open("/icons");
                if (!dir || !dir.isDirectory()) {
                    ffat_ok = false;
                } else {
                    f = dir.openNextFile();
                }
            }
        }

        void set_piece(const char* p, size_t n) {
            cur = p;
            cur_len = n;
            cur_off = 0;
        }

        bool next_piece() {
            scratch.remove(0);

            switch (phase) {
                case Phase::Header:
                    set_piece("{\"success\":true,\"source\":\"ffat\",\"icons\":[", strlen("{\"success\":true,\"source\":\"ffat\",\"icons\":["));
                    phase = Phase::Items;
                    return true;

                case Phase::Items: {
                    if (!ffat_ok) {
                        phase = Phase::Footer;
                        return next_piece();
                    }

                    while (f) {
                        if (!f.isDirectory()) {
                            const char* name = f.name();
                            if (name) {
                                const char* base = strrchr(name, '/');
                                base = base ? (base + 1) : name;
                                const char* ext = strrchr(base, '.');
                                const size_t base_len = ext ? (size_t)(ext - base) : strlen(base);

                                if (ext && strcmp(ext, ".bin") == 0 && base_len > 0 && base_len < 64) {
                                    char tmp[64];
                                    memset(tmp, 0, sizeof(tmp));
                                    memcpy(tmp, base, base_len);
                                    tmp[base_len] = '\0';

                                    if (!first) scratch += ",";
                                    first = false;

                                    // `tmp` is derived from filename and restricted to [a-z0-9_]+, so no escaping needed.
                                    scratch += "{\"id\":\"";
                                    scratch += tmp;
                                    scratch += "\",\"kind\":\"color\"}";

                                    f = dir.openNextFile();
                                    set_piece(scratch.c_str(), scratch.length());
                                    return true;
                                }
                            }
                        }
                        f = dir.openNextFile();
                    }

                    phase = Phase::Footer;
                    return next_piece();
                }

                case Phase::Footer:
                    set_piece("]}", strlen("]}"));
                    phase = Phase::Done;
                    return true;

                case Phase::Done:
                default:
                    return false;
            }
        }

        size_t fill(uint8_t* buffer, size_t maxLen) {
            size_t wrote = 0;
            while (wrote < maxLen) {
                if (!cur || cur_off >= cur_len) {
                    if (!next_piece()) break;
                }
                const size_t n = chunk_copy_out(buffer + wrote, maxLen - wrote, cur, cur_len, cur_off);
                if (n == 0) {
                    cur = nullptr;
                    cur_len = 0;
                    cur_off = 0;
                    break;
                }
                wrote += n;
            }
            return wrote;
        }
    };

    InstalledIconsChunker* st = new InstalledIconsChunker();
    if (!st) {
        request->send(503, "application/json", "{\"success\":false,\"message\":\"Out of memory\"}");
        return;
    }

    send_chunked_state(request, "application/json", st);
    #else
    request->send(200, "application/json", "{\"success\":true,\"source\":\"ffat\",\"icons\":[]}");
    #endif
}

// POST /api/icons/install?id=<icon_id>
// Body is an application/octet-stream blob produced by the portal.
void handlePostIconInstall(AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total) {
    if (!portal_auth_gate(request)) return;

    #if !(HAS_DISPLAY && HAS_ICONS)
    (void)data; (void)len; (void)index; (void)total;
    request->send(400, "application/json", "{\"success\":false,\"message\":\"Icons not supported on this target\"}");
    return;
    #else

    const String idParam = request->hasParam("id") ? request->getParam("id")->value() : String();
    if (idParam.length() == 0) {
        request->send(400, "application/json", "{\"success\":false,\"message\":\"Missing id\"}");
        return;
    }

    // Chunk-safe body accumulation.
    if (index == 0) {
        bool alreadyInProgress = false;
        uint8_t* staleBody = nullptr;
        portENTER_CRITICAL(&g_icon_body_mux);
        alreadyInProgress = g_icon_body_in_progress;
        if (!alreadyInProgress) {
            staleBody = g_icon_body;
            g_icon_body = nullptr;
            g_icon_body_total = total;
            g_icon_body_in_progress = true;
        }
        portEXIT_CRITICAL(&g_icon_body_mux);

        if (staleBody) free(staleBody);

        if (alreadyInProgress) {
            request->send(409, "application/json", "{\"success\":false,\"message\":\"Another icon install is in progress\"}");
            return;
        }

        if (total == 0 || total > (256 * 1024)) {
            icon_body_reset();
            request->send(400, "application/json", "{\"success\":false,\"message\":\"Invalid body size\"}");
            return;
        }

        g_icon_body = (uint8_t*)malloc(total);
        if (!g_icon_body) {
            icon_body_reset();
            request->send(500, "application/json", "{\"success\":false,\"message\":\"Out of memory\"}");
            return;
        }
    }

    if (!g_icon_body_in_progress || !g_icon_body || g_icon_body_total != total) {
        icon_body_reset();
        request->send(500, "application/json", "{\"success\":false,\"message\":\"Internal state error\"}");
        return;
    }

    if (index + len > total) {
        icon_body_reset();
        request->send(400, "application/json", "{\"success\":false,\"message\":\"Body overflow\"}");
        return;
    }

    memcpy(g_icon_body + index, data, len);

    // Not done yet.
    if (index + len != total) return;

    char err[128];
    const bool ok = icon_store_install_blob(idParam.c_str(), g_icon_body, g_icon_body_total, err, sizeof(err));
    icon_body_reset();

    if (!ok) {
        AsyncResponseStream* response = request->beginResponseStream("application/json");
        response->setCode(400);
        response->print("{\"success\":false,\"message\":\"");
        response->print(err);
        response->print("\"}");
        request->send(response);
        return;
    }

    request->send(200, "application/json", "{\"success\":true}");
    #endif
}

// POST /api/icons/gc
// Deletes unused installed icons (emoji_*, user_*) based on the current (already-saved) macro config.
void handlePostIconGC(AsyncWebServerRequest *request) {
    if (!portal_auth_gate(request)) return;

    #if !(HAS_DISPLAY && HAS_ICONS)
    request->send(400, "application/json", "{\"success\":false,\"message\":\"Icons not supported on this target\"}");
    return;
    #else

    size_t deleted = 0;
    size_t bytes = 0;
    char err[128];
    const bool ok = icon_store_gc_unused_from_macros(&macro_config, &deleted, &bytes, err, sizeof(err));
    if (!ok) {
        AsyncResponseStream* response = request->beginResponseStream("application/json");
        response->setCode(400);
        response->print("{\"success\":false,\"message\":\"");
        response->print(err);
        response->print("\"}");
        request->send(response);
        return;
    }

    AsyncResponseStream* response = request->beginResponseStream("application/json");
    response->setCode(200);
    response->print("{\"success\":true,\"deleted\":");
    response->print((unsigned)deleted);
    response->print(",\"bytes_freed\":");
    response->print((unsigned)bytes);
    response->print("}");
    request->send(response);
    #endif
}

// POST /api/macros
// Accepts a single JSON payload containing all screens × buttons.
void handlePostMacros(AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total) {
    if (!portal_auth_gate(request)) return;

    // Chunk-safe body accumulation (AsyncWebServer may call us multiple times).
    if (index == 0) {
        bool alreadyInProgress = false;
        uint8_t* staleBody = nullptr;
        portENTER_CRITICAL(&g_macros_body_mux);
        alreadyInProgress = g_macros_body_in_progress;
        if (!alreadyInProgress) {
            // Clear any stale buffer from a previous attempt.
            staleBody = g_macros_body;
            g_macros_body = nullptr;
            g_macros_body_total = total;
            g_macros_body_in_progress = true;
        }
        portEXIT_CRITICAL(&g_macros_body_mux);

        if (staleBody) {
            free(staleBody);
        }

        if (alreadyInProgress) {
            request->send(409, "application/json", "{\"success\":false,\"message\":\"Another macros update is in progress\"}");
            return;
        }

#if SOC_SPIRAM_SUPPORTED
        if (psramFound()) {
            g_macros_body = (uint8_t*)heap_caps_malloc(total, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        }
#endif
        if (!g_macros_body) {
            g_macros_body = (uint8_t*)malloc(total);
        }
        if (!g_macros_body) {
            macros_body_reset();
            request->send(500, "application/json", "{\"success\":false,\"message\":\"Out of memory\"}");
            return;
        }
    }

    if (!g_macros_body_in_progress || !g_macros_body || g_macros_body_total != total) {
        macros_body_reset();
        request->send(500, "application/json", "{\"success\":false,\"message\":\"Internal state error\"}");
        return;
    }

    if (index + len > total) {
        macros_body_reset();
        request->send(400, "application/json", "{\"success\":false,\"message\":\"Invalid upload\"}");
        return;
    }

    memcpy(g_macros_body + index, data, len);

    const bool is_final = (index + len == total);
    if (!is_final) {
        return;
    }

#if MEMORY_SNAPSHOT_ON_HTTP_ENABLED
    // Capture heap state right before we allocate/parse the JSON document.
    device_telemetry_log_memory_snapshot("http_macros_post_begin");
#endif

    // Parse JSON body
    BasicJsonDocument<MacrosJsonAllocator> doc(kMacrosJsonDocCapacity);
    DeserializationError error = deserializeJson(doc, g_macros_body, total);

    macros_body_reset();

    if (error) {
        Logger.logMessagef("Macros", "JSON parse error: %s", error.c_str());
#if MEMORY_SNAPSHOT_ON_HTTP_ENABLED
        device_telemetry_log_memory_snapshot("http_macros_post_parse_fail");
#endif
        if (error == DeserializationError::NoMemory) {
            request->send(413, "application/json", "{\"success\":false,\"message\":\"JSON body too large\"}");
            return;
        }
        request->send(400, "application/json", "{\"success\":false,\"message\":\"Invalid JSON\"}");
        return;
    }

#if MEMORY_SNAPSHOT_ON_HTTP_ENABLED
    device_telemetry_log_memory_snapshot("http_macros_post_parsed");
#endif

    // Basic shape validation
    if (!doc.containsKey("screens") || !doc["screens"].is<JsonArray>()) {
        request->send(400, "application/json", "{\"success\":false,\"message\":\"Missing screens[]\"}");
        return;
    }

    JsonArray screens = doc["screens"].as<JsonArray>();
    if ((int)screens.size() != MACROS_SCREEN_COUNT) {
        request->send(400, "application/json", "{\"success\":false,\"message\":\"screens[] has wrong length\"}");
        return;
    }

    MacroConfig* next = nullptr;
#if SOC_SPIRAM_SUPPORTED
    if (psramFound()) {
        next = (MacroConfig*)heap_caps_malloc(sizeof(MacroConfig), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
#endif
    if (!next) {
        next = (MacroConfig*)malloc(sizeof(MacroConfig));
    }
    if (!next) {
        request->send(500, "application/json", "{\"success\":false,\"message\":\"Out of memory\"}");
        return;
    }
    macros_config_set_defaults(next);

    // Global defaults
    if (doc.containsKey("defaults") && doc["defaults"].is<JsonObject>()) {
        JsonObject d = doc["defaults"].as<JsonObject>();
        if (d.containsKey("screen_bg")) next->default_screen_bg = clamp_rgb24(d["screen_bg"] | next->default_screen_bg);
        if (d.containsKey("button_bg")) next->default_button_bg = clamp_rgb24(d["button_bg"] | next->default_button_bg);
        if (d.containsKey("icon_color")) next->default_icon_color = clamp_rgb24(d["icon_color"] | next->default_icon_color);
        if (d.containsKey("label_color")) next->default_label_color = clamp_rgb24(d["label_color"] | next->default_label_color);
    }

    for (int s = 0; s < MACROS_SCREEN_COUNT; s++) {
        JsonVariant sv = screens[s];
        if (!sv.is<JsonObject>()) {
            free(next);
            request->send(400, "application/json", "{\"success\":false,\"message\":\"screens[] entries must be objects\"}");
            return;
        }
        JsonObject so = sv.as<JsonObject>();

        // Optional per-screen background override.
        if (so.containsKey("screen_bg")) {
            next->screen_bg[s] = clamp_rgb24(so["screen_bg"] | 0);
        } else {
            next->screen_bg[s] = MACROS_COLOR_UNSET;
        }

        // Optional template selection (defaults to the firmware default if missing/invalid).
        const char* tpl = so["template"] | macro_templates::default_id();
        if (!macro_templates::is_valid(tpl)) {
            tpl = macro_templates::default_id();
        }
        strlcpy(next->template_id[s], tpl, sizeof(next->template_id[s]));

        if (!so.containsKey("buttons") || !so["buttons"].is<JsonArray>()) {
            free(next);
            request->send(400, "application/json", "{\"success\":false,\"message\":\"Each screen must have buttons[]\"}");
            return;
        }
        JsonArray buttons = so["buttons"].as<JsonArray>();
        if ((int)buttons.size() != MACROS_BUTTONS_PER_SCREEN) {
            free(next);
            request->send(400, "application/json", "{\"success\":false,\"message\":\"buttons[] has wrong length\"}");
            return;
        }

        for (int b = 0; b < MACROS_BUTTONS_PER_SCREEN; b++) {
            JsonVariant bv = buttons[b];
            if (!bv.is<JsonObject>()) {
                free(next);
                request->send(400, "application/json", "{\"success\":false,\"message\":\"buttons[] entries must be objects\"}");
                return;
            }
            JsonObject bo = bv.as<JsonObject>();

            const char* label = bo["label"] | "";
            const char* action_s = bo["action"] | "none";
            const char* payload = bo["payload"] | "";
            const char* mqtt_topic = bo["mqtt_topic"] | "";

            MacroIconType iconType = MacroIconType::None;
            const char* iconId = "";
            const char* iconDisplay = "";
            if (bo.containsKey("icon") && bo["icon"].is<JsonObject>()) {
                JsonObject io = bo["icon"].as<JsonObject>();
                iconType = macro_icon_type_from_string(io["type"] | "none");
                iconId = io["id"] | "";
                iconDisplay = io["display"] | "";
            }

            strlcpy(next->buttons[s][b].label, label, sizeof(next->buttons[s][b].label));
            next->buttons[s][b].action = macro_action_from_string(action_s);
            strlcpy(next->buttons[s][b].payload, payload, sizeof(next->buttons[s][b].payload));
            strlcpy(next->buttons[s][b].mqtt_topic, mqtt_topic, sizeof(next->buttons[s][b].mqtt_topic));

            next->buttons[s][b].icon.type = iconType;
            strlcpy(next->buttons[s][b].icon.id, iconId, sizeof(next->buttons[s][b].icon.id));
            strlcpy(next->buttons[s][b].icon.display, iconDisplay, sizeof(next->buttons[s][b].icon.display));

            // Optional per-button appearance overrides.
            next->buttons[s][b].button_bg = bo.containsKey("button_bg") ? clamp_rgb24(bo["button_bg"] | 0) : MACROS_COLOR_UNSET;
            next->buttons[s][b].icon_color = bo.containsKey("icon_color") ? clamp_rgb24(bo["icon_color"] | 0) : MACROS_COLOR_UNSET;
            next->buttons[s][b].label_color = bo.containsKey("label_color") ? clamp_rgb24(bo["label_color"] | 0) : MACROS_COLOR_UNSET;

            // Normalize: if action is none, clear payload/icon to keep state tidy.
            if (next->buttons[s][b].action == MacroButtonAction::None) {
                next->buttons[s][b].payload[0] = '\0';
                next->buttons[s][b].mqtt_topic[0] = '\0';
                next->buttons[s][b].icon.type = MacroIconType::None;
                next->buttons[s][b].icon.id[0] = '\0';
                next->buttons[s][b].icon.display[0] = '\0';
            }

            // Validate: mqtt_send requires a topic.
            if (next->buttons[s][b].action == MacroButtonAction::MqttSend) {
                if (!next->buttons[s][b].mqtt_topic[0]) {
                    free(next);
                    request->send(400, "application/json", "{\"success\":false,\"message\":\"mqtt_send requires mqtt_topic\"}");
                    return;
                }
            }

            // For non-MQTT actions, ignore stored mqtt_topic.
            if (next->buttons[s][b].action != MacroButtonAction::MqttSend) {
                next->buttons[s][b].mqtt_topic[0] = '\0';
            }

            // For non-payload actions, ignore stored payload.
            if (next->buttons[s][b].action != MacroButtonAction::SendKeys && next->buttons[s][b].action != MacroButtonAction::NavToScreen && next->buttons[s][b].action != MacroButtonAction::MqttSend) {
                next->buttons[s][b].payload[0] = '\0';
            }
        }
    }

    if (!macros_config_save(next)) {
        free(next);
#if MEMORY_SNAPSHOT_ON_HTTP_ENABLED
        device_telemetry_log_memory_snapshot("http_macros_post_save_fail");
#endif
        request->send(500, "application/json", "{\"success\":false,\"message\":\"Failed to save\"}");
        return;
    }

#if MEMORY_SNAPSHOT_ON_HTTP_ENABLED
    device_telemetry_log_memory_snapshot("http_macros_post_saved");
#endif

    g_macros_loaded = true;

    // Apply immediately to the runtime macro UI.
    memcpy(&macro_config, next, sizeof(MacroConfig));

    free(next);

#if MEMORY_SNAPSHOT_ON_HTTP_ENABLED
    device_telemetry_log_memory_snapshot("http_macros_post_applied");
#endif

    request->send(200, "application/json", "{\"success\":true}\n");
}

// ===== GitHub Releases firmware update (app-only) =====
static TaskHandle_t firmware_update_task_handle = nullptr;
static volatile bool firmware_update_in_progress = false;
static volatile size_t firmware_update_progress = 0;
static volatile size_t firmware_update_total = 0;

static char firmware_update_state[16] = "idle"; // idle|downloading|writing|rebooting|error
static char firmware_update_error[192] = "";
static char firmware_update_latest_version[24] = "";
static char firmware_update_download_url[512] = "";

static bool parse_semver_triplet(const char *s, int *major, int *minor, int *patch) {
    if (!s || !major || !minor || !patch) return false;

    // Accept optional leading 'v'
    if (s[0] == 'v' || s[0] == 'V') {
        s++;
    }

    int a = 0, b = 0, c = 0;
    if (sscanf(s, "%d.%d.%d", &a, &b, &c) != 3) {
        return false;
    }
    *major = a;
    *minor = b;
    *patch = c;
    return true;
}

static int compare_semver(const char *a, const char *b) {
    int am = 0, an = 0, ap = 0;
    int bm = 0, bn = 0, bp = 0;
    if (!parse_semver_triplet(a, &am, &an, &ap)) return 0;
    if (!parse_semver_triplet(b, &bm, &bn, &bp)) return 0;

    if (am != bm) return (am < bm) ? -1 : 1;
    if (an != bn) return (an < bn) ? -1 : 1;
    if (ap != bp) return (ap < bp) ? -1 : 1;
    return 0;
}

static bool github_fetch_latest_release(char *out_version, size_t out_version_len, char *out_asset_url, size_t out_asset_url_len, size_t *out_asset_size, char *out_error, size_t out_error_len) {
#if !GITHUB_UPDATES_ENABLED
    (void)out_version;
    (void)out_version_len;
    (void)out_asset_url;
    (void)out_asset_url_len;
    (void)out_asset_size;
    if (out_error && out_error_len > 0) {
        strlcpy(out_error, "GitHub updates disabled", out_error_len);
    }
    return false;
#else
    if (out_error && out_error_len > 0) out_error[0] = '\0';
    if (out_version && out_version_len > 0) out_version[0] = '\0';
    if (out_asset_url && out_asset_url_len > 0) out_asset_url[0] = '\0';
    if (out_asset_size) *out_asset_size = 0;

    if (WiFi.status() != WL_CONNECTED) {
        if (out_error && out_error_len > 0) {
            strlcpy(out_error, "WiFi not connected", out_error_len);
        }
        return false;
    }

    char api_url[256];
    snprintf(api_url, sizeof(api_url), "https://api.github.com/repos/%s/%s/releases/latest", GITHUB_OWNER, GITHUB_REPO);

    WiFiClientSecure client;
    client.setInsecure();
    client.setTimeout(15000);

    HTTPClient http;
    http.setTimeout(15000);

    if (!http.begin(client, api_url)) {
        if (out_error && out_error_len > 0) {
            strlcpy(out_error, "Failed to init HTTP client", out_error_len);
        }
        return false;
    }

    http.addHeader("User-Agent", "esp32-template-firmware");
    http.addHeader("Accept", "application/vnd.github+json");

    const int http_code = http.GET();
    if (http_code != 200) {
        if (out_error && out_error_len > 0) {
            snprintf(out_error, out_error_len, "GitHub API HTTP %d", http_code);
        }
        http.end();
        return false;
    }

    // Build expected app-only asset name: <project>-<board>-vX.Y.Z.bin
    const char *board = "unknown";
    #ifdef BUILD_BOARD_NAME
    board = BUILD_BOARD_NAME;
    #endif

    char expected_asset_name[160];
    expected_asset_name[0] = '\0';

    // Parse JSON with filter to reduce memory.
    // `assets` is an array, so use [0] to apply the filter to all elements.
    StaticJsonDocument<256> filter;
    filter["tag_name"] = true;
    filter["assets"][0]["name"] = true;
    filter["assets"][0]["browser_download_url"] = true;
    filter["assets"][0]["size"] = true;

    // Read the full response before parsing. Parsing directly from the stream can
    // occasionally fail with IncompleteInput if the connection stalls.
    String payload = http.getString();
    if (payload.length() == 0) {
        if (out_error && out_error_len > 0) {
            strlcpy(out_error, "GitHub API returned empty body", out_error_len);
        }
        http.end();
        return false;
    }

    // Prefer PSRAM for this relatively large document.
    BasicJsonDocument<MacrosJsonAllocator> doc(8192);
    if (doc.capacity() == 0) {
        if (out_error && out_error_len > 0) {
            strlcpy(out_error, "OOM (GitHub JSON doc allocation failed)", out_error_len);
        }
        http.end();
        return false;
    }
    DeserializationError err = deserializeJson(doc, payload, DeserializationOption::Filter(filter));
    if (err) {
        if (out_error && out_error_len > 0) {
            snprintf(out_error, out_error_len, "GitHub JSON parse error: %s", err.c_str());
        }
        http.end();
        return false;
    }

    const char *tag_name = doc["tag_name"] | "";
    if (!tag_name || strlen(tag_name) == 0) {
        if (out_error && out_error_len > 0) {
            strlcpy(out_error, "GitHub response missing tag_name", out_error_len);
        }
        http.end();
        return false;
    }

    // Strip leading 'v' for VERSION in filenames.
    const char *version = tag_name;
    if (version[0] == 'v' || version[0] == 'V') {
        version++;
    }

    snprintf(expected_asset_name, sizeof(expected_asset_name), "%s-%s-v%s.bin", PROJECT_NAME, board, version);

    JsonArray assets = doc["assets"].as<JsonArray>();
    const char *found_url = nullptr;
    size_t found_size = 0;

    for (JsonVariant v : assets) {
        const char *name = v["name"] | "";
        const char *url = v["browser_download_url"] | "";
        const size_t size = (size_t)(v["size"] | 0);
        if (name && url && strlen(name) > 0 && strcmp(name, expected_asset_name) == 0) {
            found_url = url;
            found_size = size;
            break;
        }
    }

    if (!found_url || strlen(found_url) == 0) {
        if (out_error && out_error_len > 0) {
            snprintf(out_error, out_error_len, "No asset found: %s", expected_asset_name);
        }
        http.end();
        return false;
    }

    if (out_version && out_version_len > 0) {
        strlcpy(out_version, version, out_version_len);
    }
    if (out_asset_url && out_asset_url_len > 0) {
        strlcpy(out_asset_url, found_url, out_asset_url_len);
    }
    if (out_asset_size) {
        *out_asset_size = found_size;
    }

    http.end();
    return true;
#endif
}

static void firmware_update_task(void *pv) {
    (void)pv;

    // Snapshot URL and size/version at task start.
    char url[sizeof(firmware_update_download_url)];
    char latest_version[sizeof(firmware_update_latest_version)];
    size_t expected_total = firmware_update_total;
    strlcpy(url, firmware_update_download_url, sizeof(url));
    strlcpy(latest_version, firmware_update_latest_version, sizeof(latest_version));

    firmware_update_progress = 0;
    strlcpy(firmware_update_state, "downloading", sizeof(firmware_update_state));
    firmware_update_error[0] = '\0';

    // Mark OTA in progress to block other OTA/image operations.
    ota_in_progress = true;

    WiFiClientSecure client;
    client.setInsecure();

    HTTPClient http;
    http.setTimeout(10000);
    http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);

    if (!http.begin(client, url)) {
        strlcpy(firmware_update_state, "error", sizeof(firmware_update_state));
        strlcpy(firmware_update_error, "Failed to init download", sizeof(firmware_update_error));
        firmware_update_in_progress = false;
        ota_in_progress = false;
        vTaskDelete(nullptr);
        return;
    }

    http.addHeader("User-Agent", "esp32-template-firmware");
    const int http_code = http.GET();
    if (http_code != 200) {
        strlcpy(firmware_update_state, "error", sizeof(firmware_update_state));
        snprintf(firmware_update_error, sizeof(firmware_update_error), "Download HTTP %d", http_code);
        http.end();
        firmware_update_in_progress = false;
        ota_in_progress = false;
        vTaskDelete(nullptr);
        return;
    }

    int http_len = http.getSize();
    if (http_len <= 0) {
        http_len = -1; // unknown length
    }
    size_t total = (http_len > 0) ? (size_t)http_len : expected_total;
    firmware_update_total = total;

    const size_t freeSpace = device_telemetry_free_sketch_space();
    if (total > 0 && total > freeSpace) {
        strlcpy(firmware_update_state, "error", sizeof(firmware_update_state));
        snprintf(firmware_update_error, sizeof(firmware_update_error), "Firmware too large (%u > %u)", (unsigned)total, (unsigned)freeSpace);
        http.end();
        firmware_update_in_progress = false;
        ota_in_progress = false;
        vTaskDelete(nullptr);
        return;
    }

    if (!Update.begin((total > 0) ? total : UPDATE_SIZE_UNKNOWN, U_FLASH)) {
        strlcpy(firmware_update_state, "error", sizeof(firmware_update_state));
        strlcpy(firmware_update_error, "OTA begin failed", sizeof(firmware_update_error));
        http.end();
        firmware_update_in_progress = false;
        ota_in_progress = false;
        vTaskDelete(nullptr);
        return;
    }

    strlcpy(firmware_update_state, "writing", sizeof(firmware_update_state));

    WiFiClient *stream = http.getStreamPtr();
    uint8_t* buf = nullptr;
#if SOC_SPIRAM_SUPPORTED
    if (psramFound()) {
        buf = (uint8_t*)heap_caps_malloc(2048, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
#endif
    if (!buf) {
        buf = (uint8_t*)heap_caps_malloc(2048, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    if (!buf) {
        strlcpy(firmware_update_state, "error", sizeof(firmware_update_state));
        strlcpy(firmware_update_error, "OOM (OTA buffer alloc failed)", sizeof(firmware_update_error));
        Update.abort();
        http.end();
        firmware_update_in_progress = false;
        ota_in_progress = false;
        vTaskDelete(nullptr);
        return;
    }

    while (http.connected() && (http_len > 0 || http_len == -1)) {
        const size_t available = stream->available();
        if (!available) {
            delay(1);
            continue;
        }

        const size_t to_read = (available > 2048) ? 2048 : available;
        const int read_bytes = stream->readBytes(buf, to_read);
        if (read_bytes <= 0) {
            break;
        }

        const size_t written = Update.write(buf, (size_t)read_bytes);
        if (written != (size_t)read_bytes) {
            strlcpy(firmware_update_state, "error", sizeof(firmware_update_state));
            strlcpy(firmware_update_error, "Flash write failed", sizeof(firmware_update_error));
            Update.abort();
            http.end();
            free(buf);
            firmware_update_in_progress = false;
            ota_in_progress = false;
            vTaskDelete(nullptr);
            return;
        }

        firmware_update_progress += written;
        if (http_len > 0) {
            http_len -= (int)read_bytes;
        }
    }

    http.end();
    free(buf);

    if (!Update.end(true)) {
        strlcpy(firmware_update_state, "error", sizeof(firmware_update_state));
        strlcpy(firmware_update_error, "OTA finalize failed", sizeof(firmware_update_error));
        firmware_update_in_progress = false;
        ota_in_progress = false;
        vTaskDelete(nullptr);
        return;
    }

    strlcpy(firmware_update_state, "rebooting", sizeof(firmware_update_state));
    (void)latest_version;

    // Give the HTTP response/polling a moment to observe completion.
    delay(300);
    ESP.restart();
    vTaskDelete(nullptr);
}

// GET /api/firmware/latest - Query GitHub releases/latest and compare with current firmware.
void handleGetFirmwareLatest(AsyncWebServerRequest *request) {
    if (!portal_auth_gate(request)) return;
#if !GITHUB_UPDATES_ENABLED
    request->send(404, "application/json", "{\"success\":false,\"message\":\"GitHub updates disabled\"}");
    return;
#else
    char latest[24];
    char url[512];
    size_t size = 0;
    char err[192];

    if (!github_fetch_latest_release(latest, sizeof(latest), url, sizeof(url), &size, err, sizeof(err))) {
        char resp[256];
        snprintf(resp, sizeof(resp), "{\"success\":false,\"message\":\"%s\"}", err[0] ? err : "Failed");
        request->send(500, "application/json", resp);
        return;
    }

    const bool update_available = (compare_semver(FIRMWARE_VERSION, latest) < 0);

    StaticJsonDocument<384> doc;
    doc["success"] = true;
    doc["current_version"] = FIRMWARE_VERSION;
    doc["latest_version"] = latest;
    doc["update_available"] = update_available;

    AsyncResponseStream *response = request->beginResponseStream("application/json");
    serializeJson(doc, *response);
    request->send(response);
#endif
}

// POST /api/firmware/update - Start background download+OTA of latest app-only firmware from GitHub.
void handlePostFirmwareUpdate(AsyncWebServerRequest *request) {
    if (!portal_auth_gate(request)) return;
#if !GITHUB_UPDATES_ENABLED
    request->send(404, "application/json", "{\"success\":false,\"message\":\"GitHub updates disabled\"}");
    return;
#else
    if (ota_in_progress || firmware_update_in_progress) {
        request->send(409, "application/json", "{\"success\":false,\"message\":\"Update already in progress\"}");
        return;
    }

    char latest[24];
    char url[512];
    size_t size = 0;
    char err[192];

    if (!github_fetch_latest_release(latest, sizeof(latest), url, sizeof(url), &size, err, sizeof(err))) {
        char resp[256];
        snprintf(resp, sizeof(resp), "{\"success\":false,\"message\":\"%s\"}", err[0] ? err : "Failed");
        request->send(500, "application/json", resp);
        return;
    }

    // If no update is available, still allow re-install? For now, require newer.
    if (compare_semver(FIRMWARE_VERSION, latest) >= 0) {
        request->send(200, "application/json", "{\"success\":true,\"message\":\"Already up to date\",\"update_started\":false}");
        return;
    }

    // Seed global state for status polling.
    firmware_update_in_progress = true;
    firmware_update_progress = 0;
    firmware_update_total = size;
    strlcpy(firmware_update_latest_version, latest, sizeof(firmware_update_latest_version));
    strlcpy(firmware_update_download_url, url, sizeof(firmware_update_download_url));
    firmware_update_error[0] = '\0';
    strlcpy(firmware_update_state, "downloading", sizeof(firmware_update_state));

    // Spawn background task to avoid blocking AsyncTCP.
    const BaseType_t ok = xTaskCreate(
        firmware_update_task,
        "fw_update",
        12288,
        nullptr,
        1,
        &firmware_update_task_handle
    );

    if (ok != pdPASS) {
        firmware_update_in_progress = false;
        strlcpy(firmware_update_state, "error", sizeof(firmware_update_state));
        strlcpy(firmware_update_error, "Failed to start update task", sizeof(firmware_update_error));
        request->send(500, "application/json", "{\"success\":false,\"message\":\"Failed to start update\"}");
        return;
    }

    StaticJsonDocument<256> doc;
    doc["success"] = true;
    doc["update_started"] = true;
    doc["current_version"] = FIRMWARE_VERSION;
    doc["latest_version"] = latest;

    AsyncResponseStream *response = request->beginResponseStream("application/json");
    serializeJson(doc, *response);
    request->send(response);
#endif
}

// GET /api/firmware/update/status - Progress snapshot for online update.
void handleGetFirmwareUpdateStatus(AsyncWebServerRequest *request) {
    if (!portal_auth_gate(request)) return;
    StaticJsonDocument<384> doc;
    doc["enabled"] = (GITHUB_UPDATES_ENABLED ? true : false);
    doc["in_progress"] = firmware_update_in_progress;
    doc["state"] = firmware_update_state;
    doc["progress"] = (uint32_t)firmware_update_progress;
    doc["total"] = (uint32_t)firmware_update_total;
    doc["latest_version"] = firmware_update_latest_version;
    doc["error"] = firmware_update_error;

    AsyncResponseStream *response = request->beginResponseStream("application/json");
    serializeJson(doc, *response);
    request->send(response);
}

#if HAS_IMAGE_API && HAS_DISPLAY
// AsyncWebServer callbacks run on the AsyncTCP task; never touch LVGL/display from there.
// Use this flag to defer "hide current image / return" operations to the main loop.
static volatile bool pending_image_hide_request = false;
#endif

// ===== WEB SERVER HANDLERS =====

#if MEMORY_SNAPSHOT_ON_HTTP_ENABLED
static bool s_logged_http_root = false;
static bool s_logged_http_network = false;
static bool s_logged_http_firmware = false;

// AsyncWebServer handlers run on the AsyncTCP task; log memory snapshots from
// the main loop (web_portal_handle) to avoid extra allocator pressure in the
// networking task.
static volatile bool s_pending_http_root = false;
static volatile bool s_pending_http_network = false;
static volatile bool s_pending_http_firmware = false;
#endif

static AsyncWebServerResponse *begin_gzipped_asset_response(
    AsyncWebServerRequest *request,
    const char *content_type,
    const uint8_t *content_gz,
    size_t content_gz_len,
    const char *cache_control
) {
    // Prefer the PROGMEM-aware response helper to avoid accidental heap copies.
    // All generated assets live in flash as `const uint8_t[] PROGMEM`.
    AsyncWebServerResponse *response = request->beginResponse_P(
        200,
        content_type,
        content_gz,
        content_gz_len
    );

    response->addHeader("Content-Encoding", "gzip");
    response->addHeader("Vary", "Accept-Encoding");
    if (cache_control && strlen(cache_control) > 0) {
        response->addHeader("Cache-Control", cache_control);
    }
    return response;
}

// Handle root - redirect to network.html in AP mode, serve home in full mode
void handleRoot(AsyncWebServerRequest *request) {
    if (!portal_auth_gate(request)) return;
    if (ap_mode_active) {
        // In AP mode, redirect to network configuration page
        request->redirect("/network.html");
    } else {
        // In full mode, serve home page
        AsyncWebServerResponse *response = begin_gzipped_asset_response(
            request,
            "text/html",
            home_html_gz,
            home_html_gz_len,
            "no-store"
        );
        request->send(response);

#if MEMORY_SNAPSHOT_ON_HTTP_ENABLED
        if (!s_logged_http_root) {
            s_pending_http_root = true;
        }
#endif
    }
}

// Serve home page
void handleHome(AsyncWebServerRequest *request) {
    if (!portal_auth_gate(request)) return;
    if (ap_mode_active) {
        // In AP mode, redirect to network configuration page
        request->redirect("/network.html");
        return;
    }
    AsyncWebServerResponse *response = begin_gzipped_asset_response(
        request,
        "text/html",
        home_html_gz,
        home_html_gz_len,
        "no-store"
    );
    request->send(response);
}

// Serve network configuration page
void handleNetwork(AsyncWebServerRequest *request) {
    if (!portal_auth_gate(request)) return;
    AsyncWebServerResponse *response = begin_gzipped_asset_response(
        request,
        "text/html",
        network_html_gz,
        network_html_gz_len,
        "no-store"
    );
    request->send(response);

#if MEMORY_SNAPSHOT_ON_HTTP_ENABLED
    if (!s_logged_http_network) {
        s_pending_http_network = true;
    }
#endif
}

// Serve firmware update page
void handleFirmware(AsyncWebServerRequest *request) {
    if (!portal_auth_gate(request)) return;
    if (ap_mode_active) {
        // In AP mode, redirect to network configuration page
        request->redirect("/network.html");
        return;
    }
    AsyncWebServerResponse *response = begin_gzipped_asset_response(
        request,
        "text/html",
        firmware_html_gz,
        firmware_html_gz_len,
        "no-store"
    );
    request->send(response);

#if MEMORY_SNAPSHOT_ON_HTTP_ENABLED
    if (!s_logged_http_firmware) {
        s_pending_http_firmware = true;
    }
#endif
}

// Serve CSS
void handleCSS(AsyncWebServerRequest *request) {
    AsyncWebServerResponse *response = begin_gzipped_asset_response(
        request,
        "text/css",
        portal_css_gz,
        portal_css_gz_len,
        "public, max-age=600"
    );
    request->send(response);
}

// Serve JavaScript
void handleJS(AsyncWebServerRequest *request) {
    AsyncWebServerResponse *response = begin_gzipped_asset_response(
        request,
        "application/javascript",
        portal_js_gz,
        portal_js_gz_len,
        "public, max-age=600"
    );
    request->send(response);
}

// GET /api/mode - Return portal mode (core vs full)
void handleGetMode(AsyncWebServerRequest *request) {
    if (!portal_auth_gate(request)) return;
    
    AsyncResponseStream *response = request->beginResponseStream("application/json");
    response->print("{\"mode\":\"");
    response->print(ap_mode_active ? "core" : "full");
    response->print("\",\"ap_active\":");
    response->print(ap_mode_active ? "true" : "false");
    response->print("}");
    request->send(response);
}

// GET /api/config - Return current configuration
void handleGetConfig(AsyncWebServerRequest *request) {
    if (!portal_auth_gate(request)) return;
    
    if (!current_config) {
        request->send(500, "application/json", "{\"error\":\"Config not initialized\"}");
        return;
    }
    
    // Create JSON response (don't include passwords)
    // NOTE: AsyncWebServer handlers execute on the AsyncTCP task; avoid large stack allocations.
    static constexpr size_t kConfigJsonDocCapacity = 2304;
    BasicJsonDocument<MacrosJsonAllocator> doc(kConfigJsonDocCapacity);
    if (doc.capacity() == 0) {
        Logger.logMessage("Portal", "ERROR: /api/config OOM (JsonDocument allocation failed)");
        request->send(503, "application/json", "{\"success\":false,\"message\":\"Out of memory\"}");
        return;
    }
    doc["wifi_ssid"] = current_config->wifi_ssid;
    doc["wifi_password"] = ""; // Don't send password
    doc["device_name"] = current_config->device_name;
    
    // Sanitized name for display
    char sanitized[CONFIG_DEVICE_NAME_MAX_LEN];
    config_manager_sanitize_device_name(current_config->device_name, sanitized, CONFIG_DEVICE_NAME_MAX_LEN);
    doc["device_name_sanitized"] = sanitized;
    
    // Fixed IP settings
    doc["fixed_ip"] = current_config->fixed_ip;
    doc["subnet_mask"] = current_config->subnet_mask;
    doc["gateway"] = current_config->gateway;
    doc["dns1"] = current_config->dns1;
    doc["dns2"] = current_config->dns2;
    
    // Dummy setting
    doc["dummy_setting"] = current_config->dummy_setting;

    // MQTT settings (password not returned)
    doc["mqtt_host"] = current_config->mqtt_host;
    doc["mqtt_port"] = current_config->mqtt_port;
    doc["mqtt_username"] = current_config->mqtt_username;
    doc["mqtt_password"] = "";
    doc["mqtt_interval_seconds"] = current_config->mqtt_interval_seconds;

    // Web portal Basic Auth (password not returned)
    doc["basic_auth_enabled"] = current_config->basic_auth_enabled;
    doc["basic_auth_username"] = current_config->basic_auth_username;
    doc["basic_auth_password"] = "";
    doc["basic_auth_password_set"] = (strlen(current_config->basic_auth_password) > 0);
    
    // Display settings
    doc["backlight_brightness"] = current_config->backlight_brightness;

    #if HAS_DISPLAY
    // Screen saver settings
    doc["screen_saver_enabled"] = current_config->screen_saver_enabled;
    doc["screen_saver_timeout_seconds"] = current_config->screen_saver_timeout_seconds;
    doc["screen_saver_fade_out_ms"] = current_config->screen_saver_fade_out_ms;
    doc["screen_saver_fade_in_ms"] = current_config->screen_saver_fade_in_ms;
    doc["screen_saver_wake_on_touch"] = current_config->screen_saver_wake_on_touch;
    #endif

    if (doc.overflowed()) {
        Logger.logMessage("Portal", "ERROR: /api/config JSON overflow (StaticJsonDocument too small)");
        request->send(500, "application/json", "{\"success\":false,\"message\":\"Response too large\"}");
        return;
    }

    AsyncResponseStream *response = request->beginResponseStream("application/json");
    serializeJson(doc, *response);
    request->send(response);
}

// POST /api/config - Save new configuration
void handlePostConfig(AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total) {
    if (!portal_auth_gate(request)) return;
    
    if (!current_config) {
        request->send(500, "application/json", "{\"success\":false,\"message\":\"Config not initialized\"}");
        return;
    }
    
    // Parse JSON body
    // NOTE: AsyncWebServer handlers execute on the AsyncTCP task; avoid large stack allocations.
    static constexpr size_t kConfigJsonDocCapacity = 2304;
    BasicJsonDocument<MacrosJsonAllocator> doc(kConfigJsonDocCapacity);
    if (doc.capacity() == 0) {
        Logger.logMessage("Portal", "ERROR: /api/config OOM (JsonDocument allocation failed)");
        request->send(503, "application/json", "{\"success\":false,\"message\":\"Out of memory\"}");
        return;
    }
    DeserializationError error = deserializeJson(doc, data, len);
    
    if (error) {
        Logger.logMessagef("Portal", "JSON parse error: %s", error.c_str());
        if (error == DeserializationError::NoMemory) {
            request->send(413, "application/json", "{\"success\":false,\"message\":\"JSON body too large\"}");
            return;
        }
        request->send(400, "application/json", "{\"success\":false,\"message\":\"Invalid JSON\"}");
        return;
    }
    
    // Partial update: only update fields that are present in the request
    // This allows different pages to update only their relevant fields

    // Security hardening: never allow changing Basic Auth settings in AP/core mode.
    // Otherwise, an attacker near the device could wait for fallback AP mode and lock out the owner.
    if (ap_mode_active && (doc.containsKey("basic_auth_enabled") || doc.containsKey("basic_auth_username") || doc.containsKey("basic_auth_password"))) {
        request->send(403, "application/json", "{\"success\":false,\"message\":\"Basic Auth settings cannot be changed in AP mode\"}");
        return;
    }
    
    // WiFi SSID - only update if field exists in JSON
    if (doc.containsKey("wifi_ssid")) {
        strlcpy(current_config->wifi_ssid, doc["wifi_ssid"] | "", CONFIG_SSID_MAX_LEN);
    }
    
    // WiFi password - only update if provided and not empty
    if (doc.containsKey("wifi_password")) {
        const char* wifi_pass = doc["wifi_password"];
        if (wifi_pass && strlen(wifi_pass) > 0) {
            strlcpy(current_config->wifi_password, wifi_pass, CONFIG_PASSWORD_MAX_LEN);
        }
    }
    
    // Device name - only update if field exists
    if (doc.containsKey("device_name")) {
        const char* device_name = doc["device_name"];
        if (device_name && strlen(device_name) > 0) {
            strlcpy(current_config->device_name, device_name, CONFIG_DEVICE_NAME_MAX_LEN);
        }
    }
    
    // Fixed IP settings - only update if fields exist
    if (doc.containsKey("fixed_ip")) {
        strlcpy(current_config->fixed_ip, doc["fixed_ip"] | "", CONFIG_IP_STR_MAX_LEN);
    }
    if (doc.containsKey("subnet_mask")) {
        strlcpy(current_config->subnet_mask, doc["subnet_mask"] | "", CONFIG_IP_STR_MAX_LEN);
    }
    if (doc.containsKey("gateway")) {
        strlcpy(current_config->gateway, doc["gateway"] | "", CONFIG_IP_STR_MAX_LEN);
    }
    if (doc.containsKey("dns1")) {
        strlcpy(current_config->dns1, doc["dns1"] | "", CONFIG_IP_STR_MAX_LEN);
    }
    if (doc.containsKey("dns2")) {
        strlcpy(current_config->dns2, doc["dns2"] | "", CONFIG_IP_STR_MAX_LEN);
    }
    
    // Dummy setting - only update if field exists
    if (doc.containsKey("dummy_setting")) {
        strlcpy(current_config->dummy_setting, doc["dummy_setting"] | "", CONFIG_DUMMY_MAX_LEN);
    }

    // MQTT host
    if (doc.containsKey("mqtt_host")) {
        strlcpy(current_config->mqtt_host, doc["mqtt_host"] | "", CONFIG_MQTT_HOST_MAX_LEN);
    }

    // MQTT port (optional; 0 means default 1883)
    if (doc.containsKey("mqtt_port")) {
        if (doc["mqtt_port"].is<const char*>()) {
            const char* port_str = doc["mqtt_port"];
            current_config->mqtt_port = (uint16_t)atoi(port_str ? port_str : "0");
        } else {
            current_config->mqtt_port = (uint16_t)(doc["mqtt_port"] | 0);
        }
    }

    // MQTT username
    if (doc.containsKey("mqtt_username")) {
        strlcpy(current_config->mqtt_username, doc["mqtt_username"] | "", CONFIG_MQTT_USERNAME_MAX_LEN);
    }

    // MQTT password (only update if provided and not empty)
    if (doc.containsKey("mqtt_password")) {
        const char* mqtt_pass = doc["mqtt_password"];
        if (mqtt_pass && strlen(mqtt_pass) > 0) {
            strlcpy(current_config->mqtt_password, mqtt_pass, CONFIG_MQTT_PASSWORD_MAX_LEN);
        }
    }

    // MQTT interval seconds
    if (doc.containsKey("mqtt_interval_seconds")) {
        if (doc["mqtt_interval_seconds"].is<const char*>()) {
            const char* int_str = doc["mqtt_interval_seconds"];
            current_config->mqtt_interval_seconds = (uint16_t)atoi(int_str ? int_str : "0");
        } else {
            current_config->mqtt_interval_seconds = (uint16_t)(doc["mqtt_interval_seconds"] | 0);
        }
    }

    // Basic Auth enabled
    if (doc.containsKey("basic_auth_enabled")) {
        if (doc["basic_auth_enabled"].is<const char*>()) {
            const char* v = doc["basic_auth_enabled"];
            current_config->basic_auth_enabled = (v && (strcmp(v, "1") == 0 || strcasecmp(v, "true") == 0 || strcasecmp(v, "on") == 0));
        } else {
            current_config->basic_auth_enabled = (bool)(doc["basic_auth_enabled"] | false);
        }
    }

    // Basic Auth username
    if (doc.containsKey("basic_auth_username")) {
        strlcpy(current_config->basic_auth_username, doc["basic_auth_username"] | "", CONFIG_BASIC_AUTH_USERNAME_MAX_LEN);
    }

    // Basic Auth password (only update if provided and not empty)
    if (doc.containsKey("basic_auth_password")) {
        const char* pass = doc["basic_auth_password"];
        if (pass && strlen(pass) > 0) {
            strlcpy(current_config->basic_auth_password, pass, CONFIG_BASIC_AUTH_PASSWORD_MAX_LEN);
        }
    }
    
    // Display settings - backlight brightness (0-100%)
    if (doc.containsKey("backlight_brightness")) {
        uint8_t brightness;
        // Handle both string and integer values from form
        if (doc["backlight_brightness"].is<const char*>()) {
            const char* brightness_str = doc["backlight_brightness"];
            brightness = (uint8_t)atoi(brightness_str ? brightness_str : "100");
        } else {
            brightness = (uint8_t)(doc["backlight_brightness"] | 100);
        }
        
        if (brightness > 100) brightness = 100;
        current_config->backlight_brightness = brightness;
        
        Logger.logLinef("Config: Backlight brightness set to %d%%", brightness);
        
        // Apply brightness immediately (will also be persisted when config saved)
        #if HAS_DISPLAY
        display_manager_set_backlight_brightness(brightness);

        // Edge case: if the device was in screen saver (backlight at 0), changing brightness
        // externally would light the screen without updating the screen saver state.
        // Treat this as explicit activity+wake so auto-sleep keeps working.
        screen_saver_manager_notify_activity(true);
        #endif
    }

    #if HAS_DISPLAY
    // Screen saver settings
    if (doc.containsKey("screen_saver_enabled")) {
        if (doc["screen_saver_enabled"].is<const char*>()) {
            const char* v = doc["screen_saver_enabled"];
            current_config->screen_saver_enabled = (v && (strcmp(v, "1") == 0 || strcasecmp(v, "true") == 0 || strcasecmp(v, "on") == 0));
        } else {
            current_config->screen_saver_enabled = (bool)(doc["screen_saver_enabled"] | false);
        }
    }

    if (doc.containsKey("screen_saver_timeout_seconds")) {
        if (doc["screen_saver_timeout_seconds"].is<const char*>()) {
            const char* v = doc["screen_saver_timeout_seconds"];
            current_config->screen_saver_timeout_seconds = (uint16_t)atoi(v ? v : "0");
        } else {
            current_config->screen_saver_timeout_seconds = (uint16_t)(doc["screen_saver_timeout_seconds"] | 0);
        }
    }

    if (doc.containsKey("screen_saver_fade_out_ms")) {
        if (doc["screen_saver_fade_out_ms"].is<const char*>()) {
            const char* v = doc["screen_saver_fade_out_ms"];
            current_config->screen_saver_fade_out_ms = (uint16_t)atoi(v ? v : "0");
        } else {
            current_config->screen_saver_fade_out_ms = (uint16_t)(doc["screen_saver_fade_out_ms"] | 0);
        }
    }

    if (doc.containsKey("screen_saver_fade_in_ms")) {
        if (doc["screen_saver_fade_in_ms"].is<const char*>()) {
            const char* v = doc["screen_saver_fade_in_ms"];
            current_config->screen_saver_fade_in_ms = (uint16_t)atoi(v ? v : "0");
        } else {
            current_config->screen_saver_fade_in_ms = (uint16_t)(doc["screen_saver_fade_in_ms"] | 0);
        }
    }

    if (doc.containsKey("screen_saver_wake_on_touch")) {
        if (doc["screen_saver_wake_on_touch"].is<const char*>()) {
            const char* v = doc["screen_saver_wake_on_touch"];
            current_config->screen_saver_wake_on_touch = (v && (strcmp(v, "1") == 0 || strcasecmp(v, "true") == 0 || strcasecmp(v, "on") == 0));
        } else {
            current_config->screen_saver_wake_on_touch = (bool)(doc["screen_saver_wake_on_touch"] | false);
        }
    }
    #endif
    
    current_config->magic = CONFIG_MAGIC;
    
    // Validate config
    if (!config_manager_is_valid(current_config)) {
        request->send(400, "application/json", "{\"success\":false,\"message\":\"Invalid configuration\"}");
        return;
    }
    
    // Save to NVS
    if (config_manager_save(current_config)) {
        Logger.logMessage("Portal", "Config saved");
        request->send(200, "application/json", "{\"success\":true,\"message\":\"Configuration saved\"}");
        
        // Check for no_reboot parameter
        if (!request->hasParam("no_reboot")) {
            Logger.logMessage("Portal", "Rebooting device");
            // Schedule reboot after response is sent
            delay(100);
            ESP.restart();
        }
    } else {
        Logger.logMessage("Portal", "Config save failed");
        request->send(500, "application/json", "{\"success\":false,\"message\":\"Failed to save\"}");
    }
}

// DELETE /api/config - Reset configuration
void handleDeleteConfig(AsyncWebServerRequest *request) {
    if (!portal_auth_gate(request)) return;
    
    if (config_manager_reset()) {
        request->send(200, "application/json", "{\"success\":true,\"message\":\"Configuration reset\"}");
        
        // Schedule reboot after response is sent
        delay(100);
        ESP.restart();
    } else {
        request->send(500, "application/json", "{\"success\":false,\"message\":\"Failed to reset\"}");
    }
}

// GET /api/info - Get device information
void handleGetVersion(AsyncWebServerRequest *request) {
    if (!portal_auth_gate(request)) return;

    // Build JSON via ArduinoJson (safe escaping), then stream it using chunked response.
    // NOTE: AsyncWebServer handlers execute on the AsyncTCP task; avoid large stack allocations.
    static constexpr size_t kInfoJsonDocCapacity = 4096;
    BasicJsonDocument<MacrosJsonAllocator> doc(kInfoJsonDocCapacity);
    if (doc.capacity() == 0) {
        request->send(503, "application/json", "{\"success\":false,\"message\":\"Out of memory\"}");
        return;
    }

    doc["version"] = FIRMWARE_VERSION;
    doc["build_date"] = BUILD_DATE;
    doc["build_time"] = BUILD_TIME;
    doc["chip_model"] = ESP.getChipModel();
    doc["chip_revision"] = ESP.getChipRevision();
    doc["chip_cores"] = ESP.getChipCores();
    doc["cpu_freq"] = ESP.getCpuFreqMHz();
    doc["flash_chip_size"] = ESP.getFlashChipSize();
    doc["psram_size"] = ESP.getPsramSize();
    doc["free_heap"] = ESP.getFreeHeap();
    doc["sketch_size"] = device_telemetry_sketch_size();
    doc["free_sketch_space"] = device_telemetry_free_sketch_space();
    doc["mac_address"] = WiFi.macAddress();
    doc["wifi_hostname"] = WiFi.getHostname();

    String mdns;
    mdns.reserve(64);
    mdns += WiFi.getHostname();
    mdns += ".local";
    doc["mdns_name"] = mdns;
    doc["hostname"] = WiFi.getHostname();
    doc["project_name"] = PROJECT_NAME;
    doc["project_display_name"] = PROJECT_DISPLAY_NAME;

    // Build metadata for GitHub-based updates
    #ifdef BUILD_BOARD_NAME
    doc["board_name"] = BUILD_BOARD_NAME;
    #else
    doc["board_name"] = "unknown";
    #endif
    doc["github_updates_enabled"] = (GITHUB_UPDATES_ENABLED ? true : false);
    #if GITHUB_UPDATES_ENABLED
    doc["github_owner"] = GITHUB_OWNER;
    doc["github_repo"] = GITHUB_REPO;
    #endif

    doc["has_mqtt"] = (HAS_MQTT ? true : false);
    doc["has_backlight"] = (HAS_BACKLIGHT ? true : false);

    #if HAS_DISPLAY
    doc["has_display"] = true;

    // Display resolution (driver coordinate space for direct writes / image upload)
    int display_coord_width = DISPLAY_WIDTH;
    int display_coord_height = DISPLAY_HEIGHT;
    if (displayManager && displayManager->getDriver()) {
        display_coord_width = displayManager->getDriver()->width();
        display_coord_height = displayManager->getDriver()->height();
    }
    doc["display_coord_width"] = display_coord_width;
    doc["display_coord_height"] = display_coord_height;

    // Get available screens
    size_t screen_count = 0;
    const ScreenInfo* screens = display_manager_get_available_screens(&screen_count);
    JsonArray arr = doc.createNestedArray("available_screens");
    for (size_t i = 0; i < screen_count; i++) {
        JsonObject o = arr.createNestedObject();
        o["id"] = screens[i].id;
        o["name"] = screens[i].display_name;
    }

    // Get current screen
    const char* current_screen = display_manager_get_current_screen_id();
    if (current_screen) {
        doc["current_screen"] = current_screen;
    } else {
        doc["current_screen"] = nullptr;
    }
    #else
    doc["has_display"] = false;
    #endif

    if (doc.overflowed()) {
        Logger.logMessage("Portal", "ERROR: /api/info JSON overflow (document too small)");
        request->send(500, "application/json", "{\"success\":false,\"message\":\"Response too large\"}");
        return;
    }

    send_json_doc_chunked(request, doc, 503);
}

// GET /api/health - Get device health statistics
void handleGetHealth(AsyncWebServerRequest *request) {
    if (!portal_auth_gate(request)) return;
    BasicJsonDocument<MacrosJsonAllocator> doc(1024);
    if (doc.capacity() == 0) {
        request->send(503, "application/json", "{\"success\":false,\"message\":\"Out of memory\"}");
        return;
    }

    device_telemetry_fill_api(doc);

    if (doc.overflowed()) {
        Logger.logMessage("Portal", "ERROR: /api/health JSON overflow (StaticJsonDocument too small)");
        request->send(500, "application/json", "{\"success\":false,\"message\":\"Response too large\"}");
        return;
    }

    send_json_doc_chunked(request, doc, 503);
}

// POST /api/reboot - Reboot device without saving
void handleReboot(AsyncWebServerRequest *request) {
    if (!portal_auth_gate(request)) return;
    Logger.logMessage("API", "POST /api/reboot");
    
    request->send(200, "application/json", "{\"success\":true,\"message\":\"Rebooting device...\"}");
    
    // Schedule reboot after response is sent
    delay(100);
    Logger.logMessage("Portal", "Rebooting");
    ESP.restart();
}

// PUT /api/display/brightness - Set backlight brightness immediately (no persist)
void handleSetDisplayBrightness(AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total) {
    if (!portal_auth_gate(request)) return;
    // Only handle the complete request (index == 0 && index + len == total)
    if (index != 0 || index + len != total) {
        return;
    }
    
    // Parse JSON body
    StaticJsonDocument<256> doc;
    DeserializationError error = deserializeJson(doc, data, len);
    
    if (error) {
        request->send(400, "application/json", "{\"success\":false,\"message\":\"Invalid JSON\"}");
        return;
    }
    
    // Get brightness value (0-100)
    if (!doc.containsKey("brightness")) {
        request->send(400, "application/json", "{\"success\":false,\"message\":\"Missing brightness value\"}");
        return;
    }
    
    uint8_t brightness = (uint8_t)(doc["brightness"] | 100);
    if (brightness > 100) brightness = 100;
    
    #if HAS_DISPLAY
    // Update the in-RAM target brightness (does not persist to NVS).
    // This keeps the screen saver target consistent with what the user sees.
    if (current_config) {
        current_config->backlight_brightness = brightness;
    }

    // Edge case: if the screen saver is dimming/asleep/fading, directly setting the
    // backlight would show the UI again without updating the screen saver state.
    // Easiest fix: when not Awake, route through the screen saver wake path.
    const ScreenSaverState state = screen_saver_manager_get_status().state;
    if (state != ScreenSaverState::Awake) {
        screen_saver_manager_wake();
    } else {
        display_manager_set_backlight_brightness(brightness);
        screen_saver_manager_notify_activity(false);
    }

    #endif
    
    // Return success
    char response[64];
    snprintf(response, sizeof(response), "{\"success\":true,\"brightness\":%d}", brightness);
    request->send(200, "application/json", response);
}

// GET /api/display/sleep - Screen saver status snapshot
void handleGetDisplaySleep(AsyncWebServerRequest *request) {
    if (!portal_auth_gate(request)) return;
    #if HAS_DISPLAY
    ScreenSaverStatus status = screen_saver_manager_get_status();

    StaticJsonDocument<256> doc;
    doc["enabled"] = status.enabled;
    doc["state"] = (uint8_t)status.state;
    doc["current_brightness"] = status.current_brightness;
    doc["target_brightness"] = status.target_brightness;
    doc["seconds_until_sleep"] = status.seconds_until_sleep;

    AsyncResponseStream *response = request->beginResponseStream("application/json");
    serializeJson(doc, *response);
    request->send(response);
    #else
    request->send(404, "application/json", "{\"success\":false,\"message\":\"No display\"}");
    #endif
}

// POST /api/display/sleep - Sleep now
void handlePostDisplaySleep(AsyncWebServerRequest *request) {
    if (!portal_auth_gate(request)) return;
    #if HAS_DISPLAY
    Logger.logMessage("API", "POST /api/display/sleep");
    screen_saver_manager_sleep_now();
    request->send(200, "application/json", "{\"success\":true}");
    #else
    request->send(404, "application/json", "{\"success\":false,\"message\":\"No display\"}");
    #endif
}

// POST /api/display/wake - Wake now
void handlePostDisplayWake(AsyncWebServerRequest *request) {
    if (!portal_auth_gate(request)) return;
    #if HAS_DISPLAY
    Logger.logMessage("API", "POST /api/display/wake");
    screen_saver_manager_wake();
    request->send(200, "application/json", "{\"success\":true}");
    #else
    request->send(404, "application/json", "{\"success\":false,\"message\":\"No display\"}");
    #endif
}

// POST /api/display/activity - Reset idle timer; optionally wake
void handlePostDisplayActivity(AsyncWebServerRequest *request) {
    if (!portal_auth_gate(request)) return;
    #if HAS_DISPLAY
    bool wake = false;
    if (request->hasParam("wake")) {
        wake = (request->getParam("wake")->value() == "1");
    }
    Logger.logMessagef("API", "POST /api/display/activity (wake=%d)", (int)wake);
    screen_saver_manager_notify_activity(wake);
    request->send(200, "application/json", "{\"success\":true}");
    #else
    request->send(404, "application/json", "{\"success\":false,\"message\":\"No display\"}");
    #endif
}

#if HAS_DISPLAY
// PUT /api/display/screen - Switch to a different screen (runtime only, no persist)
void handleSetDisplayScreen(AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total) {
    if (!portal_auth_gate(request)) return;
    // Only handle the complete request (index == 0 && index + len == total)
    if (index != 0 || index + len != total) {
        return;
    }
    
    // Parse JSON body
    StaticJsonDocument<256> doc;
    DeserializationError error = deserializeJson(doc, data, len);
    
    if (error) {
        request->send(400, "application/json", "{\"success\":false,\"message\":\"Invalid JSON\"}");
        return;
    }
    
    // Get screen ID
    if (!doc.containsKey("screen")) {
        request->send(400, "application/json", "{\"success\":false,\"message\":\"Missing screen ID\"}");
        return;
    }
    
    const char* screen_id = doc["screen"];
    if (!screen_id || strlen(screen_id) == 0) {
        request->send(400, "application/json", "{\"success\":false,\"message\":\"Invalid screen ID\"}");
        return;
    }
    
    Logger.logMessagef("API", "PUT /api/display/screen: %s", screen_id);
    
    // Switch to requested screen
    bool success = false;
    display_manager_show_screen(screen_id, &success);

    // Screen-affecting action counts as explicit activity and should wake.
    if (success) {
        screen_saver_manager_notify_activity(true);
    }
    
    if (success) {
        // Build success response with new screen ID
        char response[96];
        snprintf(response, sizeof(response), "{\"success\":true,\"screen\":\"%s\"}", screen_id);
        request->send(200, "application/json", response);
    } else {
        request->send(404, "application/json", "{\"success\":false,\"message\":\"Screen not found\"}");
    }
}
#endif

// POST /api/update - Handle OTA firmware upload
void handleOTAUpload(AsyncWebServerRequest *request, String filename, size_t index, uint8_t *data, size_t len, bool final) {
    if (!portal_auth_gate(request)) return;
    // First chunk - initialize OTA
    if (index == 0) {

        
        Logger.logBegin("OTA Update");
        Logger.logLinef("File: %s", filename.c_str());
        Logger.logLinef("Size: %d bytes", request->contentLength());
        
        ota_in_progress = true;
        ota_progress = 0;
        ota_total = request->contentLength();
        
        // Check if filename ends with .bin
        if (!filename.endsWith(".bin")) {
            Logger.logEnd("Not a .bin file");
            request->send(400, "application/json", "{\"success\":false,\"message\":\"Only .bin files are supported\"}");
            ota_in_progress = false;
            return;
        }
        
        // Get OTA partition size
        size_t updateSize = (ota_total > 0) ? ota_total : UPDATE_SIZE_UNKNOWN;
        size_t freeSpace = device_telemetry_free_sketch_space();
        
        Logger.logLinef("Free space: %d bytes", freeSpace);
        
        // Validate size before starting
        if (ota_total > 0 && ota_total > freeSpace) {
            Logger.logEnd("Firmware too large");
            request->send(400, "application/json", "{\"success\":false,\"message\":\"Firmware too large\"}");
            ota_in_progress = false;
            return;
        }
        
        // Begin OTA update
        if (!Update.begin(updateSize, U_FLASH)) {
            Logger.logEnd("Begin failed");
            Update.printError(Serial);
            request->send(500, "application/json", "{\"success\":false,\"message\":\"OTA begin failed\"}");
            ota_in_progress = false;
            return;
        }
    }
    
    // Write chunk to flash
    if (len) {
        if (Update.write(data, len) != len) {
            Logger.logEnd("Write failed");
            Update.printError(Serial);
            request->send(500, "application/json", "{\"success\":false,\"message\":\"Write failed\"}");
            ota_in_progress = false;
            return;
        }
        
        ota_progress += len;
        
        // Print progress every 10%
        static uint8_t last_percent = 0;
        uint8_t percent = (ota_progress * 100) / ota_total;
        if (percent >= last_percent + 10) {
            Logger.logLinef("Progress: %d%%", percent);
            last_percent = percent;
        }
    }
    
    // Final chunk - complete OTA
    if (final) {
        if (Update.end(true)) {
            Logger.logLinef("Written: %d bytes", ota_progress);
            Logger.logEnd("Success - rebooting");
            
            request->send(200, "application/json", "{\"success\":true,\"message\":\"Update successful! Rebooting...\"}");
            
            delay(500);
            ESP.restart();
        } else {
            Logger.logEnd("Update failed");
            Update.printError(Serial);
            request->send(500, "application/json", "{\"success\":false,\"message\":\"Update failed\"}");
        }
        
        ota_in_progress = false;
    }
}

// ===== PUBLIC API =====

// Initialize web portal
void web_portal_init(DeviceConfig *config) {
    Logger.logBegin("Portal Init");
    
    current_config = config;

    // Load macros config once at portal init so GET /api/macros is cheap.
    macros_cache_load_if_needed();
    
    // Create web server instance (avoid global constructor issues)
    if (server == nullptr) {
        yield();
        delay(100);
        
        server = new AsyncWebServer(80);
        
        yield();
        delay(100);
    }

    // Page routes
    server->on("/", HTTP_GET, handleRoot);
    server->on("/home.html", HTTP_GET, handleHome);
    server->on("/network.html", HTTP_GET, handleNetwork);
    server->on("/firmware.html", HTTP_GET, handleFirmware);
    
    // Asset routes
    server->on("/portal.css", HTTP_GET, handleCSS);
    server->on("/portal.js", HTTP_GET, handleJS);
    
    // API endpoints
    server->on("/api/mode", HTTP_GET, handleGetMode);
    server->on("/api/config", HTTP_GET, handleGetConfig);

    // Icons API (for macro icon selector)
    // NOTE: register more specific routes first; some AsyncWebServer URI matchers behave like prefix matches.
    server->on("/api/icons/installed", HTTP_GET, handleGetInstalledIcons);
    server->on("/api/icons/gc", HTTP_POST, handlePostIconGC);
    server->on("/api/icons/install", HTTP_POST,
        [](AsyncWebServerRequest *request) {
            if (!portal_auth_gate(request)) return;
        },
        NULL,
        handlePostIconInstall
    );
    server->on("/api/icons", HTTP_GET, handleGetIcons);

    // Macros API
    server->on("/api/macros", HTTP_GET, handleGetMacros);
    server->on("/api/macros", HTTP_POST,
        [](AsyncWebServerRequest *request) {
            if (!portal_auth_gate(request)) return;
        },
        NULL,
        handlePostMacros
    );
    
    server->on("/api/config", HTTP_POST, 
        [](AsyncWebServerRequest *request) {
            if (!portal_auth_gate(request)) return;
        },
        NULL,
        handlePostConfig
    );
    
    server->on("/api/config", HTTP_DELETE, handleDeleteConfig);
    server->on("/api/info", HTTP_GET, handleGetVersion);
    server->on("/api/health", HTTP_GET, handleGetHealth);
    server->on("/api/reboot", HTTP_POST, handleReboot);

    // GitHub Releases-based firmware updates (stable only)
    server->on("/api/firmware/latest", HTTP_GET, handleGetFirmwareLatest);
    server->on("/api/firmware/update", HTTP_POST, handlePostFirmwareUpdate);
    server->on("/api/firmware/update/status", HTTP_GET, handleGetFirmwareUpdateStatus);
    
    #if HAS_DISPLAY
    // Display API endpoints
    server->on("/api/display/brightness", HTTP_PUT,
        [](AsyncWebServerRequest *request) {
            if (!portal_auth_gate(request)) return;
        },
        NULL,
        handleSetDisplayBrightness
    );

    // Screen saver API endpoints
    server->on("/api/display/sleep", HTTP_GET, handleGetDisplaySleep);
    server->on("/api/display/sleep", HTTP_POST, handlePostDisplaySleep);
    server->on("/api/display/wake", HTTP_POST, handlePostDisplayWake);
    server->on("/api/display/activity", HTTP_POST, handlePostDisplayActivity);

    // Runtime-only screen switch
    server->on("/api/display/screen", HTTP_PUT,
        [](AsyncWebServerRequest *request) {
            if (!portal_auth_gate(request)) return;
        },
        NULL,
        handleSetDisplayScreen
    );
    #endif
    
    // OTA upload endpoint
    server->on("/api/update", HTTP_POST,
        [](AsyncWebServerRequest *request) {
            if (!portal_auth_gate(request)) return;
        },
        handleOTAUpload
    );
    
    // Image API integration (if enabled)
    #if HAS_IMAGE_API && HAS_DISPLAY
    Logger.logMessage("Portal", "Initializing image API");
    
    // Setup backend adapter
    ImageApiBackend backend;
    backend.hide_current_image = []() {
        #if HAS_DISPLAY
        // Called from AsyncTCP task and sometimes from the main loop.
        // Always defer actual display/LVGL operations to the main loop.
        pending_image_hide_request = true;
        #endif
    };
    
    backend.start_strip_session = [](int width, int height, unsigned long timeout_ms, unsigned long start_time) -> bool {
        #if HAS_DISPLAY
        (void)start_time;
        DirectImageScreen* screen = display_manager_get_direct_image_screen();
        if (!screen) {
            Logger.logMessage("ImageAPI", "ERROR: No direct image screen");
            return false;
        }
        
        // Now called from main loop with proper task context
        // Show the DirectImageScreen first
        display_manager_show_direct_image();

        // Screen-affecting action counts as explicit activity and should wake.
        screen_saver_manager_notify_activity(true);
        
        // Configure timeout and start session
        screen->set_timeout(timeout_ms);
        screen->begin_strip_session(width, height);
        return true;
        #else
        return false;
        #endif
    };
    
    backend.decode_strip = [](const uint8_t* jpeg_data, size_t jpeg_size, uint8_t strip_index, bool output_bgr565) -> bool {
        #if HAS_DISPLAY
        DirectImageScreen* screen = display_manager_get_direct_image_screen();
        if (!screen) {
            Logger.logMessage("ImageAPI", "ERROR: No direct image screen");
            return false;
        }
        
        // Now called from main loop - safe to decode
        return screen->decode_strip(jpeg_data, jpeg_size, strip_index, output_bgr565);
        #else
        return false;
        #endif
    };
    
    // Setup configuration
    ImageApiConfig image_cfg;

    // Use the display driver's coordinate space (fast path for direct image writes).
    // This intentionally avoids LVGL calls and any DISPLAY_ROTATION heuristics.
    image_cfg.lcd_width = DISPLAY_WIDTH;
    image_cfg.lcd_height = DISPLAY_HEIGHT;

    #if HAS_DISPLAY
        if (displayManager && displayManager->getDriver()) {
            image_cfg.lcd_width = displayManager->getDriver()->width();
            image_cfg.lcd_height = displayManager->getDriver()->height();
        }
    #endif
    
    image_cfg.max_image_size_bytes = IMAGE_API_MAX_SIZE_BYTES;
    image_cfg.decode_headroom_bytes = IMAGE_API_DECODE_HEADROOM_BYTES;
    image_cfg.default_timeout_ms = IMAGE_API_DEFAULT_TIMEOUT_MS;
    image_cfg.max_timeout_ms = IMAGE_API_MAX_TIMEOUT_MS;
    
    // Initialize and register routes
    image_api_init(image_cfg, backend);
    image_api_register_routes(server, portal_auth_gate);
    Logger.logMessage("Portal", "Image API initialized");
    #endif // HAS_IMAGE_API && HAS_DISPLAY
    
    // 404 handler
    server->onNotFound([](AsyncWebServerRequest *request) {
        // In AP mode, redirect to root for captive portal
        if (ap_mode_active) {
            request->redirect("/");
        } else {
            request->send(404, "text/plain", "Not found");
        }
    });
    
    // Start server
    yield();
    delay(100);
    server->begin();
    Logger.logEnd();
}

// Start AP mode with captive portal
void web_portal_start_ap() {
    Logger.logBegin("AP Mode");
    
    // Generate AP name with chip ID
    uint32_t chipId = 0;
    for(int i=0; i<17; i=i+8) {
        chipId |= ((ESP.getEfuseMac() >> (40 - i)) & 0xff) << i;
    }
    
    // Convert PROJECT_NAME to uppercase for AP SSID
    String apPrefix = String(PROJECT_NAME);
    apPrefix.toUpperCase();
    String apName = apPrefix + "-" + String(chipId, HEX);
    
    Logger.logLinef("SSID: %s", apName.c_str());
    
    // Configure AP
    WiFi.mode(WIFI_AP);
    WiFi.softAPConfig(CAPTIVE_PORTAL_IP, CAPTIVE_PORTAL_IP, IPAddress(255, 255, 255, 0));
    WiFi.softAP(apName.c_str());
    
    // Start DNS server for captive portal (redirect all DNS queries to our IP)
    dnsServer.start(DNS_PORT, "*", CAPTIVE_PORTAL_IP);
    
    WiFi.softAPsetHostname(apName.c_str());

    // Mark AP mode active so watchdog/DNS handling knows we're in captive portal
    ap_mode_active = true;

    Logger.logLinef("IP: %s", WiFi.softAPIP().toString().c_str());
    Logger.logEnd("Captive portal active");
}

// Stop AP mode
void web_portal_stop_ap() {
    if (ap_mode_active) {
        Logger.logMessage("Portal", "Stopping AP mode");
        dnsServer.stop();
        WiFi.softAPdisconnect(true);
        ap_mode_active = false;
    }
}

// Handle web server (call in loop)
void web_portal_handle() {
    if (ap_mode_active) {
        dnsServer.processNextRequest();
    }

#if MEMORY_SNAPSHOT_ON_HTTP_ENABLED
    // Emit deferred per-page memory snapshots from the main loop.
    if (s_pending_http_root && !s_logged_http_root) {
        s_pending_http_root = false;
        s_logged_http_root = true;
        device_telemetry_log_memory_snapshot("http_root");
    }
    if (s_pending_http_network && !s_logged_http_network) {
        s_pending_http_network = false;
        s_logged_http_network = true;
        device_telemetry_log_memory_snapshot("http_network");
    }
    if (s_pending_http_firmware && !s_logged_http_firmware) {
        s_pending_http_firmware = false;
        s_logged_http_firmware = true;
        device_telemetry_log_memory_snapshot("http_firmware");
    }
#endif
}

// Check if in AP mode
bool web_portal_is_ap_mode() {
    return ap_mode_active;
}

// Check if OTA update is in progress
bool web_portal_ota_in_progress() {
    return ota_in_progress;
}

#if HAS_IMAGE_API
// Process pending image uploads (call from main loop)
void web_portal_process_pending_images() {
    // If the image API asked us to hide/dismiss the current image (or recover
    // from a failure), do it from the main loop so DisplayManager can safely
    // clear direct-image mode.
    #if HAS_DISPLAY
    if (pending_image_hide_request) {
        pending_image_hide_request = false;
        display_manager_return_to_previous_screen();
    }
    #endif

    image_api_process_pending(ota_in_progress);
}
#endif
