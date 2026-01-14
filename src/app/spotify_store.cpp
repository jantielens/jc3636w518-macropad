#include "spotify_store.h"

#include "log_manager.h"

#include <Preferences.h>
#include <string.h>

namespace {
static constexpr const char* kNs = "spotify";
static constexpr const char* kKeyRefresh = "refresh";
static Preferences prefs;
}

namespace spotify_store {

bool has_refresh_token() {
    char tmp[8];
    tmp[0] = '\0';
    if (!prefs.begin(kNs, true)) return false;
    prefs.getString(kKeyRefresh, tmp, sizeof(tmp));
    prefs.end();
    return tmp[0] != '\0';
}

bool load_refresh_token(char* out, size_t out_len) {
    if (!out || out_len == 0) return false;
    out[0] = '\0';

    if (!prefs.begin(kNs, true)) return false;
    prefs.getString(kKeyRefresh, out, out_len);
    prefs.end();

    return out[0] != '\0';
}

bool save_refresh_token(const char* token) {
    if (!token || token[0] == '\0') return false;
    if (!prefs.begin(kNs, false)) return false;
    const size_t written = prefs.putString(kKeyRefresh, token);
    prefs.end();

    Logger.logMessage("Spotify", written > 0 ? "Saved refresh token" : "Failed to save refresh token");
    return written > 0;
}

bool clear_refresh_token() {
    if (!prefs.begin(kNs, false)) return false;
    const bool ok = prefs.remove(kKeyRefresh);
    prefs.end();

    Logger.logMessage("Spotify", ok ? "Cleared refresh token" : "Failed to clear refresh token");
    return ok;
}

}
