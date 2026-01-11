#include "web_portal_routes.h"

#include <ESPAsyncWebServer.h>
#include <Update.h>

#include "device_telemetry.h"
#include "log_manager.h"
#include "web_portal_auth.h"
#include "web_portal_state.h"

static void handleOTAUpload(AsyncWebServerRequest* request, String filename, size_t index, uint8_t* data, size_t len, bool final) {
    if (!portal_auth_gate(request)) return;

    // First chunk - initialize OTA
    if (index == 0) {
        Logger.logBegin("OTA Update");
        Logger.logLinef("File: %s", filename.c_str());
        Logger.logLinef("Size: %d bytes", request->contentLength());

        web_portal_state().ota_in_progress = true;
        web_portal_state().ota_progress = 0;
        web_portal_state().ota_total = request->contentLength();

        // Check if filename ends with .bin
        if (!filename.endsWith(".bin")) {
            Logger.logEnd("Not a .bin file");
            request->send(400, "application/json", "{\"success\":false,\"message\":\"Only .bin files are supported\"}");
            web_portal_state().ota_in_progress = false;
            return;
        }

        // Get OTA partition size
        size_t updateSize = (web_portal_state().ota_total > 0) ? web_portal_state().ota_total : UPDATE_SIZE_UNKNOWN;
        size_t freeSpace = device_telemetry_free_sketch_space();

        Logger.logLinef("Free space: %d bytes", freeSpace);

        // Validate size before starting
        if (web_portal_state().ota_total > 0 && web_portal_state().ota_total > freeSpace) {
            Logger.logEnd("Firmware too large");
            request->send(400, "application/json", "{\"success\":false,\"message\":\"Firmware too large\"}");
            web_portal_state().ota_in_progress = false;
            return;
        }

        // Begin OTA update
        if (!Update.begin(updateSize, U_FLASH)) {
            Logger.logEnd("Begin failed");
            Update.printError(Serial);
            request->send(500, "application/json", "{\"success\":false,\"message\":\"OTA begin failed\"}");
            web_portal_state().ota_in_progress = false;
            return;
        }
    }

    // Write chunk to flash
    if (len) {
        if (Update.write(data, len) != len) {
            Logger.logEnd("Write failed");
            Update.printError(Serial);
            request->send(500, "application/json", "{\"success\":false,\"message\":\"Write failed\"}");
            web_portal_state().ota_in_progress = false;
            return;
        }

        web_portal_state().ota_progress += len;

        // Print progress every 10%
        static uint8_t last_percent = 0;
        uint8_t percent = (web_portal_state().ota_total > 0) ? (uint8_t)((web_portal_state().ota_progress * 100) / web_portal_state().ota_total) : 0;
        if (percent >= last_percent + 10) {
            Logger.logLinef("Progress: %d%%", percent);
            last_percent = percent;
        }
    }

    // Final chunk - complete OTA
    if (final) {
        if (Update.end(true)) {
            Logger.logLinef("Written: %d bytes", web_portal_state().ota_progress);
            Logger.logEnd("Success - rebooting");

            request->send(200, "application/json", "{\"success\":true,\"message\":\"Update successful! Rebooting...\"}");

            delay(500);
            ESP.restart();
        } else {
            Logger.logEnd("Update failed");
            Update.printError(Serial);
            request->send(500, "application/json", "{\"success\":false,\"message\":\"Update failed\"}");
        }

        web_portal_state().ota_in_progress = false;
    }
}

void web_portal_register_api_ota_routes(AsyncWebServer& server) {
    server.on(
        "/api/update",
        HTTP_POST,
        [](AsyncWebServerRequest* request) {
            if (!portal_auth_gate(request)) return;
        },
        handleOTAUpload
    );
}
