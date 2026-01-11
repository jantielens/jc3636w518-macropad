#include "web_portal_http.h"

AsyncWebServerResponse* begin_gzipped_asset_response(
    AsyncWebServerRequest* request,
    const char* content_type,
    const uint8_t* content_gz,
    size_t content_gz_len,
    const char* cache_control
) {
    // Prefer the PROGMEM-aware response helper to avoid accidental heap copies.
    // All generated assets live in flash as `const uint8_t[] PROGMEM`.
    AsyncWebServerResponse* response = request->beginResponse_P(
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
