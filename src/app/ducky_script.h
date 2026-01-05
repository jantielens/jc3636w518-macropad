#ifndef DUCKY_SCRIPT_H
#define DUCKY_SCRIPT_H

#include <Arduino.h>

class BleKeyboardManager;

// Executes a small DuckyScript-inspired subset.
// - Commands are case-insensitive.
// - Unknown tokens are ignored with a log warning.
// - Safe no-op if keyboard is null or not connected.
//
// Supported:
//   STRING <text>
//   DELAY <ms>
//   A..Z, 0..9
//   ENTER, TAB, ESCAPE, BACKSPACE, SPACE
//   UPARROW, DOWNARROW, LEFTARROW, RIGHTARROW
//   HOME, END, PAGEUP, PAGEDOWN
//   F1..F12
//   CTRL/SHIFT/ALT/GUI modifiers before a key token (multiple allowed)
//   VOLUMEUP, VOLUMEDOWN, MUTE, PLAYPAUSE, NEXTTRACK, PREVTRACK
bool ducky_execute(const char* script, BleKeyboardManager* keyboard);

#endif // DUCKY_SCRIPT_H
