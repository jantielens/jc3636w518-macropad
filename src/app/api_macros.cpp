#include "web_portal_routes.h"

#include <ArduinoJson.h>
#include <ESPAsyncWebServer.h>

#include "device_telemetry.h"
#include "log_manager.h"
#include "macros_config.h"
#include "macro_templates.h"
#include "web_portal_auth.h"
#include "web_portal_http.h"
#include "web_portal_json_alloc.h"

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <esp_heap_caps.h>
#include <soc/soc_caps.h>

// The runtime macro screen UI reads from this instance (defined in app.ino).
extern MacroConfig macro_config;

// ===== Macros Config (screens × buttons) =====
static bool g_macros_loaded = false;

static uint8_t* g_macros_body = nullptr;
static size_t g_macros_body_total = 0;
static bool g_macros_body_in_progress = false;

// MVP: macros editor posts the full screens × buttons config. Keep the parsing document bounded.
// If MACROS_* dimensions or max string sizes are increased, adjust this accordingly.
static constexpr size_t kMacrosJsonDocCapacity = 65536;

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

void web_portal_macros_preload() {
    macros_cache_load_if_needed();
}

// GET /api/macros
static void handleGetMacros(AsyncWebServerRequest* request) {
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

// POST /api/macros
// Accepts a single JSON payload containing all screens × buttons.
static void handlePostMacros(AsyncWebServerRequest* request, uint8_t* data, size_t len, size_t index, size_t total) {
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

void web_portal_register_api_macros_routes(AsyncWebServer& server) {
    server.on("/api/macros", HTTP_GET, handleGetMacros);
    server.on(
        "/api/macros",
        HTTP_POST,
        [](AsyncWebServerRequest* request) {
            if (!portal_auth_gate(request)) return;
        },
        NULL,
        handlePostMacros
    );
}
