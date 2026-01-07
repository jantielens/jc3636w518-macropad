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
#define MACROS_SCRIPT_MAX_LEN 256
#define MACROS_ICON_ID_MAX_LEN 32

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
};

struct MacroButtonConfig {
    char label[MACROS_LABEL_MAX_LEN];
    MacroButtonAction action;
    char script[MACROS_SCRIPT_MAX_LEN];
    char icon_id[MACROS_ICON_ID_MAX_LEN];

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
