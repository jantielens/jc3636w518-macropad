/*
 * Log Manager Implementation
 * 
 * Indentation-based logger with nested blocks and automatic timing.
 * Routes output to Serial only.
 */

#include "log_manager.h"
#include <stdarg.h>

namespace {

static inline bool serial_ready_for_logging() {
    // On some ESP32-Sx USB-CDC configurations, printing very early when no host has
    // opened the CDC port can be unstable. If no host is connected, drop logs.
#if defined(ARDUINO_USB_CDC_ON_BOOT) && ARDUINO_USB_CDC_ON_BOOT
    return (bool)Serial;
#else
    return true;
#endif
}

}

// Initialize static members
unsigned long LogManager::startTimes[3] = {0, 0, 0};
uint8_t LogManager::nestLevel = 0;

// Global LogManager instance
LogManager Logger;

// Constructor
LogManager::LogManager() {
}

// Initialize (sets baud rate for Serial)
void LogManager::begin(unsigned long baud) {
    Serial.begin(baud);
}

// Get indentation string based on nesting level
const char* LogManager::indent() {
    static const char* indents[] = {
        "",         // Level 0: no indent
        "  ",       // Level 1: 2 spaces
        "    ",     // Level 2: 4 spaces
        "      "    // Level 3+: 6 spaces
    };
    
    uint8_t level = nestLevel;
    if (level > 3) level = 3; // Cap at 3 for indentation
    return indents[level];
}

// Begin a log block - atomic write
void LogManager::logBegin(const char* module) {
    if (!serial_ready_for_logging()) return;
    char line[128];
    snprintf(line, sizeof(line), "%s[%s] Starting...\n", indent(), module);
    Serial.print(line);
    
    // Save start time if we haven't exceeded max depth
    if (nestLevel < 3) {
        startTimes[nestLevel] = millis();
    }
    
    // Increment nesting level (but don't overflow)
    if (nestLevel < 255) {
        nestLevel++;
    }
}

// Add a line to current block - atomic write
void LogManager::logLine(const char* message) {
    if (!serial_ready_for_logging()) return;
    char line[160];
    snprintf(line, sizeof(line), "%s%s\n", indent(), message);
    Serial.print(line);
}

// Add a formatted line (printf-style) - atomic write
void LogManager::logLinef(const char* format, ...) {
    if (!serial_ready_for_logging()) return;
    char msgbuf[128];
    va_list args;
    va_start(args, format);
    vsnprintf(msgbuf, sizeof(msgbuf), format, args);
    va_end(args);
    
    char line[160];
    snprintf(line, sizeof(line), "%s%s\n", indent(), msgbuf);
    Serial.print(line);
}

// End a log block - atomic write
void LogManager::logEnd(const char* message) {
    if (!serial_ready_for_logging()) return;
    // Decrement nesting level first (but don't underflow)
    if (nestLevel > 0) {
        nestLevel--;
    } else {
        // Extra end() calls are ignored gracefully
        return;
    }
    
    // Calculate elapsed time (0ms if we exceeded max depth)
    unsigned long elapsed = 0;
    if (nestLevel < 3) {
        elapsed = millis() - startTimes[nestLevel];
    }
    
    // Print end message with timing - atomic
    const char* msg = (message && strlen(message) > 0) ? message : "Done";
    char line[128];
    snprintf(line, sizeof(line), "%s%s (%lums)\n", indent(), msg, elapsed);
    Serial.print(line);
}

// Single-line logging with timing - atomic write to prevent interleaving
void LogManager::logMessage(const char* module, const char* msg) {
    if (!serial_ready_for_logging()) return;
    unsigned long start = millis();
    char line[192];
    snprintf(line, sizeof(line), "%s[%s] %s (%lums)\n", indent(), module, msg, millis() - start);
    Serial.print(line);
}

void LogManager::logMessagef(const char* module, const char* format, ...) {
    if (!serial_ready_for_logging()) return;
    unsigned long start = millis();
    
    char msgbuf[128];
    va_list args;
    va_start(args, format);
    vsnprintf(msgbuf, sizeof(msgbuf), format, args);
    va_end(args);
    
    char line[192];
    snprintf(line, sizeof(line), "%s[%s] %s (%lums)\n", indent(), module, msgbuf, millis() - start);
    Serial.print(line);
}

// Aliases for logMessage (for backward compatibility)
void LogManager::logQuick(const char* module, const char* msg) {
    logMessage(module, msg);
}

void LogManager::logQuickf(const char* module, const char* format, ...) {
    va_list args;
    va_start(args, format);
    char buffer[128];
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    logMessage(module, buffer);
}

// Write single byte (required by Print class)
size_t LogManager::write(uint8_t c) {
    if (!serial_ready_for_logging()) return 1;
    return Serial.write(c);
}

// Write buffer of bytes (required by Print class)
size_t LogManager::write(const uint8_t *buffer, size_t size) {
    if (!serial_ready_for_logging()) return size;
    return Serial.write(buffer, size);
}
