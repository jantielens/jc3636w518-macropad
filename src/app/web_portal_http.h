#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <ESPAsyncWebServer.h>

static inline size_t chunk_copy_out(uint8_t* dst, size_t maxLen, const char* src, size_t srcLen, size_t& srcOff) {
    if (!src || srcOff >= srcLen || maxLen == 0) return 0;
    const size_t n = (srcLen - srcOff) < maxLen ? (srcLen - srcOff) : maxLen;
    memcpy(dst, src + srcOff, n);
    srcOff += n;
    return n;
}

template <typename State>
static inline void send_chunked_state(AsyncWebServerRequest* request, const char* contentType, State* st) {
    AsyncWebServerResponse* response = request->beginChunkedResponse(
        contentType,
        [st](uint8_t* buffer, size_t maxLen, size_t /*index*/) mutable -> size_t {
            if (!st) return 0;
            const size_t n = st->fill(buffer, maxLen);
            if (n == 0) {
                delete st;
                st = nullptr;
            }
            return n;
        }
    );
    request->send(response);
}

struct JsonStringChunker {
    String payload;
    size_t off = 0;

    size_t fill(uint8_t* buffer, size_t maxLen) {
        if (maxLen == 0) return 0;
        const size_t len = payload.length();
        if (off >= len) return 0;
        const size_t n = (len - off) < maxLen ? (len - off) : maxLen;
        memcpy(buffer, payload.c_str() + off, n);
        off += n;
        return n;
    }
};

template <typename JsonDoc>
static inline bool send_json_doc_chunked(AsyncWebServerRequest* request, JsonDoc& doc, int oom_http_status) {
    JsonStringChunker* st = new JsonStringChunker();
    if (!st) {
        request->send(oom_http_status, "application/json", "{\"success\":false,\"message\":\"Out of memory\"}");
        return false;
    }

    st->payload.reserve(measureJson(doc) + 1);
    serializeJson(doc, st->payload);
    send_chunked_state(request, "application/json", st);
    return true;
}

AsyncWebServerResponse* begin_gzipped_asset_response(
    AsyncWebServerRequest* request,
    const char* content_type,
    const uint8_t* content_gz,
    size_t content_gz_len,
    const char* cache_control
);
