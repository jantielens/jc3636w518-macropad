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
#define MACROS_BUTTONS_PER_SCREEN 9
#endif

// Keep sizes conservative to avoid RAM pressure.
// Note: macros are stored in NVS as a single blob; size changes invalidate stored config.
#define MACROS_LABEL_MAX_LEN 16
#define MACROS_SCRIPT_MAX_LEN 256
#define MACROS_ICON_ID_MAX_LEN 32

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
};

struct MacroConfig {
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
