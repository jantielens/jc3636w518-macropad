#pragma once

#include <stddef.h>
#include <stdint.h>

// Spotify POC manager:
// - Handles OAuth PKCE flow (start + complete via main loop)
// - Stores refresh token in NVS
// - Polls now-playing when the Spotify screen is active
// - Downloads + decodes album art into PSRAM-backed RGB565 for LVGL

struct SpotifyNowPlaying {
    bool valid = false;
    bool is_playing = false;

    char track_name[96] = {0};
    char artist_name[96] = {0};

    // Best-fit album art URL (selected from Spotify response)
    char art_url[512] = {0};

    // A stable-ish identity to detect track changes (track id or uri)
    char track_id[96] = {0};
};

struct SpotifyImage {
    // RGB565 pixel buffer allocated with heap_caps_malloc (prefer PSRAM)
    uint16_t* pixels = nullptr;
    int w = 0;
    int h = 0;
};

namespace spotify_manager {

void init();
void loop();

// Screen activity hint (to avoid polling when not visible).
void set_active(bool active);

// OAuth
bool begin_auth(char* out_authorize_url, size_t out_len, char* out_state, size_t state_len);

// Queue completing auth; actual token exchange happens in loop().
bool queue_complete_auth(const char* code, const char* state);

// Status
bool is_connected();

// Returns a snapshot of last now-playing state (thread-safe-ish: copies).
SpotifyNowPlaying get_now_playing();

// Returns true and transfers ownership of the current decoded image (if available).
// Caller owns the buffer and must free with heap_caps_free.
bool take_image(SpotifyImage* out);

// Controls (queued, executed in loop)
void request_prev();
void request_next();

// Disconnect
void disconnect();

}
