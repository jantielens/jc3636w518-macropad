#ifndef MACROS_CONFIG_H
#define MACROS_CONFIG_H

#include <Arduino.h>

// Ensure board-specific overrides (if any) are visible here even when a source
// file includes macros_config.h before board_config.h.
// This prevents mismatched MacroConfig layouts across translation units.
#ifdef BOARD_HAS_OVERRIDE
#include "board_overrides.h"
#endif

// MVP defaults: fixed 8 screens × 9 buttons
// For debugging (or future variants), these can be overridden at compile-time
// (e.g. via board_overrides.h) before including this header.
#ifndef MACROS_SCREEN_COUNT
#define MACROS_SCREEN_COUNT 8
#endif

#ifndef MACROS_BUTTONS_PER_SCREEN
#define MACROS_BUTTONS_PER_SCREEN 16
#endif

// Keep sizes conservative to avoid RAM pressure.
// Note: macros are stored in NVS as a single blob; size changes invalidate stored config.
#define MACROS_LABEL_MAX_LEN 16
#define MACROS_PAYLOAD_MAX_LEN 256
#define MACROS_MQTT_TOPIC_MAX_LEN 128
#define MACROS_ICON_ID_MAX_LEN 32
// UTF-8 bytes (not Unicode code points). Keep generous to support ZWJ emoji sequences.
#define MACROS_ICON_DISPLAY_MAX_LEN 64

// Color values are stored as 0xRRGGBB (RGB only).
// Use 0xFFFFFFFF to indicate an unset optional override (fall back to defaults).
#define MACROS_COLOR_UNSET 0xFFFFFFFFu

// Template IDs are stored per macro screen.
// Keep this large enough for descriptive IDs like "round_stack_sides_5".
#define MACROS_TEMPLATE_ID_MAX_LEN 32

enum class MacroButtonAction : uint8_t {
    None = 0,
    SendKeys = 1,
    NavPrevScreen = 2,
    NavNextScreen = 3,
    NavToScreen = 4,
    GoBack = 5,
    MqttSend = 6,
};

enum class MacroIconType : uint8_t {
    None = 0,
    Builtin = 1, // compiled monochrome (mask) icons
    Emoji = 2,   // Twemoji (installed to FFat)
    Asset = 3,   // user-uploaded image (installed to FFat)
};

struct MacroButtonIcon {
    MacroIconType type;
    // Stable lookup ID used by firmware (e.g. "mic_off", "emoji_u1f381", "user_ab12cd34").
    char id[MACROS_ICON_ID_MAX_LEN];
    // Human-friendly display value for the portal UI (emoji literal or filename/label).
    char display[MACROS_ICON_DISPLAY_MAX_LEN];
};

struct MacroButtonConfig {
    char label[MACROS_LABEL_MAX_LEN];
    MacroButtonAction action;
    // Generic action payload.
    // - SendKeys: DuckyScript-like payload
    // - NavToScreen: target screen id (e.g. "macro1", "info")
    // - MqttSend: payload string (may be empty)
    // - Others: unused for now
    char payload[MACROS_PAYLOAD_MAX_LEN];

    // Per-button MQTT topic (full topic, no auto-prefixing).
    // Used only for MqttSend.
    char mqtt_topic[MACROS_MQTT_TOPIC_MAX_LEN];
    MacroButtonIcon icon;

    // Optional per-button appearance overrides (0xRRGGBB or MACROS_COLOR_UNSET).
    uint32_t button_bg;
    uint32_t icon_color;  // Only used for mask icons (IconKind::Mask)
    uint32_t label_color;
};

struct MacroConfig {
    // Global defaults used when per-screen/per-button values are unset.
    uint32_t default_screen_bg;
    uint32_t default_button_bg;
    uint32_t default_icon_color;
    uint32_t default_label_color;

    // Optional per-screen background overrides (0xRRGGBB or MACROS_COLOR_UNSET).
    uint32_t screen_bg[MACROS_SCREEN_COUNT];

    // Template id per macro screen (NUL-terminated).
    // Stored alongside buttons in the same blob/file.
    char template_id[MACROS_SCREEN_COUNT][MACROS_TEMPLATE_ID_MAX_LEN];
    MacroButtonConfig buttons[MACROS_SCREEN_COUNT][MACROS_BUTTONS_PER_SCREEN];
};

void macros_config_set_defaults(MacroConfig* cfg);

// Returns true when a valid config was loaded.
bool macros_config_load(MacroConfig* cfg);

// Returns true when write succeeded.
bool macros_config_save(const MacroConfig* cfg);

// Clears stored macros config from NVS.
bool macros_config_reset();

#endif // MACROS_CONFIG_H
