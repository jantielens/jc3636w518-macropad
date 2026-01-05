# RAM Optimization Options for jc3636w518-macropad

## Executive Summary

This document provides a comprehensive analysis of RAM optimization opportunities for the ESP32-S3 based macropad device. The device has:
- **16MB Flash** (plenty of space for firmware - currently using ~3MB)
- **8MB PSRAM** (OPI PSRAM available)
- **Limited Internal RAM** (~512KB total, but system services consume a significant portion)

**Key Finding**: The project already uses PSRAM well for LVGL allocations and large buffers. Most optimization opportunities involve moving more data structures from internal RAM to PSRAM or Flash, reducing stack sizes, and making features optional.

---

## Memory Architecture Overview

### Current Memory Usage Patterns

The codebase already implements smart PSRAM usage:
- **LVGL heap**: Custom allocator preferring PSRAM (`lvgl_heap.cpp`)
- **LVGL draw buffer**: 360×16 pixels (11,520 bytes) in PSRAM when available
- **Image API**: Large buffers (up to 300KB) in PSRAM for JPEG processing
- **Web portal**: AsyncTCP task stack at 10,240 bytes (board override)

### Internal RAM Consumers
1. **FreeRTOS stacks** - LVGL task, AsyncTCP task, Arduino loop task
2. **Global variables** - Config structs, screen objects, manager instances
3. **BLE/NimBLE stack** - Bluetooth HID keyboard (~40-60KB)
4. **Web assets** - Compressed HTML/CSS/JS embedded in firmware
5. **String literals** - Log messages, HTML templates, JSON keys
6. **Static buffers** - Line buffers for image decoding, log buffers

---

## Optimization Options

### Category 1: Move Large Data to PSRAM

#### Option 1.1: BLE Keyboard Stack in PSRAM
**Description**: Configure NimBLE to use PSRAM for its internal buffers instead of internal RAM.

**Implementation**:
```cpp
// In BleKeyboard.cpp or ble_keyboard_manager.cpp
#include <esp_nimble_hci.h>

// Configure NimBLE memory allocation
void ble_keyboard_init() {
    // Set NimBLE to use external RAM
    esp_nimble_hci_set_mem_alloc(MALLOC_CAP_SPIRAM);
    // ... rest of init
}
```

**Files to modify**:
- `src/app/ble_keyboard_manager.cpp`
- Potentially `src/app/BleKeyboard.cpp`

**Estimated savings**: 40-60 KB internal RAM
**Complexity**: Medium (requires NimBLE configuration changes)
**Risk**: Low-Medium (well-documented ESP32 feature, but may affect BLE performance slightly)
**Testing**: Verify BLE keyboard still works reliably, check for increased latency

---

#### Option 1.2: Web Assets in PSRAM
**Description**: Store compressed web assets (HTML/CSS/JS) in PSRAM instead of internal RAM.

**Implementation**:
```cpp
// In tools/minify-web-assets.sh, modify generated code to use PSRAM attribute
// Before:
const uint8_t WEB_CSS[] PROGMEM = {...};

// After:
const uint8_t WEB_CSS[] __attribute__((section(".psram"))) = {...};

// Or use explicit PSRAM allocation:
static uint8_t* web_css = nullptr;
void web_assets_init() {
    web_css = (uint8_t*)heap_caps_malloc(sizeof(WEB_CSS_DATA), MALLOC_CAP_SPIRAM);
    memcpy(web_css, WEB_CSS_DATA, sizeof(WEB_CSS_DATA));
}
```

**Files to modify**:
- `tools/minify-web-assets.sh` (generator script)
- `src/app/web_assets.h` (generated file)
- `src/app/web_portal.cpp` (asset serving)

**Estimated savings**: 10-20 KB internal RAM (depends on asset size)
**Complexity**: Medium (requires modifying code generation)
**Risk**: Low (assets are read-only and accessed infrequently)
**Testing**: Verify all web pages load correctly

---

#### Option 1.3: Log Buffer in PSRAM
**Description**: Move the serial log buffer (if any exists for web streaming) to PSRAM.

**Implementation**:
```cpp
// In log_manager.cpp
class LogManager {
private:
    static char* log_buffer;  // Instead of static char log_buffer[SIZE]
    
public:
    void init() {
        log_buffer = (char*)heap_caps_malloc(LOG_BUFFER_SIZE, MALLOC_CAP_SPIRAM);
    }
};
```

**Files to modify**:
- `src/app/log_manager.h`
- `src/app/log_manager.cpp`

**Estimated savings**: 0-4 KB (only if circular buffer exists - current code appears to be direct serial output)
**Complexity**: Low
**Risk**: Very Low
**Testing**: Verify logging still works

---

### Category 2: Move Static/Const Data to Flash

#### Option 2.1: String Literals to PROGMEM
**Description**: Mark all constant strings with `PROGMEM` attribute to keep them in Flash instead of copying to RAM.

**Implementation**:
```cpp
// Before:
const char* error_msg = "Configuration failed";
Logger.logLine(error_msg);

// After:
const char error_msg[] PROGMEM = "Configuration failed";
Logger.logLine(PSTR("Configuration failed"));  // or use F() macro

// Or create helper:
void logLineP(const char* str_P) {
    char buf[128];
    strncpy_P(buf, str_P, sizeof(buf));
    logLine(buf);
}
```

**Files to modify**:
- All `.cpp` files with string constants
- `src/app/log_manager.cpp/h` (add PROGMEM-aware functions)
- `src/app/web_portal.cpp`
- `src/app/mqtt_manager.cpp`
- `src/app/ha_discovery.cpp`

**Estimated savings**: 5-15 KB internal RAM
**Complexity**: High (requires modifying many files, easy to introduce bugs)
**Risk**: Medium (can cause crashes if strings are accessed incorrectly)
**Testing**: Extensive testing of all log messages, error strings, JSON keys

---

#### Option 2.2: Lookup Tables to PROGMEM
**Description**: Move any lookup tables or constant arrays to Flash.

**Implementation**:
```cpp
// Before:
const uint8_t gamma_table[256] = {0, 1, 2, ...};

// After:
const uint8_t gamma_table[256] PROGMEM = {0, 1, 2, ...};
uint8_t val = pgm_read_byte(&gamma_table[index]);
```

**Files to modify**:
- Check `src/app/ducky_script.cpp` for keystroke tables
- Check `src/app/BleKeyboard.cpp` for HID descriptor tables (already appears to be const)
- Any other files with large const arrays

**Estimated savings**: 1-5 KB internal RAM
**Complexity**: Medium
**Risk**: Low-Medium
**Testing**: Verify affected functionality works correctly

---

### Category 3: Reduce Stack Sizes

#### Option 3.1: LVGL Task Stack Reduction
**Description**: The LVGL rendering task stack might be oversized.

**Implementation**:
```cpp
// In display_manager.cpp
// Before:
#define LVGL_TASK_STACK_SIZE (8 * 1024)  // 8KB

// After:
#define LVGL_TASK_STACK_SIZE (6 * 1024)  // 6KB
// Or even 4KB if testing shows it's sufficient

xTaskCreatePinnedToCore(
    lvglTask,
    "LVGL",
    LVGL_TASK_STACK_SIZE,  // Reduced stack
    // ...
);
```

**Files to modify**:
- `src/app/display_manager.cpp`

**Estimated savings**: 2-4 KB internal RAM per KB reduced
**Complexity**: Low
**Risk**: Medium-High (stack overflow can cause crashes or corruption)
**Testing**: Monitor stack watermark with `uxTaskGetStackHighWaterMark()`, test all UI screens

---

#### Option 3.2: AsyncTCP Task Stack Reduction
**Description**: The AsyncTCP task stack is currently 10,240 bytes (board override). This might be reducible.

**Implementation**:
```cpp
// In src/boards/jc3636w518/board_overrides.h
// Before:
#define CONFIG_ASYNC_TCP_STACK_SIZE 10240

// After:
#define CONFIG_ASYNC_TCP_STACK_SIZE 8192  // Try 8KB first
```

**Files to modify**:
- `src/boards/jc3636w518/board_overrides.h`

**Estimated savings**: 2 KB internal RAM
**Complexity**: Very Low
**Risk**: Medium (may cause crashes during web uploads or API calls)
**Testing**: Test OTA updates, large JSON POST requests, concurrent connections

---

### Category 4: Lazy Initialization / On-Demand Loading

#### Option 4.1: Lazy Screen Initialization
**Description**: Don't create all screen objects at boot. Initialize them when first accessed.

**Implementation**:
```cpp
// In display_manager.cpp
class DisplayManager {
private:
    Screen* screens[MAX_SCREENS];  // Pointers instead of objects
    bool screens_initialized[MAX_SCREENS];
    
public:
    void switchScreen(const char* id) {
        int idx = findScreenIndex(id);
        if (!screens_initialized[idx]) {
            screens[idx] = createScreen(id);  // Create on demand
            screens_initialized[idx] = true;
        }
        // ... rest of switch logic
    }
};
```

**Files to modify**:
- `src/app/display_manager.cpp/h`
- `src/app/screens/*.cpp`

**Estimated savings**: 5-15 KB internal RAM (screen objects are created only when needed)
**Complexity**: High (requires significant refactoring)
**Risk**: Medium (initialization errors might occur at runtime instead of boot)
**Testing**: Test switching to every screen, verify all functionality

---

#### Option 4.2: Lazy MQTT Connection
**Description**: Don't connect to MQTT at boot. Connect only when MQTT settings are configured and on-demand.

**Implementation**:
```cpp
// In mqtt_manager.cpp
void mqtt_manager_connect() {
    if (!config->mqtt_host[0]) return;  // Skip if not configured
    if (mqtt_connected) return;  // Already connected
    
    // Connect now
    mqtt_client.begin(config->mqtt_host, config->mqtt_port, net);
    mqtt_client.connect(/*...*/);
}

// Connect on first publish attempt instead of at boot
```

**Files to modify**:
- `src/app/mqtt_manager.cpp/h`
- `src/app/app.ino` (remove auto-connect from setup)

**Estimated savings**: 0 KB (connection is on-demand, but client object still exists)
**Complexity**: Low
**Risk**: Low
**Testing**: Verify MQTT connects when needed, publishes work correctly

---

### Category 5: Conditional Compilation / Feature Removal

#### Option 5.1: Make BLE Keyboard Optional
**Description**: Add a compile-time flag to disable BLE keyboard if not needed.

**Implementation**:
```cpp
// In board_config.h - already exists!
#ifndef HAS_BLE_KEYBOARD
#define HAS_BLE_KEYBOARD false  // Default to disabled
#endif

// Usage is already conditional in app.ino:
#if HAS_BLE_KEYBOARD
    ble_keyboard.begin();
#endif
```

**Files to modify**:
- None (already implemented! Just set to false in board_overrides.h)

**Estimated savings**: 40-80 KB internal RAM + ~200 KB Flash
**Complexity**: Very Low (just change a config flag)
**Risk**: Very Low (feature is already conditionally compiled)
**Testing**: Verify firmware still builds and runs without BLE

---

#### Option 5.2: Disable Unused LVGL Features
**Description**: Review `lv_conf.h` and disable any unused LVGL widgets or features.

**Implementation**:
```cpp
// In src/app/lv_conf.h
// Review and disable unused widgets:
#define LV_USE_ARC 0          // Circular arc widget
#define LV_USE_BAR 0          // Progress bar
#define LV_USE_SLIDER 0       // Slider widget
#define LV_USE_ROLLER 0       // Roller selector
#define LV_USE_TEXTAREA 0     // Text area input
#define LV_USE_TABLE 0        // Table widget
#define LV_USE_CALENDAR 0     // Calendar widget
#define LV_USE_CHART 0        // Chart widget
#define LV_USE_COLORWHEEL 0   // Color wheel
#define LV_USE_IMGBTN 0       // Image button
#define LV_USE_KEYBOARD 0     // On-screen keyboard
#define LV_USE_LED 0          // LED indicator
#define LV_USE_LIST 0         // List widget
#define LV_USE_MENU 0         // Menu widget
#define LV_USE_METER 0        // Meter widget
#define LV_USE_MSGBOX 0       // Message box
#define LV_USE_SPAN 0         // Span (rich text)
#define LV_USE_SPINBOX 0      // Spin box
#define LV_USE_SPINNER 0      // Spinner (unless used!)
#define LV_USE_TABVIEW 0      // Tab view
#define LV_USE_TILEVIEW 0     // Tile view
#define LV_USE_WIN 0          // Window widget
```

**Files to modify**:
- `src/app/lv_conf.h`

**Estimated savings**: 5-30 KB Flash (minimal RAM impact, mostly code)
**Complexity**: Low (need to identify which features are actually used)
**Risk**: Low-Medium (disabling used features will cause compile errors)
**Testing**: Full compile, test all screens and UI interactions

---

#### Option 5.3: Reduce LVGL Font Selection
**Description**: Disable unused Montserrat font sizes to save Flash.

**Implementation**:
```cpp
// In src/app/lv_conf.h
// Current (3 fonts enabled):
#define LV_FONT_MONTSERRAT_14 1
#define LV_FONT_MONTSERRAT_18 1
#define LV_FONT_MONTSERRAT_24 1

// Review actual usage in screens/*.cpp and disable unused sizes
// Each font is ~2-5 KB in Flash
```

**Files to modify**:
- `src/app/lv_conf.h`
- Review `src/app/screens/*.cpp` to see which fonts are used

**Estimated savings**: 2-10 KB Flash per font (minimal RAM impact)
**Complexity**: Very Low
**Risk**: Low (compile error if font is used)
**Testing**: Verify all text renders correctly

---

#### Option 5.4: Disable Image API When Not Needed
**Description**: The Image API (JPEG upload/display) uses significant resources.

**Implementation**:
```cpp
// In board_overrides.h
#define HAS_IMAGE_API false  // Disable if not using image display
```

**Files to modify**:
- `src/boards/jc3636w518/board_overrides.h`

**Estimated savings**: 10-20 KB internal RAM + ~50 KB Flash
**Complexity**: Very Low
**Risk**: Very Low (feature is already conditionally compiled)
**Testing**: Verify firmware builds without image API

---

### Category 6: Optimize Data Structures

#### Option 6.1: Pack DeviceConfig Structure
**Description**: Add `__attribute__((packed))` to reduce struct padding.

**Implementation**:
```cpp
// In config_manager.h
struct __attribute__((packed)) DeviceConfig {
    char wifi_ssid[32];
    char wifi_password[64];
    // ... rest of fields
};
```

**Files to modify**:
- `src/app/config_manager.h`

**Estimated savings**: 1-4 KB internal RAM (depends on alignment)
**Complexity**: Very Low
**Risk**: Very Low (only affects storage, not functionality)
**Testing**: Verify config save/load still works, check NVS compatibility

---

#### Option 6.2: Reduce String Buffer Sizes
**Description**: Review and reduce oversized string buffers in config structs.

**Implementation**:
```cpp
// In config_manager.h
// Before:
#define CONFIG_MQTT_HOST_MAX_LEN 64
#define CONFIG_MQTT_USERNAME_MAX_LEN 32
#define CONFIG_MQTT_PASSWORD_MAX_LEN 64

// After (if appropriate for use case):
#define CONFIG_MQTT_HOST_MAX_LEN 48  // Still plenty for hostnames
#define CONFIG_MQTT_USERNAME_MAX_LEN 24
#define CONFIG_MQTT_PASSWORD_MAX_LEN 48
```

**Files to modify**:
- `src/app/config_manager.h`

**Estimated savings**: 1-3 KB internal RAM
**Complexity**: Low
**Risk**: Low-Medium (may truncate long values)
**Testing**: Test with long SSID/password/hostname values

---

### Category 7: PSRAM-Specific Optimizations

#### Option 7.1: Use PSRAM for Arduino String Objects
**Description**: Configure Arduino core to allocate String objects in PSRAM by default.

**Implementation**:
```cpp
// In platformio.ini or arduino-cli build flags:
build_flags = 
    -DARDUINO_STRING_USE_PSRAM

// Or manually in code:
// Replace String usage with explicit PSRAM allocation
char* str = (char*)ps_malloc(size);
```

**Files to modify**:
- `config.sh` (add build flag)
- Or replace String usage with manual PSRAM allocation

**Estimated savings**: 2-10 KB internal RAM (depends on String usage)
**Complexity**: Low (if using build flag) to High (if manual replacement)
**Risk**: Low (build flag) to Medium (manual replacement)
**Testing**: Test all String operations, JSON parsing, WiFi operations

---

#### Option 7.2: JPEG Work Buffers in PSRAM
**Description**: Already mostly implemented, but ensure all temporary buffers use PSRAM.

**Implementation**:
```cpp
// Already done in strip_decoder.cpp and lvgl_jpeg_decoder.cpp
// Verify all temporary buffers prefer PSRAM:
work_buffer = heap_caps_malloc(size, MALLOC_CAP_SPIRAM);
if (!work_buffer) {
    work_buffer = heap_caps_malloc(size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}
```

**Files to modify**:
- `src/app/strip_decoder.cpp` (already done)
- `src/app/lvgl_jpeg_decoder.cpp` (already done)
- `src/app/image_api.cpp` (already done)

**Estimated savings**: 0 KB (already implemented!)
**Complexity**: N/A
**Risk**: N/A
**Testing**: N/A

---

### Category 8: Reduce Runtime Allocations

#### Option 8.1: Pre-allocate Image Buffers
**Description**: Instead of malloc/free during image operations, pre-allocate and reuse buffers.

**Implementation**:
```cpp
// In image_api.cpp
static uint8_t* persistent_image_buffer = nullptr;
static size_t persistent_buffer_size = 0;

void image_api_init() {
    // Pre-allocate maximum size in PSRAM
    persistent_buffer_size = IMAGE_API_MAX_SIZE_BYTES;
    persistent_image_buffer = heap_caps_malloc(
        persistent_buffer_size, 
        MALLOC_CAP_SPIRAM
    );
}

// Reuse instead of malloc/free per upload
```

**Files to modify**:
- `src/app/image_api.cpp`

**Estimated savings**: 0 KB RAM, but reduces heap fragmentation
**Complexity**: Medium
**Risk**: Low-Medium (increases base memory usage)
**Testing**: Test multiple image uploads, OTA updates, verify no memory leaks

---

#### Option 8.2: Use FreeRTOS Static Allocation
**Description**: Use static task/queue allocation instead of dynamic to reduce fragmentation.

**Implementation**:
```cpp
// In display_manager.cpp
static StackType_t lvgl_task_stack[LVGL_TASK_STACK_SIZE];
static StaticTask_t lvgl_task_tcb;

xTaskCreateStaticPinnedToCore(
    lvglTask,
    "LVGL",
    LVGL_TASK_STACK_SIZE,
    this,
    1,
    lvgl_task_stack,
    &lvgl_task_tcb,
    1
);
```

**Files to modify**:
- `src/app/display_manager.cpp`
- `src/app/web_portal.cpp` (if AsyncTCP allows)

**Estimated savings**: 0 KB (same RAM usage, but better fragmentation)
**Complexity**: Medium
**Risk**: Low
**Testing**: Verify tasks create successfully, monitor for issues

---

### Category 9: Aggressive Optimizations (Higher Risk)

#### Option 9.1: Disable WiFi Power Save
**Description**: WiFi power save can cause buffer allocations. Disable if power isn't a concern.

**Implementation**:
```cpp
// In app.ino
WiFi.setSleep(false);  // Disable WiFi sleep mode
```

**Files to modify**:
- `src/app/app.ino`

**Estimated savings**: 0-2 KB internal RAM (minor)
**Complexity**: Very Low
**Risk**: Low (increases power consumption)
**Testing**: Measure current draw, verify WiFi stability

---

#### Option 9.2: Reduce Display Buffer Size
**Description**: Use smaller LVGL draw buffer at cost of refresh rate.

**Implementation**:
```cpp
// In board_overrides.h
// Before:
#define LVGL_BUFFER_SIZE (DISPLAY_WIDTH * 16)  // 16 rows

// After:
#define LVGL_BUFFER_SIZE (DISPLAY_WIDTH * 8)   // 8 rows (half size)
```

**Files to modify**:
- `src/boards/jc3636w518/board_overrides.h`

**Estimated savings**: 5-6 KB PSRAM (11,520 → 5,760 bytes)
**Complexity**: Very Low
**Risk**: Low (slower display refresh, but PSRAM is plentiful)
**Testing**: Verify UI still renders smoothly

---

#### Option 9.3: Single Screen at a Time
**Description**: Destroy previous screen fully before creating new one.

**Implementation**:
```cpp
// In display_manager.cpp
void DisplayManager::switchScreen(Screen* new_screen) {
    if (currentScreen) {
        currentScreen->destroy();
        delete currentScreen;  // Free object
        currentScreen = nullptr;
    }
    currentScreen = new_screen;
    currentScreen->create();
}
```

**Files to modify**:
- `src/app/display_manager.cpp/h`

**Estimated savings**: 10-30 KB internal RAM (depends on screen count)
**Complexity**: High (requires careful lifecycle management)
**Risk**: High (may cause issues with screen state)
**Testing**: Extensive testing of screen switching, verify no crashes

---

## Summary Table

| Option | Category | Est. RAM Saved | Complexity | Risk | Priority |
|--------|----------|----------------|------------|------|----------|
| 1.1 BLE Stack → PSRAM | PSRAM | 40-60 KB | Medium | Low-Med | **HIGH** |
| 1.2 Web Assets → PSRAM | PSRAM | 10-20 KB | Medium | Low | **HIGH** |
| 5.1 Disable BLE (flag) | Conditional | 40-80 KB | Very Low | Very Low | **HIGH** |
| 3.2 AsyncTCP Stack↓ | Stack | 2 KB | Very Low | Medium | **MEDIUM** |
| 3.1 LVGL Stack↓ | Stack | 2-4 KB | Low | Med-High | **MEDIUM** |
| 5.4 Disable Image API | Conditional | 10-20 KB | Very Low | Very Low | **MEDIUM** |
| 6.1 Pack Structs | Optimize | 1-4 KB | Very Low | Very Low | **MEDIUM** |
| 1.3 Log Buffer → PSRAM | PSRAM | 0-4 KB | Low | Very Low | LOW |
| 2.1 Strings → PROGMEM | Flash | 5-15 KB | High | Medium | LOW |
| 2.2 Tables → PROGMEM | Flash | 1-5 KB | Medium | Low-Med | LOW |
| 4.1 Lazy Screens | On-demand | 5-15 KB | High | Medium | LOW |
| 5.2 Disable LVGL Features | Conditional | 0-5 KB* | Low | Low-Med | LOW |
| 6.2 Reduce Buffer Sizes | Optimize | 1-3 KB | Low | Low-Med | LOW |
| 7.1 String→PSRAM | PSRAM | 2-10 KB | Low-High | Low-Med | LOW |
| 8.1 Pre-alloc Buffers | Allocation | 0 KB† | Medium | Low-Med | LOW |
| 9.3 Single Screen | Aggressive | 10-30 KB | High | High | LOW |

\* Mainly Flash savings  
† Reduces fragmentation, not total RAM

---

## Recommended Implementation Order

### Phase 1: Low-Hanging Fruit (Quick Wins)
1. **Disable BLE if not needed** (`HAS_BLE_KEYBOARD false`) - 40-80 KB instantly
2. **Pack DeviceConfig struct** - 1-4 KB, zero risk
3. **Reduce AsyncTCP stack** to 8192 - 2 KB, test thoroughly

**Expected total: 43-86 KB internal RAM saved**

### Phase 2: Medium Effort, High Reward
4. **Move BLE stack to PSRAM** - 40-60 KB (if keeping BLE)
5. **Move web assets to PSRAM** - 10-20 KB
6. **Reduce LVGL task stack** to 6 KB - 2 KB (monitor watermark)

**Expected total: 52-82 KB internal RAM saved**

### Phase 3: Optimization & Polish (Optional)
7. **Disable unused LVGL features** - Saves Flash, minimal RAM
8. **String literals to PROGMEM** - 5-15 KB (high effort)
9. **Consider lazy initialization** - 5-15 KB (if needed)

**Expected total: 10-30 KB additional**

---

## Testing Recommendations

For each implemented option:

1. **Compile Test**: Verify firmware builds without errors
2. **Boot Test**: Device boots and initializes all subsystems
3. **Memory Test**: Log free heap at boot and during operation
4. **Stress Test**: 
   - Upload large images (if Image API enabled)
   - Multiple concurrent web connections
   - Extended BLE keyboard usage
   - Rapid screen switching
5. **Stability Test**: 24-hour runtime without crashes

**Memory Monitoring Code**:
```cpp
void log_memory_status() {
    Serial.printf("Free heap: %d bytes\n", ESP.getFreeHeap());
    Serial.printf("Min free heap: %d bytes\n", ESP.getMinFreeHeap());
    Serial.printf("Free PSRAM: %d bytes\n", ESP.getFreePsram());
    Serial.printf("Min free PSRAM: %d bytes\n", ESP.getMinFreePsram());
}
```

---

## Notes on PSRAM vs Internal RAM

**When to prefer PSRAM:**
- Large buffers (>4 KB)
- Infrequently accessed data
- Static/constant data that's rarely needed
- Image/video buffers
- Web assets
- BLE buffers

**When to prefer Internal RAM:**
- Small, frequently accessed structures
- Time-critical data (DMA, ISR handlers)
- Stack frames for tasks
- Very small buffers (<1 KB)

**The device has 8MB PSRAM** - use it liberally! Internal RAM should be reserved for performance-critical operations.

---

## Conclusion

The most impactful optimizations with lowest risk are:

1. **Disable BLE keyboard** if not needed (instant 40-80 KB)
2. **Move BLE stack to PSRAM** if keeping BLE (40-60 KB)
3. **Move web assets to PSRAM** (10-20 KB)
4. **Pack structures** (1-4 KB)
5. **Reduce stack sizes** carefully (4-6 KB total)

These five changes alone could recover **90-170 KB of internal RAM** with reasonable effort and risk.

With 3MB of Flash available and 8MB of PSRAM, the device is well-positioned to move more data out of internal RAM. Focus on PSRAM-based optimizations first, as they provide the best risk/reward ratio.
