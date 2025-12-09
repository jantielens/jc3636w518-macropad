/*
 * Keystrokes Handler Implementation
 * 
 * Parses keystrokes strings and executes them via BLE keyboard.
 * Format: comma-separated actions with 50ms default delay between them.
 */

#include "keystrokes_handler.h"
#include "log_manager.h"
#include <Arduino.h>

// ============================================================================
// Global State
// ============================================================================

static BleKeyboard* g_keyboard = nullptr;
static const unsigned long DEFAULT_ACTION_DELAY_MS = 50;

// ============================================================================
// Key Lookup Tables
// ============================================================================

struct KeyMapping {
    const char* name;
    uint8_t code;
};

struct MediaKeyMapping {
    const char* name;
    const MediaKeyReport* report;
};

// Special keys (arrows, navigation, etc.)
static const KeyMapping SPECIAL_KEYS[] = {
    {"enter", KEY_RETURN},
    {"return", KEY_RETURN},
    {"tab", KEY_TAB},
    {"esc", KEY_ESC},
    {"escape", KEY_ESC},
    {"backspace", KEY_BACKSPACE},
    {"delete", KEY_DELETE},
    {"del", KEY_DELETE},
    {"insert", KEY_INSERT},
    {"ins", KEY_INSERT},
    {"home", KEY_HOME},
    {"end", KEY_END},
    {"pageup", KEY_PAGE_UP},
    {"pagedown", KEY_PAGE_DOWN},
    {"pgup", KEY_PAGE_UP},
    {"pgdn", KEY_PAGE_DOWN},
    {"up", KEY_UP_ARROW},
    {"down", KEY_DOWN_ARROW},
    {"left", KEY_LEFT_ARROW},
    {"right", KEY_RIGHT_ARROW},
    {"capslock", KEY_CAPS_LOCK},
    {"caps", KEY_CAPS_LOCK},
};

// Function keys
static const KeyMapping FUNCTION_KEYS[] = {
    {"f1", KEY_F1}, {"f2", KEY_F2}, {"f3", KEY_F3}, {"f4", KEY_F4},
    {"f5", KEY_F5}, {"f6", KEY_F6}, {"f7", KEY_F7}, {"f8", KEY_F8},
    {"f9", KEY_F9}, {"f10", KEY_F10}, {"f11", KEY_F11}, {"f12", KEY_F12},
    {"f13", KEY_F13}, {"f14", KEY_F14}, {"f15", KEY_F15}, {"f16", KEY_F16},
    {"f17", KEY_F17}, {"f18", KEY_F18}, {"f19", KEY_F19}, {"f20", KEY_F20},
    {"f21", KEY_F21}, {"f22", KEY_F22}, {"f23", KEY_F23}, {"f24", KEY_F24},
};

// Modifier keys
static const KeyMapping MODIFIER_KEYS[] = {
    {"ctrl", KEY_LEFT_CTRL},
    {"control", KEY_LEFT_CTRL},
    {"shift", KEY_LEFT_SHIFT},
    {"alt", KEY_LEFT_ALT},
    {"option", KEY_LEFT_ALT},  // Mac alias
    {"cmd", KEY_LEFT_GUI},
    {"command", KEY_LEFT_GUI},
    {"win", KEY_LEFT_GUI},
    {"windows", KEY_LEFT_GUI},
    {"gui", KEY_LEFT_GUI},
};

// Media keys
static const MediaKeyMapping MEDIA_KEYS[] = {
    {"volumeup", &KEY_MEDIA_VOLUME_UP},
    {"volumedown", &KEY_MEDIA_VOLUME_DOWN},
    {"mute", &KEY_MEDIA_MUTE},
    {"playpause", &KEY_MEDIA_PLAY_PAUSE},
    {"play", &KEY_MEDIA_PLAY_PAUSE},
    {"pause", &KEY_MEDIA_PLAY_PAUSE},
    {"nexttrack", &KEY_MEDIA_NEXT_TRACK},
    {"next", &KEY_MEDIA_NEXT_TRACK},
    {"prevtrack", &KEY_MEDIA_PREVIOUS_TRACK},
    {"previous", &KEY_MEDIA_PREVIOUS_TRACK},
    {"prev", &KEY_MEDIA_PREVIOUS_TRACK},
    {"stop", &KEY_MEDIA_STOP},
    {"wwwhome", &KEY_MEDIA_WWW_HOME},
    {"home", &KEY_MEDIA_WWW_HOME},
    {"calculator", &KEY_MEDIA_CALCULATOR},
    {"calc", &KEY_MEDIA_CALCULATOR},
    {"mail", &KEY_MEDIA_EMAIL_READER},
    {"email", &KEY_MEDIA_EMAIL_READER},
};

// ============================================================================
// Helper Functions
// ============================================================================

// Convert string to lowercase (in place for buffer, or create new String)
static String toLowercase(const String& str) {
    String result = str;
    result.toLowerCase();
    return result;
}

// Trim whitespace from string
static String trim(const String& str) {
    String result = str;
    result.trim();
    return result;
}

// Check if string is enclosed in braces: {KeyName}
static bool isBracedKey(const String& str) {
    return str.startsWith("{") && str.endsWith("}");
}

// Extract content from braces: {Enter} → enter
static String extractBracedContent(const String& str) {
    if (isBracedKey(str)) {
        return str.substring(1, str.length() - 1);
    }
    return str;
}

// Check if action contains a modifier combo (e.g., Ctrl+C)
static bool isModifierCombo(const String& action) {
    return action.indexOf('+') != -1 && !isBracedKey(action);
}

// Lookup special key code by name (case-insensitive)
static bool lookupSpecialKey(const String& name, uint8_t* code) {
    String lowerName = toLowercase(name);
    
    // Check special keys
    for (size_t i = 0; i < sizeof(SPECIAL_KEYS) / sizeof(SPECIAL_KEYS[0]); i++) {
        if (lowerName == SPECIAL_KEYS[i].name) {
            *code = SPECIAL_KEYS[i].code;
            return true;
        }
    }
    
    // Check function keys
    for (size_t i = 0; i < sizeof(FUNCTION_KEYS) / sizeof(FUNCTION_KEYS[0]); i++) {
        if (lowerName == FUNCTION_KEYS[i].name) {
            *code = FUNCTION_KEYS[i].code;
            return true;
        }
    }
    
    return false;
}

// Lookup media key by name (case-insensitive)
static bool lookupMediaKey(const String& name, const MediaKeyReport** report) {
    String lowerName = toLowercase(name);
    
    for (size_t i = 0; i < sizeof(MEDIA_KEYS) / sizeof(MEDIA_KEYS[0]); i++) {
        if (lowerName == MEDIA_KEYS[i].name) {
            *report = MEDIA_KEYS[i].report;
            return true;
        }
    }
    
    return false;
}

// Lookup modifier key by name (case-insensitive)
static bool lookupModifier(const String& name, uint8_t* code) {
    String lowerName = toLowercase(name);
    
    for (size_t i = 0; i < sizeof(MODIFIER_KEYS) / sizeof(MODIFIER_KEYS[0]); i++) {
        if (lowerName == MODIFIER_KEYS[i].name) {
            *code = MODIFIER_KEYS[i].code;
            return true;
        }
    }
    
    return false;
}

// ============================================================================
// Action Executors
// ============================================================================

// Execute plain text (types character by character with delay for BLE stability)
static bool executePlainText(const String& text) {
    if (text.length() == 0) return true;
    
    Logger.logMessagef("Keystroke", "Type: '%s'", text.c_str());
    
    // Type each character individually with a small delay for BLE keyboard
    // BLE needs time to process each keypress/release cycle
    for (size_t i = 0; i < text.length(); i++) {
        g_keyboard->write(text[i]);
        if (i < text.length() - 1) {  // No delay after last character
            delay(10);  // 10ms between characters
        }
    }
    
    return true;
}

// Execute special key (e.g., Enter, Tab, F1)
static bool executeSpecialKey(const String& keyName) {
    uint8_t code;
    if (!lookupSpecialKey(keyName, &code)) {
        Logger.logMessagef("Keystroke", "ERROR: Unknown special key '%s'", keyName.c_str());
        return false;
    }
    
    Logger.logMessagef("Keystroke", "Press: {%s}", keyName.c_str());
    g_keyboard->press(code);
    delay(10);  // Small delay for key registration
    g_keyboard->release(code);
    return true;
}

// Execute media key (e.g., VolumeUp, PlayPause)
static bool executeMediaKey(const String& keyName) {
    const MediaKeyReport* report;
    if (!lookupMediaKey(keyName, &report)) {
        Logger.logMessagef("Keystroke", "ERROR: Unknown media key '%s'", keyName.c_str());
        return false;
    }
    
    Logger.logMessagef("Keystroke", "Media: {%s}", keyName.c_str());
    g_keyboard->press(*report);
    delay(10);
    g_keyboard->release(*report);
    return true;
}

// Execute modifier combo (e.g., Ctrl+C, Shift+Alt+F)
static bool executeModifierCombo(const String& combo) {
    // Split by '+' to get modifier(s) and final key
    String parts[8];  // Support up to 8 parts
    int partCount = 0;
    int lastIndex = 0;
    
    for (int i = 0; i <= combo.length(); i++) {
        if (i == combo.length() || combo.charAt(i) == '+') {
            if (partCount < 8) {
                parts[partCount++] = trim(combo.substring(lastIndex, i));
            }
            lastIndex = i + 1;
        }
    }
    
    if (partCount < 2) {
        Logger.logMessagef("Keystroke", "ERROR: Invalid combo '%s'", combo.c_str());
        return false;
    }
    
    // All parts except last should be modifiers
    uint8_t modifiers[7];
    int modCount = 0;
    
    for (int i = 0; i < partCount - 1; i++) {
        uint8_t modCode;
        if (lookupModifier(parts[i], &modCode)) {
            modifiers[modCount++] = modCode;
        } else {
            Logger.logMessagef("Keystroke", "ERROR: Unknown modifier '%s'", parts[i].c_str());
            return false;
        }
    }
    
    // Last part is the key to press
    String keyPart = parts[partCount - 1];
    uint8_t keyCode;
    bool isSpecial = lookupSpecialKey(keyPart, &keyCode);
    
    // If not special key, treat as regular character
    if (!isSpecial) {
        if (keyPart.length() == 1) {
            keyCode = (uint8_t)keyPart.charAt(0);
        } else {
            Logger.logMessagef("Keystroke", "ERROR: Invalid key '%s' in combo", keyPart.c_str());
            return false;
        }
    }
    
    Logger.logMessagef("Keystroke", "Combo: %s", combo.c_str());
    
    // Press all modifiers
    for (int i = 0; i < modCount; i++) {
        g_keyboard->press(modifiers[i]);
    }
    
    // Press the key
    g_keyboard->press(keyCode);
    delay(10);
    
    // Release the key
    g_keyboard->release(keyCode);
    
    // Release all modifiers (in reverse order)
    for (int i = modCount - 1; i >= 0; i--) {
        g_keyboard->release(modifiers[i]);
    }
    
    return true;
}

// Execute wait/delay action
static bool executeWait(const String& waitSpec) {
    // Extract number from "Wait:100" → 100
    int colonIndex = waitSpec.indexOf(':');
    if (colonIndex == -1) {
        Logger.logMessage("Keystroke", "Wait: 100ms (default)");
        delay(100);
        return true;
    }
    
    String delayStr = waitSpec.substring(colonIndex + 1);
    int delayMs = delayStr.toInt();
    
    if (delayMs <= 0 || delayMs > 10000) {
        Logger.logMessagef("Keystroke", "ERROR: Invalid wait time '%s', using 100ms", delayStr.c_str());
        delayMs = 100;
    }
    
    Logger.logMessagef("Keystroke", "Wait: %dms", delayMs);
    delay(delayMs);
    return true;
}

// Execute a single action
static bool executeSingleAction(const String& action) {
    String trimmedAction = trim(action);
    
    if (trimmedAction.length() == 0) {
        return true;  // Empty action, skip silently
    }
    
    // Check for braced keys: {Enter}, {VolumeUp}, {Wait:100}
    if (isBracedKey(trimmedAction)) {
        String keyName = extractBracedContent(trimmedAction);
        String lowerKeyName = toLowercase(keyName);
        
        // Check for wait command
        if (lowerKeyName.startsWith("wait")) {
            return executeWait(lowerKeyName);
        }
        
        // Try media key first (more specific)
        const MediaKeyReport* mediaReport;
        if (lookupMediaKey(keyName, &mediaReport)) {
            return executeMediaKey(keyName);
        }
        
        // Try special key
        uint8_t specialCode;
        if (lookupSpecialKey(keyName, &specialCode)) {
            return executeSpecialKey(keyName);
        }
        
        Logger.logMessagef("Keystroke", "ERROR: Unknown key '{%s}'", keyName.c_str());
        return false;
    }
    
    // Check for modifier combo: Ctrl+C, Shift+Alt+F
    if (isModifierCombo(trimmedAction)) {
        return executeModifierCombo(trimmedAction);
    }
    
    // Otherwise, treat as plain text
    return executePlainText(trimmedAction);
}

// ============================================================================
// Public API
// ============================================================================

bool keystrokes_handler_init(BleKeyboard* keyboard) {
    if (!keyboard) {
        Logger.logMessage("Keystroke", "ERROR: NULL keyboard pointer");
        return false;
    }
    
    g_keyboard = keyboard;
    Logger.logMessage("Keystroke", "Handler initialized");
    return true;
}

bool keystrokes_handler_execute(const char* keystrokes) {
    if (!g_keyboard) {
        Logger.logMessage("Keystroke", "ERROR: Handler not initialized");
        return false;
    }
    
    if (!keystrokes || strlen(keystrokes) == 0) {
        Logger.logMessage("Keystroke", "ERROR: Empty keystrokes string");
        return false;
    }
    
    if (!g_keyboard->isConnected()) {
        Logger.logMessage("Keystroke", "ERROR: BLE not connected");
        return false;
    }
    
    Logger.logMessagef("Keystroke", "Execute: '%s'", keystrokes);
    
    // Split by comma to get individual actions
    String keystrokesStr = String(keystrokes);
    int actionCount = 0;
    int lastIndex = 0;
    
    for (int i = 0; i <= keystrokesStr.length(); i++) {
        if (i == keystrokesStr.length() || keystrokesStr.charAt(i) == ',') {
            String action = keystrokesStr.substring(lastIndex, i);
            
            if (action.length() > 0) {
                if (!executeSingleAction(action)) {
                    Logger.logMessagef("Keystroke", "ERROR: Failed to execute action '%s'", action.c_str());
                    // Continue with next action despite error
                }
                
                actionCount++;
                
                // Add delay between actions (except after last action)
                if (i < keystrokesStr.length()) {
                    delay(DEFAULT_ACTION_DELAY_MS);
                }
            }
            
            lastIndex = i + 1;
        }
    }
    
    Logger.logMessagef("Keystroke", "Completed %d action(s)", actionCount);
    return true;
}
