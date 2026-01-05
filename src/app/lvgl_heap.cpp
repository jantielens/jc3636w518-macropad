#include "lvgl_heap.h"

#include <esp_heap_caps.h>
#include <esp_rom_sys.h>
#include <esp_system.h>

// Arduino-ESP32 helper for detecting PSRAM at runtime.
// (psramFound() is not declared in esp_system.h)
#include <esp32-hal-psram.h>
#include <soc/soc_caps.h>

extern "C" void* lvgl_heap_malloc(size_t size) {
    if (size == 0) return nullptr;

    static bool logged_psram_ok = false;
    static bool logged_psram_fail = false;

#if defined(SOC_SPIRAM_SUPPORTED) && SOC_SPIRAM_SUPPORTED
    // Prefer PSRAM to keep internal heap healthy (LVGL does lots of small allocs).
    if (psramFound()) {
        // Note: requesting MALLOC_CAP_8BIT along with SPIRAM can be unnecessarily
        // restrictive on some cores; SPIRAM is already byte-addressable.
        void* p = heap_caps_malloc(size, MALLOC_CAP_SPIRAM);
        if (p) {
            if (!logged_psram_ok) {
                esp_rom_printf("[LVGL] heap: PSRAM alloc OK (first) size=%u\n", (unsigned)size);
                logged_psram_ok = true;
            }
            return p;
        }
        if (!logged_psram_fail) {
            esp_rom_printf("[LVGL] heap: PSRAM alloc FAIL (first) size=%u\n", (unsigned)size);
            logged_psram_fail = true;
        }
    }
#endif

    // Fallback: internal RAM.
    return heap_caps_malloc(size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}

extern "C" void* lvgl_heap_realloc(void* ptr, size_t size) {
    if (ptr == nullptr) return lvgl_heap_malloc(size);
    if (size == 0) {
        lvgl_heap_free(ptr);
        return nullptr;
    }

#if defined(SOC_SPIRAM_SUPPORTED) && SOC_SPIRAM_SUPPORTED
    // Prefer PSRAM to keep internal heap healthy.
    if (psramFound()) {
        void* p = heap_caps_realloc(ptr, size, MALLOC_CAP_SPIRAM);
        if (p) return p;
    }
#endif

    // Fallback: internal RAM.
    return heap_caps_realloc(ptr, size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}

extern "C" void lvgl_heap_free(void* ptr) {
    if (!ptr) return;
    heap_caps_free(ptr);
}
