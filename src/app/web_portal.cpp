/*
 * Web Configuration Portal Implementation
 * 
 * Async web server with captive portal support.
 * Serves static files and provides REST API for configuration.
 */

// Increase AsyncTCP task stack size to prevent overflow
// Default is 8192, increase to 16384 for web assets
#define CONFIG_ASYNC_TCP_STACK_SIZE 16384

#include "web_portal.h"
#include "web_assets.h"
#include "config_manager.h"
#include "log_manager.h"
#include "board_config.h"
#include "device_telemetry.h"
#include "github_release_config.h"
#include "../version.h"

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

// ===== Basic Auth (optional; STA/full mode only) =====
static bool portal_auth_required() {
    if (ap_mode_active) return false;
    if (!current_config) return false;
    return current_config->basic_auth_enabled;
}

static bool portal_auth_gate(AsyncWebServerRequest *request) {
    if (!portal_auth_required()) return true;

    const char *user = current_config->basic_auth_username;
    const char *pass = current_config->basic_auth_password;

    if (request->authenticate(user, pass)) {
        return true;
    }

    request->requestAuthentication(PROJECT_DISPLAY_NAME);
    return false;
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

    DynamicJsonDocument doc(8192);
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
    uint8_t buf[2048];

    while (http.connected() && (http_len > 0 || http_len == -1)) {
        const size_t available = stream->available();
        if (!available) {
            delay(1);
            continue;
        }

        const size_t to_read = (available > sizeof(buf)) ? sizeof(buf) : available;
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
    StaticJsonDocument<2304> doc;
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
    StaticJsonDocument<2304> doc;
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
    AsyncResponseStream *response = request->beginResponseStream("application/json");
    response->print("{\"version\":\"");
    response->print(FIRMWARE_VERSION);
    response->print("\",\"build_date\":\"");
    response->print(BUILD_DATE);
    response->print("\",\"build_time\":\"");
    response->print(BUILD_TIME);
    response->print("\",\"chip_model\":\"");
    response->print(ESP.getChipModel());
    response->print("\",\"chip_revision\":");
    response->print(ESP.getChipRevision());
    response->print(",\"chip_cores\":");
    response->print(ESP.getChipCores());
    response->print(",\"cpu_freq\":");
    response->print(ESP.getCpuFreqMHz());
    response->print(",\"flash_chip_size\":");
    response->print(ESP.getFlashChipSize());
    response->print(",\"psram_size\":");
    response->print(ESP.getPsramSize());
    response->print(",\"free_heap\":");
    response->print(ESP.getFreeHeap());
    response->print(",\"sketch_size\":");
    response->print(device_telemetry_sketch_size());
    response->print(",\"free_sketch_space\":");
    response->print(device_telemetry_free_sketch_space());
    response->print(",\"mac_address\":\"");
    response->print(WiFi.macAddress());
    response->print("\",\"wifi_hostname\":\"");
    response->print(WiFi.getHostname());
    response->print("\",\"mdns_name\":\"");
    response->print(WiFi.getHostname());
    response->print(".local\",\"hostname\":\"");
    response->print(WiFi.getHostname());
    response->print("\",\"project_name\":\"");
    response->print(PROJECT_NAME);
    response->print("\",\"project_display_name\":\"");
    response->print(PROJECT_DISPLAY_NAME);

    // Build metadata for GitHub-based updates
    response->print("\",\"board_name\":\"");
    #ifdef BUILD_BOARD_NAME
    response->print(BUILD_BOARD_NAME);
    #else
    response->print("unknown");
    #endif
    response->print("\",\"github_updates_enabled\":");
    response->print(GITHUB_UPDATES_ENABLED ? "true" : "false");
    #if GITHUB_UPDATES_ENABLED
    response->print(",\"github_owner\":\"");
    response->print(GITHUB_OWNER);
    response->print("\",\"github_repo\":\"");
    response->print(GITHUB_REPO);
    response->print("\"");
    #endif
    response->print(",\"has_mqtt\":");
    response->print(HAS_MQTT ? "true" : "false");
    response->print(",\"has_backlight\":");
    response->print(HAS_BACKLIGHT ? "true" : "false");
    
    #if HAS_DISPLAY
    // Display screen information
    response->print(",\"has_display\":true");

    // Display resolution (driver coordinate space for direct writes / image upload)
    int display_coord_width = DISPLAY_WIDTH;
    int display_coord_height = DISPLAY_HEIGHT;
    if (displayManager && displayManager->getDriver()) {
        display_coord_width = displayManager->getDriver()->width();
        display_coord_height = displayManager->getDriver()->height();
    }
    response->print(",\"display_coord_width\":");
    response->print(display_coord_width);
    response->print(",\"display_coord_height\":");
    response->print(display_coord_height);
    
    // Get available screens
    size_t screen_count = 0;
    const ScreenInfo* screens = display_manager_get_available_screens(&screen_count);
    
    response->print(",\"available_screens\":[");
    for (size_t i = 0; i < screen_count; i++) {
        if (i > 0) response->print(",");
        response->print("{\"id\":\"");
        response->print(screens[i].id);
        response->print("\",\"name\":\"");
        response->print(screens[i].display_name);
        response->print("\"}");
    }
    response->print("]");
    
    // Get current screen
    const char* current_screen = display_manager_get_current_screen_id();
    response->print(",\"current_screen\":");
    if (current_screen) {
        response->print("\"");
        response->print(current_screen);
        response->print("\"");
    } else {
        response->print("null");
    }
    #else
    response->print(",\"has_display\":false");
    #endif
    
    response->print("}");
    request->send(response);
}

// GET /api/health - Get device health statistics
void handleGetHealth(AsyncWebServerRequest *request) {
    if (!portal_auth_gate(request)) return;
    StaticJsonDocument<1024> doc;

    device_telemetry_fill_api(doc);

    if (doc.overflowed()) {
        Logger.logMessage("Portal", "ERROR: /api/health JSON overflow (StaticJsonDocument too small)");
        request->send(500, "application/json", "{\"success\":false,\"message\":\"Response too large\"}");
        return;
    }

    AsyncResponseStream *response = request->beginResponseStream("application/json");
    serializeJson(doc, *response);
    request->send(response);
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
    Logger.logLinef("Portal config pointer: %p, backlight_brightness: %d", 
                    current_config, current_config->backlight_brightness);
    
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
    Logger.logMessage("Portal", "Calling image_api_init...");
    image_api_init(image_cfg, backend);
    Logger.logMessage("Portal", "Calling image_api_register_routes...");
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
