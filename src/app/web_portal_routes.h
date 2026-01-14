#pragma once

class AsyncWebServer;

void web_portal_register_page_routes(AsyncWebServer& server);
void web_portal_register_asset_routes(AsyncWebServer& server);

void web_portal_register_api_core_routes(AsyncWebServer& server);
void web_portal_register_api_config_routes(AsyncWebServer& server);
void web_portal_register_api_macros_routes(AsyncWebServer& server);
void web_portal_register_api_icons_routes(AsyncWebServer& server);
void web_portal_register_api_firmware_routes(AsyncWebServer& server);
void web_portal_register_api_display_routes(AsyncWebServer& server);
void web_portal_register_api_ota_routes(AsyncWebServer& server);
void web_portal_register_api_spotify_routes(AsyncWebServer& server);

void web_portal_macros_preload();
