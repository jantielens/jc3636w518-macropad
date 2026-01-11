#include "web_portal_routes.h"

#include <ESPAsyncWebServer.h>

#include "log_manager.h"
#include "web_assets.h"
#include "web_portal_auth.h"
#include "web_portal_http.h"
#include "web_portal_state.h"

#if MEMORY_SNAPSHOT_ON_HTTP_ENABLED
extern volatile bool g_portal_pending_http_root;
extern volatile bool g_portal_pending_http_network;
extern volatile bool g_portal_pending_http_firmware;
extern bool g_portal_logged_http_root;
extern bool g_portal_logged_http_network;
extern bool g_portal_logged_http_firmware;
#endif

static void handleRoot(AsyncWebServerRequest* request) {
    if (!portal_auth_gate(request)) return;
    if (web_portal_state().ap_mode_active) {
        request->redirect("/network.html");
    } else {
        AsyncWebServerResponse* response = begin_gzipped_asset_response(
            request,
            "text/html",
            home_html_gz,
            home_html_gz_len,
            "no-store"
        );
        request->send(response);

#if MEMORY_SNAPSHOT_ON_HTTP_ENABLED
        if (!g_portal_logged_http_root) {
            g_portal_pending_http_root = true;
        }
#endif
    }
}

static void handleHome(AsyncWebServerRequest* request) {
    if (!portal_auth_gate(request)) return;
    if (web_portal_state().ap_mode_active) {
        request->redirect("/network.html");
        return;
    }
    AsyncWebServerResponse* response = begin_gzipped_asset_response(
        request,
        "text/html",
        home_html_gz,
        home_html_gz_len,
        "no-store"
    );
    request->send(response);
}

static void handleNetwork(AsyncWebServerRequest* request) {
    if (!portal_auth_gate(request)) return;
    AsyncWebServerResponse* response = begin_gzipped_asset_response(
        request,
        "text/html",
        network_html_gz,
        network_html_gz_len,
        "no-store"
    );
    request->send(response);

#if MEMORY_SNAPSHOT_ON_HTTP_ENABLED
    if (!g_portal_logged_http_network) {
        g_portal_pending_http_network = true;
    }
#endif
}

static void handleFirmware(AsyncWebServerRequest* request) {
    if (!portal_auth_gate(request)) return;
    if (web_portal_state().ap_mode_active) {
        request->redirect("/network.html");
        return;
    }
    AsyncWebServerResponse* response = begin_gzipped_asset_response(
        request,
        "text/html",
        firmware_html_gz,
        firmware_html_gz_len,
        "no-store"
    );
    request->send(response);

#if MEMORY_SNAPSHOT_ON_HTTP_ENABLED
    if (!g_portal_logged_http_firmware) {
        g_portal_pending_http_firmware = true;
    }
#endif
}

void web_portal_register_page_routes(AsyncWebServer& server) {
    server.on("/", HTTP_GET, handleRoot);
    server.on("/home.html", HTTP_GET, handleHome);
    server.on("/network.html", HTTP_GET, handleNetwork);
    server.on("/firmware.html", HTTP_GET, handleFirmware);
}

static void handleCSS(AsyncWebServerRequest* request) {
    AsyncWebServerResponse* response = begin_gzipped_asset_response(
        request,
        "text/css",
        portal_css_gz,
        portal_css_gz_len,
        "public, max-age=600"
    );
    request->send(response);
}

static void handleJS(AsyncWebServerRequest* request) {
    AsyncWebServerResponse* response = begin_gzipped_asset_response(
        request,
        "application/javascript",
        portal_js_gz,
        portal_js_gz_len,
        "public, max-age=600"
    );
    request->send(response);
}

void web_portal_register_asset_routes(AsyncWebServer& server) {
    server.on("/portal.css", HTTP_GET, handleCSS);
    server.on("/portal.js", HTTP_GET, handleJS);
}
