#include "ducky_script.h"

#include "ble_keyboard_manager.h"
#include "log_manager.h"

#include <ctype.h>
#include <string.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#if BLE_KEYBOARD_MANAGER_ENABLED

namespace {

constexpr uint32_t kInterStepDelayMs = 8;

static void delayMs(uint32_t ms) {
    if (ms == 0) return;
    vTaskDelay(pdMS_TO_TICKS(ms));
}

static void strToUpperInPlace(char* s) {
    if (!s) return;
    for (; *s; s++) {
        *s = (char)toupper((unsigned char)*s);
    }
}

static char* trimInPlace(char* s) {
    if (!s) return s;

    // Left trim
    while (*s && isspace((unsigned char)*s)) s++;

    // Right trim
    char* end = s + strlen(s);
    while (end > s && isspace((unsigned char)end[-1])) end--;
    *end = '\0';
    return s;
}

static bool tokenEquals(const char* a, const char* b) {
    if (!a || !b) return false;
    return strcasecmp(a, b) == 0;
}

struct KeyToken {
    bool isMedia;
    uint8_t key;
    MediaKeyReport media;
};

static bool parseMediaToken(const char* tok, KeyToken* out) {
    if (!tok || !out) return false;

    if (tokenEquals(tok, "VOLUMEUP")) {
        out->isMedia = true;
        memcpy(out->media, KEY_MEDIA_VOLUME_UP, sizeof(MediaKeyReport));
        return true;
    }
    if (tokenEquals(tok, "VOLUMEDOWN")) {
        out->isMedia = true;
        memcpy(out->media, KEY_MEDIA_VOLUME_DOWN, sizeof(MediaKeyReport));
        return true;
    }
    if (tokenEquals(tok, "MUTE")) {
        out->isMedia = true;
        memcpy(out->media, KEY_MEDIA_MUTE, sizeof(MediaKeyReport));
        return true;
    }
    if (tokenEquals(tok, "PLAYPAUSE")) {
        out->isMedia = true;
        memcpy(out->media, KEY_MEDIA_PLAY_PAUSE, sizeof(MediaKeyReport));
        return true;
    }
    if (tokenEquals(tok, "NEXTTRACK")) {
        out->isMedia = true;
        memcpy(out->media, KEY_MEDIA_NEXT_TRACK, sizeof(MediaKeyReport));
        return true;
    }
    if (tokenEquals(tok, "PREVTRACK") || tokenEquals(tok, "PREV")) {
        out->isMedia = true;
        memcpy(out->media, KEY_MEDIA_PREVIOUS_TRACK, sizeof(MediaKeyReport));
        return true;
    }

    return false;
}

static bool parseKeyToken(const char* tok, KeyToken* out) {
    if (!tok || !*tok || !out) return false;

    // Media keys first
    if (parseMediaToken(tok, out)) return true;

    out->isMedia = false;

    // Single character (letters/digits) — use ASCII path.
    if (strlen(tok) == 1) {
        out->key = (uint8_t)tok[0];
        return true;
    }

    // Normalize common names
    if (tokenEquals(tok, "ENTER") || tokenEquals(tok, "RETURN")) { out->key = KEY_RETURN; return true; }
    if (tokenEquals(tok, "TAB")) { out->key = KEY_TAB; return true; }
    if (tokenEquals(tok, "ESC") || tokenEquals(tok, "ESCAPE")) { out->key = KEY_ESC; return true; }
    if (tokenEquals(tok, "BACKSPACE") || tokenEquals(tok, "BKSP")) { out->key = KEY_BACKSPACE; return true; }
    if (tokenEquals(tok, "SPACE")) { out->key = (uint8_t)' '; return true; }

    if (tokenEquals(tok, "UPARROW") || tokenEquals(tok, "UP")) { out->key = KEY_UP_ARROW; return true; }
    if (tokenEquals(tok, "DOWNARROW") || tokenEquals(tok, "DOWN")) { out->key = KEY_DOWN_ARROW; return true; }
    if (tokenEquals(tok, "LEFTARROW") || tokenEquals(tok, "LEFT")) { out->key = KEY_LEFT_ARROW; return true; }
    if (tokenEquals(tok, "RIGHTARROW") || tokenEquals(tok, "RIGHT")) { out->key = KEY_RIGHT_ARROW; return true; }

    if (tokenEquals(tok, "HOME")) { out->key = KEY_HOME; return true; }
    if (tokenEquals(tok, "END")) { out->key = KEY_END; return true; }
    if (tokenEquals(tok, "PAGEUP")) { out->key = KEY_PAGE_UP; return true; }
    if (tokenEquals(tok, "PAGEDOWN")) { out->key = KEY_PAGE_DOWN; return true; }

    // Function keys
    if (tok[0] == 'F' || tok[0] == 'f') {
        const int n = atoi(tok + 1);
        switch (n) {
            case 1: out->key = KEY_F1; return true;
            case 2: out->key = KEY_F2; return true;
            case 3: out->key = KEY_F3; return true;
            case 4: out->key = KEY_F4; return true;
            case 5: out->key = KEY_F5; return true;
            case 6: out->key = KEY_F6; return true;
            case 7: out->key = KEY_F7; return true;
            case 8: out->key = KEY_F8; return true;
            case 9: out->key = KEY_F9; return true;
            case 10: out->key = KEY_F10; return true;
            case 11: out->key = KEY_F11; return true;
            case 12: out->key = KEY_F12; return true;
            default: break;
        }
    }

    return false;
}

static bool parseModifierToken(const char* tok, uint8_t* keyOut) {
    if (!tok || !keyOut) return false;

    if (tokenEquals(tok, "CTRL") || tokenEquals(tok, "CONTROL")) { *keyOut = KEY_LEFT_CTRL; return true; }
    if (tokenEquals(tok, "SHIFT")) { *keyOut = KEY_LEFT_SHIFT; return true; }
    if (tokenEquals(tok, "ALT")) { *keyOut = KEY_LEFT_ALT; return true; }
    if (tokenEquals(tok, "GUI") || tokenEquals(tok, "WIN") || tokenEquals(tok, "CMD")) { *keyOut = KEY_LEFT_GUI; return true; }

    return false;
}

static void sendKeyToken(const KeyToken& tok, BleKeyboardManager* keyboard) {
    if (!keyboard) return;

    if (tok.isMedia) {
        keyboard->tapMedia(tok.media);
    } else {
        keyboard->tap(tok.key);
    }
}

} // namespace

bool ducky_execute(const char* script, BleKeyboardManager* keyboard) {
    if (!script || !*script) return false;
    if (!keyboard) return false;

    if (!keyboard->enabled()) {
        Logger.logMessage("Ducky", "BLE keyboard is not enabled in this build");
        return false;
    }

    if (!keyboard->isConnected()) {
        Logger.logMessage("Ducky", "BLE keyboard not connected; macro skipped");
        return false;
    }

    // Copy into a scratch buffer so we can split lines in-place.
    // Macro payload strings are bounded by MACROS_PAYLOAD_MAX_LEN (256), but keep a little headroom.
    char buf[384];
    strlcpy(buf, script, sizeof(buf));

    char* cursor = buf;
    while (cursor && *cursor) {
        // Find end-of-line
        char* line = cursor;
        char* nl = strpbrk(cursor, "\r\n");
        if (nl) {
            // Null-terminate this line
            *nl = '\0';

            // Advance cursor past any CR/LF combo
            cursor = nl + 1;
            while (*cursor == '\r' || *cursor == '\n') cursor++;
        } else {
            cursor = nullptr;
        }

        char* trimmed = trimInPlace(line);
        if (!trimmed || !*trimmed) continue;

        // Comments
        if (strncasecmp(trimmed, "REM", 3) == 0 && (trimmed[3] == '\0' || isspace((unsigned char)trimmed[3]))) {
            continue;
        }
        if (trimmed[0] == '#' || (trimmed[0] == '/' && trimmed[1] == '/')) {
            continue;
        }

        // STRING <text>
        if (strncasecmp(trimmed, "STRING", 6) == 0 && isspace((unsigned char)trimmed[6])) {
            const char* text = trimmed + 6;
            while (*text && isspace((unsigned char)*text)) text++;
            keyboard->sendText(text);
            delayMs(kInterStepDelayMs);
            continue;
        }

        // DELAY <ms>
        if (strncasecmp(trimmed, "DELAY", 5) == 0 && isspace((unsigned char)trimmed[5])) {
            const char* msStr = trimmed + 5;
            while (*msStr && isspace((unsigned char)*msStr)) msStr++;
            const long ms = strtol(msStr, nullptr, 10);
            if (ms > 0) delayMs((uint32_t)ms);
            continue;
        }

        // Tokenize by whitespace
        // Make a working copy to uppercase tokens as needed.
        char work[256];
        strlcpy(work, trimmed, sizeof(work));

        uint8_t modifiers[4];
        int modifierCount = 0;

        char* saveptr = nullptr;
        char* tok = strtok_r(work, " \t", &saveptr);

        // Collect modifier tokens
        while (tok) {
            uint8_t modKey = 0;
            if (parseModifierToken(tok, &modKey)) {
                if (modifierCount < (int)(sizeof(modifiers) / sizeof(modifiers[0]))) {
                    modifiers[modifierCount++] = modKey;
                }
                tok = strtok_r(nullptr, " \t", &saveptr);
                continue;
            }
            break;
        }

        if (!tok) {
            continue;
        }

        KeyToken keyTok;
        if (!parseKeyToken(tok, &keyTok)) {
            Logger.logMessagef("Ducky", "Unknown token: %s", tok);
            continue;
        }

        // Send chord or single token
        for (int i = 0; i < modifierCount; i++) {
            keyboard->press(modifiers[i]);
        }

        sendKeyToken(keyTok, keyboard);
        delayMs(kInterStepDelayMs);

        // Release modifiers and any sticky keys
        keyboard->releaseAll();
        delayMs(kInterStepDelayMs);
    }

    return true;
}

#else

bool ducky_execute(const char* script, BleKeyboardManager* keyboard) {
    (void)script;
    (void)keyboard;
    Logger.logMessage("Ducky", "BLE keyboard is not enabled in this build");
    return false;
}

#endif // BLE_KEYBOARD_MANAGER_ENABLED
