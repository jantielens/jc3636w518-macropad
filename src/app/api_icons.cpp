#include "web_portal_routes.h"

#include <ESPAsyncWebServer.h>

#include "macros_config.h"
#include "web_portal_auth.h"
#include "web_portal_http.h"

#if HAS_DISPLAY && HAS_ICONS
#include "icon_registry.h"
#include "icon_store.h"
#include <FFat.h>
#endif

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

// The runtime macro screen UI reads from this instance (defined in app.ino).
extern MacroConfig macro_config;

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

// GET /api/icons
// Returns the compiled icon IDs so the portal can offer an autocomplete list.
static void handleGetIcons(AsyncWebServerRequest* request) {
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
static void handleGetInstalledIcons(AsyncWebServerRequest* request) {
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
static void handlePostIconInstall(AsyncWebServerRequest* request, uint8_t* data, size_t len, size_t index, size_t total) {
    if (!portal_auth_gate(request)) return;

#if !(HAS_DISPLAY && HAS_ICONS)
    (void)data;
    (void)len;
    (void)index;
    (void)total;
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
static void handlePostIconGC(AsyncWebServerRequest* request) {
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

void web_portal_register_api_icons_routes(AsyncWebServer& server) {
    // NOTE: register more specific routes first; some AsyncWebServer URI matchers behave like prefix matches.
    server.on("/api/icons/installed", HTTP_GET, handleGetInstalledIcons);
    server.on("/api/icons/gc", HTTP_POST, handlePostIconGC);
    server.on(
        "/api/icons/install",
        HTTP_POST,
        [](AsyncWebServerRequest* request) {
            if (!portal_auth_gate(request)) return;
        },
        NULL,
        handlePostIconInstall
    );
    server.on("/api/icons", HTTP_GET, handleGetIcons);
}
