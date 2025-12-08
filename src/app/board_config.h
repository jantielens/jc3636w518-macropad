#ifndef BOARD_CONFIG_H
#define BOARD_CONFIG_H

// ============================================================================
// Board Configuration - Default Settings
// ============================================================================
// This file provides default configuration for all boards.
// To customize for a specific board, create: src/boards/[board-name]/board_config.h
// The build system will automatically use board-specific overrides if present.
//
// Example board-specific override:
//   src/boards/esp32/board_config.h
//   src/boards/esp32c3/board_config.h
//   src/boards/esp32s3_with_display/board_config.h

// ============================================================================
// Hardware Capabilities
// ============================================================================

// Built-in LED
#ifndef HAS_BUILTIN_LED
#define HAS_BUILTIN_LED false
#endif

#ifndef LED_PIN
#define LED_PIN 2  // Common GPIO for ESP32 boards
#endif

#ifndef LED_ACTIVE_HIGH
#define LED_ACTIVE_HIGH true  // true = HIGH turns LED on, false = LOW turns LED on
#endif

// ============================================================================
// WiFi Configuration
// ============================================================================

#ifndef WIFI_MAX_ATTEMPTS
#define WIFI_MAX_ATTEMPTS 3
#endif

// ============================================================================
// Additional default configuration settings
// ============================================================================

// Example: Define additional pins for sensors, buttons, etc.
// #ifndef BUTTON_PIN
// #define BUTTON_PIN 0
// #endif

#endif // BOARD_CONFIG_H

// ============================================================================
// Board-specific overrides
// ============================================================================
// build.sh defines BOARD_HAS_OVERRIDE when a board override directory exists
// and adds that directory to the include path. Using include_next allows this
// generic header to pull in the board-specific board_config.h without touching
// application code.

#ifdef BOARD_HAS_OVERRIDE
#include_next "board_config.h"
#endif

