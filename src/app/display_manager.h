#ifndef DISPLAY_MANAGER_H
#define DISPLAY_MANAGER_H

#include "board_config.h"
#include "config_manager.h"
#include "display_driver.h"
#include "screens/screen.h"
#include "screens/splash_screen.h"
#include "screens/info_screen.h"
#include "screens/watchlist_screen.h"
#include "screens/test_screen.h"
#include "screens/macropad_screen.h"
#include "screens/error_screen.h"
#include "macros_config.h"

#if HAS_IMAGE_API
#include "screens/direct_image_screen.h"
#include "screens/lvgl_image_screen.h"
#endif

#include <lvgl.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>

// ============================================================================
// Screen Registry
// ============================================================================
// Maximum number of screens that can be registered for runtime navigation
// Generous headroom (8 slots) allows adding new screens without recompiling
// Only ~192 bytes total (24 bytes × 8), negligible overhead vs heap allocation
#define MAX_SCREENS 16

struct MacroConfig;
class BleKeyboardManager;
class MqttManager;

// Struct for registering available screens dynamically
struct ScreenInfo {
    const char* id;            // Unique identifier (e.g., "info", "test")
    const char* display_name;  // Human-readable name (e.g., "Info Screen")
    Screen* instance;          // Pointer to screen instance
};

// ============================================================================
// Display Manager
// ============================================================================
// Manages display hardware, LVGL, screen lifecycle, and navigation.
// Uses FreeRTOS task for continuous LVGL rendering (works on single and dual core).
//
// Usage:
//   display_manager_init(&device_config);  // In setup() - starts rendering task
//   display_manager_show_main();           // When WiFi connected
//   display_manager_set_splash_status();   // Update splash text
//
// Note: No need to call update() in loop() - rendering task handles it

class DisplayManager {
private:
    // Hardware (display driver abstraction)
    DisplayDriver* driver;
    lv_disp_draw_buf_t draw_buf;
    lv_color_t* buf;  // Dynamically allocated LVGL buffer
    lv_disp_drv_t disp_drv;
    
    // Configuration reference
    DeviceConfig* config;
    
    // FreeRTOS task and mutex
    TaskHandle_t lvglTaskHandle;
    SemaphoreHandle_t lvglMutex;
    
    // Screen management
    Screen* currentScreen;
    Screen* previousScreen;  // Track previous screen for return navigation
    Screen* pendingScreen;   // Deferred screen switch (processed in lvglTask)

    // Helpers: avoid taking the LVGL mutex when already inside the LVGL task
    bool isInLvglTask() const;
    void lockIfNeeded(bool& didLock);
    void unlockIfNeeded(bool didLock);
    
    // Screen instances (created at init, kept in memory)
    SplashScreen splashScreen;
    InfoScreen infoScreen;
    WatchlistScreen watchlistScreen;
    TestScreen testScreen;
    ErrorScreen errorScreen;

    // Error state (copied in; owned by DisplayManager)
    char errorTitle[48];
    char errorMessage[192];

    // When called off the LVGL task we try a bounded mutex wait. If that fails,
    // defer the splash status update and apply it from the LVGL task loop.
    char pendingSplashStatus[96];
    bool pendingSplashStatusPending;

    // Fixed macro pad screens (created lazily on first show)
    MacroPadScreen macroScreens[MACROS_SCREEN_COUNT];

    // Persistent storage for screen registry strings.
    // "macro" + up to 2 digits + NUL
    char macroScreenIds[MACROS_SCREEN_COUNT][8];
    // "Macro " + up to 2 digits + NUL
    char macroScreenNames[MACROS_SCREEN_COUNT][16];
    
    #if HAS_IMAGE_API
    DirectImageScreen directImageScreen;
    #if LV_USE_IMG
    LvglImageScreen lvglImageScreen;
    #endif
    #endif
    
    // Screen registry for runtime navigation (static allocation, no heap)
    // screenCount tracks how many slots are actually used (currently 2: info, test)
    // Splash excluded from runtime selection (boot-specific only)
    ScreenInfo availableScreens[MAX_SCREENS];
    size_t screenCount;

    // Internal helper: map a Screen instance to its logical screen id.
    // Uses the registered screen list so adding new screens doesn't require
    // updating logging code.
    const char* getScreenIdForInstance(const Screen* screen) const;

    // Macro runtime pointers (owned elsewhere; set by app.ino after init)
    MacroConfig* macroConfig;
    BleKeyboardManager* bleKeyboard;
    MqttManager* mqttManager;
    
    // Hardware initialization
    void initHardware();
    void initLVGL();
    
    // LVGL flush callback (static, accesses instance via user_data)
    static void flushCallback(lv_disp_drv_t *disp, const lv_area_t *area, lv_color_t *color_p);

    // Buffered render-mode drivers (e.g., Arduino_GFX canvas) need an explicit
    // present() step, but only after LVGL has actually rendered something.
    bool flushPending;

    // When true, LVGL flushes must not touch the panel.
    // This is enabled as soon as DirectImageScreen is requested so that
    // the JPEG decoder can safely write to the display without SPI contention.
    volatile bool directImageActive;
    
    // FreeRTOS task for LVGL rendering
    static void lvglTask(void* pvParameter);
    
public:
    DisplayManager(DeviceConfig* config);
    ~DisplayManager();

    // Access to the active device configuration (owned by app.ino).
    const DeviceConfig* getConfig() const { return config; }
    
    // Initialize hardware + LVGL + screens + rendering task (shows splash automatically)
    void init();
    
    // Navigation API (thread-safe)
    void showSplash();
    void showInfo();
    void showTest();

    void setMacroRuntime(MacroConfig* cfg, BleKeyboardManager* keyboard);
    const MacroConfig* getMacroConfig() const { return macroConfig; }
    BleKeyboardManager* getBleKeyboard() const { return bleKeyboard; }

    // Optional runtime pointer for macro actions.
    void setMqttManager(MqttManager* mqtt) { mqttManager = mqtt; }
    MqttManager* getMqttManager() const { return mqttManager; }

    // Show a generic error screen (thread-safe).
    void showError(const char* title, const char* message);
    
    #if HAS_IMAGE_API
    void showDirectImage();
    void returnToPreviousScreen();  // Return to screen before image was shown
    #endif
    
    // Screen selection by ID (thread-safe, returns true if found)
    bool showScreen(const char* screen_id);

    // Return to previous runtime screen when available; otherwise go to default ("macro1").
    // Returns true when a navigation was queued.
    bool goBackOrDefault();
    
    // Get current screen ID (returns nullptr if splash or no screen)
    const char* getCurrentScreenId();
    
    // Get available screens for runtime navigation
    const ScreenInfo* getAvailableScreens(size_t* count);
    
    // Splash status update (thread-safe)
    void setSplashStatus(const char* text);
    
    // Mutex helpers for external thread-safe access
    void lock();
    void unlock();

    // Attempt to lock the LVGL mutex with a timeout (in milliseconds).
    // Returns true if the lock was acquired.
    bool tryLock(uint32_t timeoutMs);

    // Active LVGL logical resolution (post driver->configureLVGL()).
    // Prefer using these instead of calling LVGL APIs from non-LVGL tasks.
    int getActiveWidth() const { return (int)disp_drv.hor_res; }
    int getActiveHeight() const { return (int)disp_drv.ver_res; }
    
    // Access to splash screen for status updates
    SplashScreen* getSplash() { return &splashScreen; }
    
    #if HAS_IMAGE_API
    // Access to direct image screen for image API
    DirectImageScreen* getDirectImageScreen() { return &directImageScreen; }

    // Access to LVGL image screen (shows images via lv_img)
    #if LV_USE_IMG
    LvglImageScreen* getLvglImageScreen() { return &lvglImageScreen; }
    #endif
    #endif
    
    // Access to display driver (for touch integration)
    DisplayDriver* getDriver() { return driver; }
};

// Global instance (managed by app.ino)
extern DisplayManager* displayManager;

// C-style interface for app.ino
void display_manager_init(DeviceConfig* config);
void display_manager_show_splash();
void display_manager_show_info();
void display_manager_show_test();
void display_manager_show_screen(const char* screen_id, bool* success);  // success is optional output
const char* display_manager_get_current_screen_id();
const ScreenInfo* display_manager_get_available_screens(size_t* count);
void display_manager_set_splash_status(const char* text);
void display_manager_set_backlight_brightness(uint8_t brightness);  // 0-100%

// Provide macro runtime pointers used by MacroPadScreen.
// Safe to call multiple times.
void display_manager_set_macro_runtime(MacroConfig* cfg, BleKeyboardManager* keyboard);

// Show a generic error screen (title/message copied).
void display_manager_show_error(const char* title, const char* message);

// Provide MQTT manager pointer for runtime features like macro actions.
// Safe to call with nullptr.
void display_manager_set_mqtt_manager(MqttManager* mqtt);

// Serialization helpers for code running outside the LVGL task.
// Use these to avoid concurrent access to buffered display backends (e.g., Arduino_GFX canvas).
void display_manager_lock();
void display_manager_unlock();
bool display_manager_try_lock(uint32_t timeout_ms);

#if HAS_IMAGE_API
// C-style interface for image API
void display_manager_show_direct_image();
DirectImageScreen* display_manager_get_direct_image_screen();
#if LV_USE_IMG
LvglImageScreen* display_manager_get_lvgl_image_screen();
#endif
void display_manager_return_to_previous_screen();
#endif

#endif // DISPLAY_MANAGER_H
