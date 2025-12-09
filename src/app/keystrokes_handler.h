/*
 * Keystrokes Handler
 * 
 * Parses and executes keystrokes strings via BLE keyboard.
 * Supports plain text, special keys, modifiers, media keys, and sequences.
 * 
 * String Format Examples:
 *   "hello world"           - Types text character by character
 *   "Ctrl+C"                - Press Ctrl+C
 *   "{Enter}"               - Press Enter key
 *   "Ctrl+A,Ctrl+C"         - Press Ctrl+A, then Ctrl+C (50ms delay between)
 *   "{Wait:100}"            - Wait 100 milliseconds
 *   "{VolumeUp}"            - Press volume up media key
 *   "Cmd+Space,term,{Enter}" - Mac Spotlight workflow
 * 
 * Key names are case-insensitive: "ctrl+c" = "Ctrl+C" = "CTRL+C"
 * Plain text preserves case: "Hello" types "Hello" not "hello"
 * 
 * USAGE:
 *   BleKeyboard keyboard(deviceName);
 *   keyboard.begin();
 *   keystrokes_handler_init(&keyboard);
 *   keystrokes_handler_execute("Ctrl+C");
 */

#ifndef KEYSTROKES_HANDLER_H
#define KEYSTROKES_HANDLER_H

#include "BleKeyboard.h"

// Initialize the keystrokes handler with a BLE keyboard instance
// Must be called before keystrokes_handler_execute()
bool keystrokes_handler_init(BleKeyboard* keyboard);

// Execute a keystrokes string
// Returns true if successful, false if BLE not connected or parsing error
// Format: comma-separated actions (e.g., "Ctrl+C,{Enter},hello")
bool keystrokes_handler_execute(const char* keystrokes);

#endif // KEYSTROKES_HANDLER_H
