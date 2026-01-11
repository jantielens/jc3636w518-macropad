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

#include "board_config.h"
#include "device_telemetry.h"
#include "log_manager.h"
#include "project_branding.h"

#include "web_portal_auth.h"
#include "web_portal_routes.h"
#include "web_portal_state.h"

#if HAS_DISPLAY
#include "display_manager.h"
#include "screen_saver_manager.h"
#endif

#if HAS_IMAGE_API
#include "image_api.h"
#endif

#include <DNSServer.h>
#include <ESPAsyncWebServer.h>
#include <WiFi.h>

// Web server on port 80 (pointer to avoid constructor issues)
static AsyncWebServer* server = nullptr;

// DNS server for captive portal (port 53)
static DNSServer dnsServer;

// AP configuration
#define DNS_PORT 53
#define CAPTIVE_PORTAL_IP IPAddress(192, 168, 4, 1)

#if HAS_IMAGE_API && HAS_DISPLAY
// AsyncWebServer callbacks run on the AsyncTCP task; never touch LVGL/display from there.
// Use this flag to defer "hide current image / return" operations to the main loop.
static volatile bool pending_image_hide_request = false;
#endif

#if MEMORY_SNAPSHOT_ON_HTTP_ENABLED
// AsyncWebServer handlers run on the AsyncTCP task; log memory snapshots from
// the main loop (web_portal_handle) to avoid extra allocator pressure in the
// networking task.
volatile bool g_portal_pending_http_root = false;
volatile bool g_portal_pending_http_network = false;
volatile bool g_portal_pending_http_firmware = false;

static bool s_logged_http_root = false;
static bool s_logged_http_network = false;
static bool s_logged_http_firmware = false;
#endif

void web_portal_init(DeviceConfig* config) {
    Logger.logBegin("Portal Init");

    web_portal_state().config = config;

    // Load macros config once at portal init so GET /api/macros is cheap.
    web_portal_macros_preload();

    // Create web server instance (avoid global constructor issues)
    if (server == nullptr) {
        yield();
        delay(100);
        server = new AsyncWebServer(80);
        yield();
        delay(100);
    }

    // Routes
    web_portal_register_page_routes(*server);
    web_portal_register_asset_routes(*server);

    web_portal_register_api_core_routes(*server);
    web_portal_register_api_config_routes(*server);
    web_portal_register_api_icons_routes(*server);
    web_portal_register_api_macros_routes(*server);
    web_portal_register_api_firmware_routes(*server);
    web_portal_register_api_display_routes(*server);
    web_portal_register_api_ota_routes(*server);

#if HAS_IMAGE_API && HAS_DISPLAY
    Logger.logMessage("Portal", "Initializing image API");

    // Setup backend adapter
    ImageApiBackend backend;
    backend.hide_current_image = []() {
        // Called from AsyncTCP task and sometimes from the main loop.
        // Always defer actual display/LVGL operations to the main loop.
        pending_image_hide_request = true;
    };

    backend.start_strip_session = [](int width, int height, unsigned long timeout_ms, unsigned long start_time) -> bool {
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
    };

    backend.decode_strip = [](const uint8_t* jpeg_data, size_t jpeg_size, uint8_t strip_index, bool output_bgr565) -> bool {
        DirectImageScreen* screen = display_manager_get_direct_image_screen();
        if (!screen) {
            Logger.logMessage("ImageAPI", "ERROR: No direct image screen");
            return false;
        }

        // Now called from main loop - safe to decode
        return screen->decode_strip(jpeg_data, jpeg_size, strip_index, output_bgr565);
    };

    // Setup configuration
    ImageApiConfig image_cfg;

    // Use the display driver's coordinate space (fast path for direct image writes).
    // This intentionally avoids LVGL calls and any DISPLAY_ROTATION heuristics.
    image_cfg.lcd_width = DISPLAY_WIDTH;
    image_cfg.lcd_height = DISPLAY_HEIGHT;

    if (displayManager && displayManager->getDriver()) {
        image_cfg.lcd_width = displayManager->getDriver()->width();
        image_cfg.lcd_height = displayManager->getDriver()->height();
    }

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
    server->onNotFound([](AsyncWebServerRequest* request) {
        // In AP mode, redirect to root for captive portal
        if (web_portal_state().ap_mode_active) {
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

void web_portal_start_ap() {
    Logger.logBegin("AP Mode");

    // Generate AP name with chip ID
    uint32_t chipId = 0;
    for (int i = 0; i < 17; i = i + 8) {
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
    web_portal_state().ap_mode_active = true;

    Logger.logLinef("IP: %s", WiFi.softAPIP().toString().c_str());
    Logger.logEnd("Captive portal active");
}

void web_portal_stop_ap() {
    if (web_portal_state().ap_mode_active) {
        Logger.logMessage("Portal", "Stopping AP mode");
        dnsServer.stop();
        WiFi.softAPdisconnect(true);
        web_portal_state().ap_mode_active = false;
    }
}

void web_portal_handle() {
    if (web_portal_state().ap_mode_active) {
        dnsServer.processNextRequest();
    }

#if MEMORY_SNAPSHOT_ON_HTTP_ENABLED
    // Emit deferred per-page memory snapshots from the main loop.
    if (g_portal_pending_http_root && !s_logged_http_root) {
        g_portal_pending_http_root = false;
        s_logged_http_root = true;
        device_telemetry_log_memory_snapshot("http_root");
    }
    if (g_portal_pending_http_network && !s_logged_http_network) {
        g_portal_pending_http_network = false;
        s_logged_http_network = true;
        device_telemetry_log_memory_snapshot("http_network");
    }
    if (g_portal_pending_http_firmware && !s_logged_http_firmware) {
        g_portal_pending_http_firmware = false;
        s_logged_http_firmware = true;
        device_telemetry_log_memory_snapshot("http_firmware");
    }
#endif
}

bool web_portal_is_ap_mode() {
    return web_portal_state().ap_mode_active;
}

bool web_portal_ota_in_progress() {
    return web_portal_state().ota_in_progress;
}

#if HAS_IMAGE_API
void web_portal_process_pending_images() {
#if HAS_DISPLAY
    // If the image API asked us to hide/dismiss the current image (or recover
    // from a failure), do it from the main loop so DisplayManager can safely
    // clear direct-image mode.
    if (pending_image_hide_request) {
        pending_image_hide_request = false;
        display_manager_return_to_previous_screen();
    }
#endif

    image_api_process_pending(web_portal_state().ota_in_progress);
}
#endif
