/*
 * Log Manager Implementation
 * 
 * Indentation-based logger with nested blocks and automatic timing.
 * Routes output to Serial only.
 */

#include "log_manager.h"
#include <stdarg.h>

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

// Begin a log block
void LogManager::logBegin(const char* module) {
    print(indent());
    print("[");
    print(module);
    println("] Starting...");
    
    // Save start time if we haven't exceeded max depth
    if (nestLevel < 3) {
        startTimes[nestLevel] = millis();
    }
    
    // Increment nesting level (but don't overflow)
    if (nestLevel < 255) {
        nestLevel++;
    }
}

// Add a line to current block
void LogManager::logLine(const char* message) {
    print(indent());
    println(message);
}

// Add a formatted line (printf-style)
void LogManager::logLinef(const char* format, ...) {
    char buffer[128];
    va_list args;
    va_start(args, format);
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    
    print(indent());
    println(buffer);
}

// End a log block
void LogManager::logEnd(const char* message) {
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
    
    // Print end message with timing
    const char* msg = (message && strlen(message) > 0) ? message : "Done";
    print(indent());
    print(msg);
    print(" (");
    print(elapsed);
    println("ms)");
}

// Single-line logging with timing
void LogManager::logMessage(const char* module, const char* msg) {
    unsigned long start = millis();
    print(indent());
    print("[");
    print(module);
    print("] ");
    print(msg);
    print(" (");
    print(millis() - start);
    println("ms)");
}

void LogManager::logMessagef(const char* module, const char* format, ...) {
    unsigned long start = millis();
    
    char buffer[128];
    va_list args;
    va_start(args, format);
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    
    print(indent());
    print("[");
    print(module);
    print("] ");
    print(buffer);
    print(" (");
    print(millis() - start);
    println("ms)");
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
    return Serial.write(c);
}

// Write buffer of bytes (required by Print class)
size_t LogManager::write(const uint8_t *buffer, size_t size) {
    return Serial.write(buffer, size);
}
