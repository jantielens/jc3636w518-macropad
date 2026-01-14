#pragma once

#include <stddef.h>

// Minimal NVS-backed storage for Spotify OAuth tokens.
// Stored separately from DeviceConfig to avoid inflating /api/config.

namespace spotify_store {

// Returns true when a refresh token is present.
bool has_refresh_token();

// Loads the refresh token into out (NUL-terminated). Returns true on success.
bool load_refresh_token(char* out, size_t out_len);

// Saves refresh token (NUL-terminated). Returns true on success.
bool save_refresh_token(const char* token);

// Clears the stored refresh token.
bool clear_refresh_token();

}
