#include "web_portal_routes.h"

#include <ESPAsyncWebServer.h>
#include <ArduinoJson.h>

#include "log_manager.h"
#include "web_portal_auth.h"
#include "spotify_manager.h"

static void handleSpotifyAuthStart(AsyncWebServerRequest* request) {
    if (!portal_auth_gate(request)) return;

    char url[768];
    char state[64];
    if (!spotify_manager::begin_auth(url, sizeof(url), state, sizeof(state))) {
        request->send(500, "application/json", "{\"success\":false,\"message\":\"Failed to start auth\"}");
        return;
    }

    StaticJsonDocument<896> doc;
    doc["success"] = true;
    doc["authorize_url"] = url;
    doc["state"] = state;

    AsyncResponseStream* response = request->beginResponseStream("application/json");
    serializeJson(doc, *response);
    request->send(response);
}

static void handleSpotifyAuthComplete(AsyncWebServerRequest* request, uint8_t* data, size_t len, size_t index, size_t total) {
    if (!portal_auth_gate(request)) return;
    if (index != 0 || index + len != total) return;

    StaticJsonDocument<2048> doc;
    DeserializationError err = deserializeJson(doc, data, len);
    if (err) {
        request->send(400, "application/json", "{\"success\":false,\"message\":\"Invalid JSON\"}");
        return;
    }

    const char* code = doc["code"] | "";
    const char* state = doc["state"] | "";

    if (!code[0] || !state[0]) {
        request->send(400, "application/json", "{\"success\":false,\"message\":\"Missing code/state\"}");
        return;
    }

    if (!spotify_manager::queue_complete_auth(code, state)) {
        request->send(500, "application/json", "{\"success\":false,\"message\":\"Failed to queue auth\"}");
        return;
    }

    // Token exchange happens asynchronously in the main loop.
    request->send(202, "application/json", "{\"success\":true,\"message\":\"Auth queued\"}");
}

static void handleSpotifyStatus(AsyncWebServerRequest* request) {
    if (!portal_auth_gate(request)) return;

    StaticJsonDocument<256> doc;
    doc["connected"] = spotify_manager::is_connected();

    AsyncResponseStream* response = request->beginResponseStream("application/json");
    serializeJson(doc, *response);
    request->send(response);
}

static void handleSpotifyDisconnect(AsyncWebServerRequest* request) {
    if (!portal_auth_gate(request)) return;
    Logger.logMessage("API", "POST /api/spotify/disconnect");
    spotify_manager::disconnect();
    request->send(200, "application/json", "{\"success\":true}");
}

void web_portal_register_api_spotify_routes(AsyncWebServer& server) {
    server.on("/api/spotify/auth/start", HTTP_POST, handleSpotifyAuthStart);

    server.on(
        "/api/spotify/auth/complete",
        HTTP_POST,
        [](AsyncWebServerRequest* request) {
            if (!portal_auth_gate(request)) return;
        },
        NULL,
        handleSpotifyAuthComplete
    );

    server.on("/api/spotify/status", HTTP_GET, handleSpotifyStatus);
    server.on("/api/spotify/disconnect", HTTP_POST, handleSpotifyDisconnect);
}
