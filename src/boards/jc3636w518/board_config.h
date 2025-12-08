#ifndef BOARD_CONFIG_JC3636W518_H
#define BOARD_CONFIG_JC3636W518_H

// ============================================================================
// JC3636W518 (ESP32-S3 round display) Board Configuration
// ============================================================================
// Overrides defaults in src/app/board_config.h

// Pull in defaults first
#include "../../app/board_config.h"

// Ensure WiFi defaults exist (safety net if include resolution changes)
#ifndef WIFI_MAX_ATTEMPTS
#define WIFI_MAX_ATTEMPTS 3
#endif

#define HAS_DISPLAY true
#define DISPLAY_WIDTH 360
#define DISPLAY_HEIGHT 360
#define DISPLAY_DEFAULT_ROTATION 0

// Optional: Built-in LED specifics (unknown; keep defaults)
// #define HAS_BUILTIN_LED true
// #define LED_PIN <gpio>

// Display initialization hooks (implemented in display_driver.cpp)
void board_display_init();
void board_display_loop();

#endif // BOARD_CONFIG_JC3636W518_H
