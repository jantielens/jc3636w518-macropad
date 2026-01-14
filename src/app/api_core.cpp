#include "web_portal_routes.h"

#include <ESPAsyncWebServer.h>
#include <ArduinoJson.h>
#include <WiFi.h>

#include "board_config.h"
#include "device_telemetry.h"
#include "github_release_config.h"
#include "log_manager.h"
#include "project_branding.h"
#include "web_portal_auth.h"
#include "web_portal_http.h"
#include "web_portal_json_alloc.h"
#include "web_portal_state.h"
#include "../version.h"

#if HAS_DISPLAY
#include "display_manager.h"
#include "screen_saver_manager.h"
#endif

static void handleGetMode(AsyncWebServerRequest* request) {
    if (!portal_auth_gate(request)) return;

    AsyncResponseStream* response = request->beginResponseStream("application/json");
    response->print("{\"mode\":\"");
    response->print(web_portal_state().ap_mode_active ? "core" : "full");
    response->print("\",\"ap_active\":");
    response->print(web_portal_state().ap_mode_active ? "true" : "false");
    response->print("}");
    request->send(response);
}

static void handleGetVersion(AsyncWebServerRequest* request);
static void handleGetHealth(AsyncWebServerRequest* request);

static void handleReboot(AsyncWebServerRequest* request) {
    if (!portal_auth_gate(request)) return;
    Logger.logMessage("API", "POST /api/reboot");

    request->send(200, "application/json", "{\"success\":true,\"message\":\"Rebooting device...\"}");

    // Schedule reboot after response is sent
    delay(100);
    Logger.logMessage("Portal", "Rebooting");
    ESP.restart();
}

void web_portal_register_api_core_routes(AsyncWebServer& server) {
    server.on("/api/mode", HTTP_GET, handleGetMode);
    server.on("/api/info", HTTP_GET, handleGetVersion);
    server.on("/api/health", HTTP_GET, handleGetHealth);
    server.on("/api/reboot", HTTP_POST, handleReboot);
}

static void handleGetVersion(AsyncWebServerRequest* request) {
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

    // Web portal health widget configuration (client-side only).
    doc["health_poll_interval_ms"] = HEALTH_POLL_INTERVAL_MS;
    doc["health_history_seconds"] = HEALTH_HISTORY_SECONDS;

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

static void handleGetHealth(AsyncWebServerRequest* request) {
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
