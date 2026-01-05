#include "ble_keyboard_manager.h"

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#if BLE_KEYBOARD_MANAGER_ENABLED
#include <esp_bt.h>
#endif

#include <string>

void BleKeyboardManager::begin(const DeviceConfig* config) {
#if BLE_KEYBOARD_MANAGER_ENABLED
    if (keyboard) {
        return;
    }

    // We only use BLE HID. Release Classic BT controller memory to reclaim internal RAM.
    // Must be called before the controller is initialized by the BLE stack.
    static bool classicBtReleased = false;
    if (!classicBtReleased) {
        const esp_err_t err = esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT);
        Logger.logMessagef("BLE", "Classic BT mem release: %s (%d)", (err == ESP_OK) ? "OK" : "ERR", (int)err);
        classicBtReleased = true;
    }

    const char* name = (config && strlen(config->device_name) > 0) ? config->device_name : "ESP32-Keyboard";

    keyboard = new BleKeyboard(std::string(name), "Espressif", 100);

    keyboard->onConnect([]() {
        Logger.logMessage("BLE", "Keyboard connected");
    });
    keyboard->onDisconnect([]() {
        Logger.logMessage("BLE", "Keyboard disconnected");
    });

    Logger.logMessagef("BLE", "Starting BLE keyboard: %s", name);
    keyboard->begin();
#else
    (void)config;

    // Make it obvious in logs when BLE HID is not compiled in.
    // This is the most common reason the device doesn't appear in the Bluetooth scan list.
    static bool loggedDisabled = false;
    if (!loggedDisabled) {
        #if HAS_BLE_KEYBOARD
        const int hasBleKeyboard = 1;
        #else
        const int hasBleKeyboard = 0;
        #endif

        #if defined(CONFIG_BT_ENABLED)
        const int btEnabled = 1;
        #else
        const int btEnabled = 0;
        #endif

        #if defined(CONFIG_BT_NIMBLE_ROLE_PERIPHERAL)
        const int nimblePeripheral = 1;
        #else
        const int nimblePeripheral = 0;
        #endif

        Logger.logMessagef(
            "BLE",
            "BLE keyboard disabled at build (HAS_BLE_KEYBOARD=%d, CONFIG_BT_ENABLED=%d, CONFIG_BT_NIMBLE_ROLE_PERIPHERAL=%d)",
            hasBleKeyboard,
            btEnabled,
            nimblePeripheral);
        loggedDisabled = true;
    }
#endif
}

void BleKeyboardManager::end() {
#if BLE_KEYBOARD_MANAGER_ENABLED
    if (!keyboard) return;
    keyboard->end();
#endif
}

bool BleKeyboardManager::enabled() const {
#if BLE_KEYBOARD_MANAGER_ENABLED
    return true;
#else
    return false;
#endif
}

bool BleKeyboardManager::isConnected() const {
#if BLE_KEYBOARD_MANAGER_ENABLED
    return keyboard && keyboard->isConnected();
#else
    return false;
#endif
}

void BleKeyboardManager::sendText(const char* text) {
#if BLE_KEYBOARD_MANAGER_ENABLED
    if (!keyboard || !text) return;
    if (!keyboard->isConnected()) return;

    keyboard->print(text);
#else
    (void)text;
#endif
}

void BleKeyboardManager::press(uint8_t key) {
#if BLE_KEYBOARD_MANAGER_ENABLED
    if (!keyboard) return;
    if (!keyboard->isConnected()) return;
    keyboard->press(key);
#else
    (void)key;
#endif
}

void BleKeyboardManager::release(uint8_t key) {
#if BLE_KEYBOARD_MANAGER_ENABLED
    if (!keyboard) return;
    if (!keyboard->isConnected()) return;
    keyboard->release(key);
#else
    (void)key;
#endif
}

void BleKeyboardManager::tap(uint8_t key) {
#if BLE_KEYBOARD_MANAGER_ENABLED
    if (!keyboard) return;
    if (!keyboard->isConnected()) return;

    keyboard->press(key);
    vTaskDelay(3);
    keyboard->release(key);
#else
    (void)key;
#endif
}

void BleKeyboardManager::releaseAll() {
#if BLE_KEYBOARD_MANAGER_ENABLED
    if (!keyboard) return;
    keyboard->releaseAll();
#endif
}

void BleKeyboardManager::tapMedia(const MediaKeyReport key) {
#if BLE_KEYBOARD_MANAGER_ENABLED
    if (!keyboard) return;
    if (!keyboard->isConnected()) return;

    keyboard->press(key);
    vTaskDelay(3);
    keyboard->release(key);
#else
    (void)key;
#endif
}
