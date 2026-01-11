#include "web_portal_routes.h"

#include <ESPAsyncWebServer.h>
#include <ArduinoJson.h>

#include "board_config.h"
#include "config_manager.h"
#include "log_manager.h"
#include "web_portal_auth.h"
#include "web_portal_json_alloc.h"
#include "web_portal_state.h"

#if HAS_DISPLAY
#include "display_manager.h"
#include "screen_saver_manager.h"
#endif

static void handleGetConfig(AsyncWebServerRequest* request) {
    if (!portal_auth_gate(request)) return;

    DeviceConfig* current_config = web_portal_state().config;
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

    AsyncResponseStream* response = request->beginResponseStream("application/json");
    serializeJson(doc, *response);
    request->send(response);
}

static void handlePostConfig(AsyncWebServerRequest* request, uint8_t* data, size_t len, size_t index, size_t total) {
    if (!portal_auth_gate(request)) return;

    (void)index;
    (void)total;

    DeviceConfig* current_config = web_portal_state().config;
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
    if (web_portal_state().ap_mode_active && (doc.containsKey("basic_auth_enabled") || doc.containsKey("basic_auth_username") || doc.containsKey("basic_auth_password"))) {
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

static void handleDeleteConfig(AsyncWebServerRequest* request) {
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

void web_portal_register_api_config_routes(AsyncWebServer& server) {
    server.on("/api/config", HTTP_GET, handleGetConfig);

    server.on(
        "/api/config",
        HTTP_POST,
        [](AsyncWebServerRequest* request) {
            if (!portal_auth_gate(request)) return;
        },
        NULL,
        handlePostConfig
    );

    server.on("/api/config", HTTP_DELETE, handleDeleteConfig);
}
