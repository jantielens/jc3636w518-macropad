#ifndef BLE_KEYBOARD_MANAGER_H
#define BLE_KEYBOARD_MANAGER_H

#include <Arduino.h>

#include "board_config.h"
#include "config_manager.h"
#include "log_manager.h"

#include "sdkconfig.h"

#if HAS_BLE_KEYBOARD && defined(CONFIG_BT_ENABLED) && defined(CONFIG_BT_NIMBLE_ROLE_PERIPHERAL)
#define BLE_KEYBOARD_MANAGER_ENABLED 1
#include "BleKeyboard.h"
#else
#define BLE_KEYBOARD_MANAGER_ENABLED 0
#endif

class BleKeyboardManager {
public:
    void begin(const DeviceConfig* config);
    void end();

    bool enabled() const;
    bool isConnected() const;

    // For MVP: just enough for DuckyScript STRING + basic key taps.
    // More complex chords/media keys can be added in the DuckyScript executor step.
    void sendText(const char* text);

    void press(uint8_t key);
    void release(uint8_t key);
    void tap(uint8_t key);
    void releaseAll();

    // Media keys (no-op when BLE keyboard is not enabled).
    void tapMedia(const MediaKeyReport key);

private:
#if BLE_KEYBOARD_MANAGER_ENABLED
    BleKeyboard* keyboard = nullptr;
#endif
};

#endif // BLE_KEYBOARD_MANAGER_H
