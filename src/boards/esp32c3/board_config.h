#ifndef BOARD_CONFIG_H
#define BOARD_CONFIG_H

// ============================================================================
// ESP32-C3 Super Mini Board Configuration
// ============================================================================
// This file overrides default settings in src/app/board_config.h
// for the ESP32-C3 Super Mini board.
//
// Only include settings that differ from defaults.
// All other settings will use defaults from src/app/board_config.h

// Built-in LED on ESP32-C3 Super Mini is on GPIO8 (not GPIO2 like ESP32)
#define HAS_BUILTIN_LED true
#define LED_PIN 8

// Example: Board-specific function
// Uncomment both lines to enable board-specific code demonstration:
// #define HAS_CUSTOM_IDENTIFIER
// const char* board_get_custom_identifier();

#endif // BOARD_CONFIG_H
