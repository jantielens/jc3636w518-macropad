#include "device_telemetry.h"

#include "board_config.h"
#include "log_manager.h"

#include "fs_health.h"

#if HAS_MQTT
#include "mqtt_manager.h"
#endif

#if HAS_DISPLAY
#include "display_manager.h"
#endif

#include <Arduino.h>
#include <WiFi.h>
#include "soc/soc_caps.h"
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
#include <freertos/timers.h>

// Temperature sensor support (ESP32-C3, ESP32-S2, ESP32-S3, ESP32-C2, ESP32-C6, ESP32-H2)
#if SOC_TEMP_SENSOR_SUPPORTED
#include "driver/temperature_sensor.h"
#endif

// CPU usage tracking (task-based with min/max over 60s window)
static SemaphoreHandle_t cpu_mutex = nullptr;
static int cpu_usage_current = 0;
static int cpu_usage_min = 100;
static int cpu_usage_max = 0;
static unsigned long last_minmax_reset = 0;
static TaskHandle_t cpu_task_handle = nullptr;

// Delta tracking for calculation
static uint32_t last_idle_runtime = 0;
static uint32_t last_total_runtime = 0;
static bool first_calculation = true;

#define CPU_MINMAX_WINDOW_SECONDS 60

// Flash/sketch metadata caching (avoid re-entrant ESP-IDF image/mmap helpers)
static bool flash_cache_initialized = false;
static size_t cached_sketch_size = 0;
static size_t cached_free_sketch_space = 0;

static void fill_common(JsonDocument &doc, bool include_ip_and_channel, bool include_debug_fields);

static void get_memory_snapshot(
    size_t *out_heap_free,
    size_t *out_heap_min,
    size_t *out_heap_largest,
    size_t *out_internal_free,
    size_t *out_internal_min,
    size_t *out_psram_free,
    size_t *out_psram_min,
    size_t *out_psram_largest
);

// /api/health min/max window sampling (reset on /api/health).
// Goal: capture short-lived dips/spikes between HTTP polls without storing
// time series on-device.
namespace {
static portMUX_TYPE g_health_window_mux = portMUX_INITIALIZER_UNLOCKED;
static TimerHandle_t g_health_window_timer = nullptr;
static constexpr uint32_t kHealthWindowSamplePeriodMs = 200;

struct HealthWindowStats {
    bool initialized;

    size_t internal_free_min;
    size_t internal_free_max;
    size_t internal_largest_min;
    int internal_frag_max;

    size_t psram_free_min;
    size_t psram_free_max;
    size_t psram_largest_min;
    int psram_frag_max;
};

static HealthWindowStats g_health_window = {};

static int compute_fragmentation_percent(size_t free_bytes, size_t largest_bytes) {
    if (free_bytes == 0) return 0;
    if (largest_bytes > free_bytes) return 0;
    float frag = (1.0f - ((float)largest_bytes / (float)free_bytes)) * 100.0f;
    if (frag < 0) frag = 0;
    if (frag > 100) frag = 100;
    return (int)frag;
}

static void health_window_update_sample(size_t internal_free, size_t internal_largest, size_t psram_free, size_t psram_largest) {
    const int internal_frag = compute_fragmentation_percent(internal_free, internal_largest);
    const int psram_frag = compute_fragmentation_percent(psram_free, psram_largest);

    portENTER_CRITICAL(&g_health_window_mux);

    if (!g_health_window.initialized) {
        g_health_window.initialized = true;

        g_health_window.internal_free_min = internal_free;
        g_health_window.internal_free_max = internal_free;
        g_health_window.internal_largest_min = internal_largest;
        g_health_window.internal_frag_max = internal_frag;

        g_health_window.psram_free_min = psram_free;
        g_health_window.psram_free_max = psram_free;
        g_health_window.psram_largest_min = psram_largest;
        g_health_window.psram_frag_max = psram_frag;

        portEXIT_CRITICAL(&g_health_window_mux);
        return;
    }

    if (internal_free < g_health_window.internal_free_min) g_health_window.internal_free_min = internal_free;
    if (internal_free > g_health_window.internal_free_max) g_health_window.internal_free_max = internal_free;
    if (internal_largest < g_health_window.internal_largest_min) g_health_window.internal_largest_min = internal_largest;
    if (internal_frag > g_health_window.internal_frag_max) g_health_window.internal_frag_max = internal_frag;

    if (psram_free < g_health_window.psram_free_min) g_health_window.psram_free_min = psram_free;
    if (psram_free > g_health_window.psram_free_max) g_health_window.psram_free_max = psram_free;
    if (psram_largest < g_health_window.psram_largest_min) g_health_window.psram_largest_min = psram_largest;
    if (psram_frag > g_health_window.psram_frag_max) g_health_window.psram_frag_max = psram_frag;

    portEXIT_CRITICAL(&g_health_window_mux);
}

static void health_window_timer_cb(TimerHandle_t) {
    size_t heap_free = 0;
    size_t heap_min = 0;
    size_t heap_largest = 0;
    size_t internal_free = 0;
    size_t internal_min = 0;
    size_t psram_free = 0;
    size_t psram_min = 0;
    size_t psram_largest = 0;

    get_memory_snapshot(
        &heap_free,
        &heap_min,
        &heap_largest,
        &internal_free,
        &internal_min,
        &psram_free,
        &psram_min,
        &psram_largest
    );

    // heap_largest is computed as INTERNAL largest free block (see get_memory_snapshot).
    health_window_update_sample(internal_free, heap_largest, psram_free, psram_largest);
}

static HealthWindowStats health_window_get_and_reset(size_t internal_free_now, size_t internal_largest_now, size_t psram_free_now, size_t psram_largest_now) {
    const int internal_frag_now = compute_fragmentation_percent(internal_free_now, internal_largest_now);
    const int psram_frag_now = compute_fragmentation_percent(psram_free_now, psram_largest_now);

    HealthWindowStats snap;

    portENTER_CRITICAL(&g_health_window_mux);

    // Ensure the current point-in-time values are included in the window we return.
    if (!g_health_window.initialized) {
        g_health_window.initialized = true;
        g_health_window.internal_free_min = internal_free_now;
        g_health_window.internal_free_max = internal_free_now;
        g_health_window.internal_largest_min = internal_largest_now;
        g_health_window.internal_frag_max = internal_frag_now;
        g_health_window.psram_free_min = psram_free_now;
        g_health_window.psram_free_max = psram_free_now;
        g_health_window.psram_largest_min = psram_largest_now;
        g_health_window.psram_frag_max = psram_frag_now;
    } else {
        if (internal_free_now < g_health_window.internal_free_min) g_health_window.internal_free_min = internal_free_now;
        if (internal_free_now > g_health_window.internal_free_max) g_health_window.internal_free_max = internal_free_now;
        if (internal_largest_now < g_health_window.internal_largest_min) g_health_window.internal_largest_min = internal_largest_now;
        if (internal_frag_now > g_health_window.internal_frag_max) g_health_window.internal_frag_max = internal_frag_now;

        if (psram_free_now < g_health_window.psram_free_min) g_health_window.psram_free_min = psram_free_now;
        if (psram_free_now > g_health_window.psram_free_max) g_health_window.psram_free_max = psram_free_now;
        if (psram_largest_now < g_health_window.psram_largest_min) g_health_window.psram_largest_min = psram_largest_now;
        if (psram_frag_now > g_health_window.psram_frag_max) g_health_window.psram_frag_max = psram_frag_now;
    }

    snap = g_health_window;

    // Reset the window and immediately seed the next window with the current values.
    g_health_window = {};
    g_health_window.initialized = true;
    g_health_window.internal_free_min = internal_free_now;
    g_health_window.internal_free_max = internal_free_now;
    g_health_window.internal_largest_min = internal_largest_now;
    g_health_window.internal_frag_max = internal_frag_now;
    g_health_window.psram_free_min = psram_free_now;
    g_health_window.psram_free_max = psram_free_now;
    g_health_window.psram_largest_min = psram_largest_now;
    g_health_window.psram_frag_max = psram_frag_now;

    portEXIT_CRITICAL(&g_health_window_mux);
    return snap;
}
} // namespace

// One-shot tripwire state (per boot).
static bool g_low_mem_tripwire_fired = false;

DeviceMemorySnapshot device_telemetry_get_memory_snapshot() {
    DeviceMemorySnapshot snapshot = {};

    get_memory_snapshot(
        &snapshot.heap_free_bytes,
        &snapshot.heap_min_free_bytes,
        &snapshot.heap_largest_free_block_bytes,
        &snapshot.heap_internal_free_bytes,
        &snapshot.heap_internal_min_free_bytes,
        &snapshot.psram_free_bytes,
        &snapshot.psram_min_free_bytes,
        &snapshot.psram_largest_free_block_bytes
    );

    return snapshot;
}

void device_telemetry_log_memory_snapshot(const char *tag) {
    size_t heap_free = 0;
    size_t heap_min = 0;
    size_t heap_largest = 0;
    size_t internal_free = 0;
    size_t internal_min = 0;
    size_t psram_free = 0;
    size_t psram_min = 0;
    size_t psram_largest = 0;

    get_memory_snapshot(
        &heap_free,
        &heap_min,
        &heap_largest,
        &internal_free,
        &internal_min,
        &psram_free,
        &psram_min,
        &psram_largest
    );

    // Keep this line short to avoid LogManager's internal fixed buffers truncating the output.
    // Keys:
    // hf=heap_free hm=heap_min hl=heap_largest hi=internal_free hin=internal_min
    // pf=psram_free pm=psram_min pl=psram_largest
    // frag=heap fragmentation percent (based on hl/hf)

    unsigned frag_percent = 0;
    if (heap_free > 0) {
        float fragmentation = (1.0f - ((float)heap_largest / (float)heap_free)) * 100.0f;
        if (fragmentation < 0) fragmentation = 0;
        if (fragmentation > 100) fragmentation = 100;
        frag_percent = (unsigned)fragmentation;
    }

    Logger.logMessagef(
        "Mem",
        "%s hf=%u hm=%u hl=%u hi=%u hin=%u frag=%u pf=%u pm=%u pl=%u",
        tag ? tag : "(null)",
        (unsigned)heap_free,
        (unsigned)heap_min,
        (unsigned)heap_largest,
        (unsigned)internal_free,
        (unsigned)internal_min,
        (unsigned)frag_percent,
        (unsigned)psram_free,
        (unsigned)psram_min,
        (unsigned)psram_largest
    );

    // Tripwire: dump task stack watermarks once per boot when internal min heap is critically low.
    // Runtime gating (not preprocessor gating) so we can't accidentally compile this out.
    if (MEMORY_TRIPWIRE_ENABLED) {
        // Use heap_caps min free size as it reflects internal allocator pressure across the boot.
        if (!g_low_mem_tripwire_fired && internal_min > 0 && internal_min < (size_t)MEMORY_TRIPWIRE_INTERNAL_MIN_BYTES) {
            g_low_mem_tripwire_fired = true;
            Logger.logMessagef(
                "Mem",
                "TRIPWIRE fired tag=%s hin=%uB < %uB (hf=%uB hl=%uB frag=%u%% pf=%uB pl=%uB)",
                tag ? tag : "(null)",
                (unsigned)internal_min,
                (unsigned)MEMORY_TRIPWIRE_INTERNAL_MIN_BYTES,
                (unsigned)heap_free,
                (unsigned)heap_largest,
                (unsigned)frag_percent,
                (unsigned)psram_free,
                (unsigned)psram_largest
            );
            device_telemetry_dump_task_stack_watermarks(tag);
        }
    }
}

void device_telemetry_dump_task_stack_watermarks(const char *tag) {
    // Keep this dump compact and parseable (one line per task).
    // NOTE: This runs in the caller task context. Avoid dynamic allocation.
    constexpr uint32_t kMaxTasks = 24;
    TaskStatus_t task_stats[kMaxTasks];
    uint32_t total_runtime = 0;

    const int task_count = uxTaskGetSystemState(task_stats, kMaxTasks, &total_runtime);
    if (task_count <= 0) {
        Logger.logMessage("Mem", "Task dump: uxTaskGetSystemState returned 0");
        return;
    }

    // Simple selection sort by remaining stack bytes ascending (worst first).
    auto remaining_bytes_for = [](TaskHandle_t h) -> uint32_t {
        if (!h) return 0;
        const UBaseType_t remaining_words = uxTaskGetStackHighWaterMark(h);
        return (uint32_t)remaining_words * (uint32_t)sizeof(StackType_t);
    };

    // Precompute remaining stack bytes to avoid repeated calls in sort.
    uint32_t rem_bytes[kMaxTasks];
    const int n = task_count > (int)kMaxTasks ? (int)kMaxTasks : task_count;
    for (int i = 0; i < n; i++) {
        rem_bytes[i] = remaining_bytes_for(task_stats[i].xHandle);
    }

    for (int i = 0; i < n; i++) {
        int min_i = i;
        for (int j = i + 1; j < n; j++) {
            if (rem_bytes[j] < rem_bytes[min_i]) min_i = j;
        }
        if (min_i != i) {
            TaskStatus_t tmp = task_stats[i];
            task_stats[i] = task_stats[min_i];
            task_stats[min_i] = tmp;

            uint32_t tmpb = rem_bytes[i];
            rem_bytes[i] = rem_bytes[min_i];
            rem_bytes[min_i] = tmpb;
        }
    }

    Logger.logMessagef(
        "Mem",
        "Task dump (%d tasks, sorted by stack margin) tag=%s",
        n,
        tag ? tag : "(null)"
    );

    for (int i = 0; i < n; i++) {
        const TaskStatus_t &t = task_stats[i];
        const uint32_t rb = rem_bytes[i];
        const char *name = t.pcTaskName;
        // Keep log lines short (LogManager buffers are small).
        Logger.logMessagef(
            "Task",
            "name=%s prio=%u core=%d stack_rem=%uB",
            name ? name : "(null)",
            (unsigned)t.uxCurrentPriority,
            (int)t.xCoreID,
            (unsigned)rb
        );
    }

    if (task_count > (int)kMaxTasks) {
        Logger.logMessagef("Mem", "Task dump truncated: total=%d max=%u", task_count, (unsigned)kMaxTasks);
    }
}

void device_telemetry_fill_api(JsonDocument &doc) {
    fill_common(doc, true, true);

    // =====================================================================
    // USER-EXTEND: Add your own sensors to the web "health" API (/api/health)
    // =====================================================================
    // If you want your external sensors to show up in the web portal health widget,
    // add fields here.
    //
    // IMPORTANT:
    // - The key "cpu_temperature" is used for the SoC/internal temperature.
    //   You can safely use "temperature" for an external/ambient sensor.
    // - If you also publish these over MQTT, keep the JSON keys identical in
    //   device_telemetry_fill_mqtt() so you can reuse the same HA templates.
    //
    // Example (commented out):
    // doc["temperature"] = 23.4;
    // doc["humidity"] = 55.2;
}

void device_telemetry_fill_mqtt(JsonDocument &doc) {
    fill_common(doc, false, false);

    // =====================================================================
    // USER-EXTEND: Add your own sensors to the MQTT state payload
    // =====================================================================
    // The MQTT integration publishes ONE batched JSON document (retained) to:
    //   devices/<sanitized>/health/state
    // Home Assistant entities then extract values via value_template, e.g.:
    //   {{ value_json.temperature }}
    //
    // Add your custom sensor fields below.
    //
    // IMPORTANT:
    // - The key "cpu_temperature" is used for the SoC/internal temperature.
    //   You can safely use "temperature" for an external/ambient sensor.
    //
    // Example (commented out):
    // doc["temperature"] = 23.4;
    // doc["humidity"] = 55.2;
}

void device_telemetry_init() {
    if (flash_cache_initialized) return;

    cached_sketch_size = ESP.getSketchSize();
    cached_free_sketch_space = ESP.getFreeSketchSpace();
    flash_cache_initialized = true;
}

void device_telemetry_start_health_window_sampling() {
    if (g_health_window_timer != nullptr) return;

    // FreeRTOS timer runs on the timer service task. This avoids adding a new task.
    g_health_window_timer = xTimerCreate(
        "health_win",
        pdMS_TO_TICKS(kHealthWindowSamplePeriodMs),
        pdTRUE,
        nullptr,
        health_window_timer_cb
    );
    if (g_health_window_timer == nullptr) {
        Logger.logMessage("HealthWin", "Failed to create sampler timer");
        return;
    }
    if (xTimerStart(g_health_window_timer, 0) != pdPASS) {
        Logger.logMessage("HealthWin", "Failed to start sampler timer");
    }
}

size_t device_telemetry_sketch_size() {
    if (!flash_cache_initialized) {
        device_telemetry_init();
    }
    return cached_sketch_size;
}

size_t device_telemetry_free_sketch_space() {
    if (!flash_cache_initialized) {
        device_telemetry_init();
    }
    return cached_free_sketch_space;
}

// Internal: Calculate CPU usage from IDLE task runtime
static int calculate_cpu_usage() {
    TaskStatus_t task_stats[16];
    uint32_t total_runtime;
    int task_count = uxTaskGetSystemState(task_stats, 16, &total_runtime);

    // Count IDLE tasks and sum their runtimes
    uint32_t idle_runtime = 0;
    int idle_task_count = 0;
    for (int i = 0; i < task_count; i++) {
        if (strstr(task_stats[i].pcTaskName, "IDLE") != nullptr) {
            idle_runtime += task_stats[i].ulRunTimeCounter;
            idle_task_count++;
        }
    }

    // Skip first calculation (need delta)
    if (first_calculation) {
        last_idle_runtime = idle_runtime;
        last_total_runtime = total_runtime;
        first_calculation = false;
        return 0;
    }

    // Calculate delta
    uint32_t idle_delta = idle_runtime - last_idle_runtime;
    uint32_t total_delta = total_runtime - last_total_runtime;
    
    last_idle_runtime = idle_runtime;
    last_total_runtime = total_runtime;

    if (total_delta == 0) return 0;

    // FreeRTOS uxTaskGetSystemState:
    // - total_runtime = elapsed wall-clock time (in timer ticks) since boot
    // - Each task's ulRunTimeCounter = time that task has been running
    // 
    // On dual-core ESP32:
    // - 2 IDLE tasks (IDLE0, IDLE1), one per core
    // - Maximum possible idle time = total_delta * number_of_cores
    // - idle_delta = combined idle time from both cores
    //
    // CPU usage = 100 - (idle_time / max_possible_idle_time * 100)
    uint32_t max_idle_time = total_delta * idle_task_count;
    if (max_idle_time == 0) return 0;
    
    float idle_percent = ((float)idle_delta / max_idle_time) * 100.0f;
    int cpu_usage = (int)(100.0f - idle_percent);
    
    if (cpu_usage < 0) cpu_usage = 0;
    if (cpu_usage > 100) cpu_usage = 100;
    
    return cpu_usage;
}

// Background task: Calculate CPU usage every 1s, track min/max over 60s window
static void cpu_monitoring_task(void* param) {
    while (true) {
        int new_value = calculate_cpu_usage();
        unsigned long now = millis();
        
        xSemaphoreTake(cpu_mutex, portMAX_DELAY);
        
        cpu_usage_current = new_value;
        
        // Update min/max
        if (new_value < cpu_usage_min) cpu_usage_min = new_value;
        if (new_value > cpu_usage_max) cpu_usage_max = new_value;
        
        // Reset min/max every 60 seconds
        if (last_minmax_reset == 0 || (now - last_minmax_reset >= CPU_MINMAX_WINDOW_SECONDS * 1000UL)) {
            cpu_usage_min = new_value;
            cpu_usage_max = new_value;
            last_minmax_reset = now;
        }
        
        xSemaphoreGive(cpu_mutex);
        
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

void device_telemetry_start_cpu_monitoring() {
    if (cpu_task_handle != nullptr) return;  // Already started
    
    cpu_mutex = xSemaphoreCreateMutex();
    if (cpu_mutex == nullptr) {
        Logger.logMessage("CPU Monitor", "Failed to create mutex");
        return;
    }
    
    BaseType_t result = xTaskCreate(
        cpu_monitoring_task,
        "cpu_monitor",
        2048,  // Stack size
        nullptr,
        1,  // Low priority
        &cpu_task_handle
    );
    
    if (result != pdPASS) {
        Logger.logMessage("CPU Monitor", "Failed to create task");
        vSemaphoreDelete(cpu_mutex);
        cpu_mutex = nullptr;
    }
}

int device_telemetry_get_cpu_usage() {
    if (cpu_mutex == nullptr) return 0;  // Not initialized
    
    xSemaphoreTake(cpu_mutex, portMAX_DELAY);
    int value = cpu_usage_current;
    xSemaphoreGive(cpu_mutex);
    
    return value;
}

void device_telemetry_get_cpu_minmax(int* out_min, int* out_max) {
    if (cpu_mutex == nullptr) {
        if (out_min) *out_min = 0;
        if (out_max) *out_max = 0;
        return;
    }
    
    xSemaphoreTake(cpu_mutex, portMAX_DELAY);
    if (out_min) *out_min = cpu_usage_min;
    if (out_max) *out_max = cpu_usage_max;
    xSemaphoreGive(cpu_mutex);
}

static void fill_common(JsonDocument &doc, bool include_ip_and_channel, bool include_debug_fields) {
    fs_health_init();

    // System
    uint64_t uptime_us = esp_timer_get_time();
    doc["uptime_seconds"] = uptime_us / 1000000;

    // Reset reason
    esp_reset_reason_t reset_reason = esp_reset_reason();
    const char* reset_str = "Unknown";
    switch (reset_reason) {
        case ESP_RST_POWERON:   reset_str = "Power On"; break;
        case ESP_RST_SW:        reset_str = "Software"; break;
        case ESP_RST_PANIC:     reset_str = "Panic"; break;
        case ESP_RST_INT_WDT:   reset_str = "Interrupt WDT"; break;
        case ESP_RST_TASK_WDT:  reset_str = "Task WDT"; break;
        case ESP_RST_WDT:       reset_str = "WDT"; break;
        case ESP_RST_DEEPSLEEP: reset_str = "Deep Sleep"; break;
        case ESP_RST_BROWNOUT:  reset_str = "Brownout"; break;
        case ESP_RST_SDIO:      reset_str = "SDIO"; break;
        default: break;
    }
    doc["reset_reason"] = reset_str;

    // CPU (API includes cpu_freq; MQTT keeps payload smaller)
    if (include_debug_fields) {
        doc["cpu_freq"] = ESP.getCpuFreqMHz();
    }

    // CPU usage with min/max over 60s window
    doc["cpu_usage"] = device_telemetry_get_cpu_usage();
    int cpu_min, cpu_max;
    device_telemetry_get_cpu_minmax(&cpu_min, &cpu_max);
    doc["cpu_usage_min"] = cpu_min;
    doc["cpu_usage_max"] = cpu_max;

    // CPU / SoC temperature
#if SOC_TEMP_SENSOR_SUPPORTED
    float temp_celsius = 0;
    temperature_sensor_handle_t temp_sensor = NULL;
    temperature_sensor_config_t temp_sensor_config = TEMPERATURE_SENSOR_CONFIG_DEFAULT(-10, 80);

    if (temperature_sensor_install(&temp_sensor_config, &temp_sensor) == ESP_OK) {
        if (temperature_sensor_enable(temp_sensor) == ESP_OK) {
            if (temperature_sensor_get_celsius(temp_sensor, &temp_celsius) == ESP_OK) {
                doc["cpu_temperature"] = (int)temp_celsius;
            } else {
                doc["cpu_temperature"] = nullptr;
            }
            temperature_sensor_disable(temp_sensor);
        } else {
            doc["cpu_temperature"] = nullptr;
        }
        temperature_sensor_uninstall(temp_sensor);
    } else {
        doc["cpu_temperature"] = nullptr;
    }
#else
    doc["cpu_temperature"] = nullptr;
#endif

    // Memory
    size_t heap_free = 0;
    size_t heap_min = 0;
    size_t heap_largest = 0;
    size_t internal_free = 0;
    size_t internal_min = 0;
    size_t psram_free = 0;
    size_t psram_min = 0;
    size_t psram_largest = 0;

    get_memory_snapshot(
        &heap_free,
        &heap_min,
        &heap_largest,
        &internal_free,
        &internal_min,
        &psram_free,
        &psram_min,
        &psram_largest
    );

    doc["heap_free"] = heap_free;
    doc["heap_min"] = heap_min;
    if (include_debug_fields) {
        doc["heap_size"] = ESP.getHeapSize();
    }

    // Additional heap/PSRAM details (useful for memory/fragmentation investigations)
    doc["heap_largest"] = heap_largest;
    doc["heap_internal_free"] = internal_free;
    doc["heap_internal_min"] = internal_min;
    const size_t internal_largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    doc["heap_internal_largest"] = internal_largest;
    doc["psram_free"] = psram_free;
    doc["psram_min"] = psram_min;
    doc["psram_largest"] = psram_largest;

    // Heap fragmentation
    // IMPORTANT: On PSRAM boards, `heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)` can return a PSRAM block,
    // while `ESP.getFreeHeap()` reports internal heap only. Mixing those yields negative fragmentation.
    // We define heap fragmentation as INTERNAL heap fragmentation.
    const int heap_frag = compute_fragmentation_percent(internal_free, internal_largest);
    doc["heap_fragmentation"] = heap_frag;

    const int psram_frag = compute_fragmentation_percent(psram_free, psram_largest);
    doc["psram_fragmentation"] = psram_frag;

    // Windowed min/max sampling between /api/health calls.
    // Only include (and reset) these fields for the web API path.
    if (include_debug_fields) {
        const HealthWindowStats win = health_window_get_and_reset(internal_free, internal_largest, psram_free, psram_largest);
        if (win.initialized) {
            doc["heap_internal_free_min_window"] = win.internal_free_min;
            doc["heap_internal_free_max_window"] = win.internal_free_max;
            doc["heap_internal_largest_min_window"] = win.internal_largest_min;
            doc["heap_fragmentation_max_window"] = win.internal_frag_max;

            doc["psram_free_min_window"] = win.psram_free_min;
            doc["psram_free_max_window"] = win.psram_free_max;
            doc["psram_largest_min_window"] = win.psram_largest_min;
            doc["psram_fragmentation_max_window"] = win.psram_frag_max;
        } else {
            doc["heap_internal_free_min_window"] = nullptr;
            doc["heap_internal_free_max_window"] = nullptr;
            doc["heap_internal_largest_min_window"] = nullptr;
            doc["heap_fragmentation_max_window"] = nullptr;

            doc["psram_free_min_window"] = nullptr;
            doc["psram_free_max_window"] = nullptr;
            doc["psram_largest_min_window"] = nullptr;
            doc["psram_fragmentation_max_window"] = nullptr;
        }
    }

    // Flash usage
    const size_t sketch_size = device_telemetry_sketch_size();
    const size_t free_sketch_space = device_telemetry_free_sketch_space();
    doc["flash_used"] = sketch_size;
    doc["flash_total"] = sketch_size + free_sketch_space;

    // Filesystem (cached; no mounting/probing from /api/health)
    {
        FSHealthStats fs;
        fs_health_get(&fs);

        if (fs.ffat_partition_present) {
            doc["fs_type"] = "ffat";
            doc["fs_mounted"] = fs.ffat_mounted;
            if (fs.ffat_mounted && fs.ffat_total_bytes > 0) {
                doc["fs_used_bytes"] = fs.ffat_used_bytes;
                doc["fs_total_bytes"] = fs.ffat_total_bytes;
            } else {
                doc["fs_used_bytes"] = nullptr;
                doc["fs_total_bytes"] = nullptr;
            }
        } else {
            doc["fs_type"] = nullptr;
            doc["fs_mounted"] = nullptr;
            doc["fs_used_bytes"] = nullptr;
            doc["fs_total_bytes"] = nullptr;
        }
    }

    // MQTT status (best-effort)
#if HAS_MQTT
    {
        doc["mqtt_enabled"] = mqtt_manager.enabled();
        doc["mqtt_publish_enabled"] = mqtt_manager.publishEnabled();
        const bool connected = mqtt_manager.connected();
        doc["mqtt_connected"] = connected;
        const unsigned long last_pub = mqtt_manager.lastHealthPublishMs();
        if (last_pub) {
            doc["mqtt_last_health_publish_ms"] = (uint32_t)last_pub;
            doc["mqtt_health_publish_age_ms"] = (uint32_t)(millis() - last_pub);
        } else {
            doc["mqtt_last_health_publish_ms"] = nullptr;
            doc["mqtt_health_publish_age_ms"] = nullptr;
        }
    }
#else
    doc["mqtt_enabled"] = false;
    doc["mqtt_publish_enabled"] = false;
    doc["mqtt_connected"] = nullptr;
    doc["mqtt_last_health_publish_ms"] = nullptr;
    doc["mqtt_health_publish_age_ms"] = nullptr;
#endif

    // Display perf stats (best-effort)
#if HAS_DISPLAY
    {
        DisplayPerfStats stats;
        if (display_manager_get_perf_stats(&stats)) {
            doc["display_fps"] = stats.fps;
            doc["display_lv_timer_us"] = stats.lv_timer_us;
            doc["display_present_us"] = stats.present_us;
        } else {
            doc["display_fps"] = nullptr;
            doc["display_lv_timer_us"] = nullptr;
            doc["display_present_us"] = nullptr;
        }
    }
#else
    doc["display_fps"] = nullptr;
    doc["display_lv_timer_us"] = nullptr;
    doc["display_present_us"] = nullptr;
#endif

    // WiFi stats (only if connected)
    if (WiFi.status() == WL_CONNECTED) {
        doc["wifi_rssi"] = WiFi.RSSI();

        if (include_ip_and_channel) {
            doc["wifi_channel"] = WiFi.channel();
            {
                IPAddress ip = WiFi.localIP();
                char ip_buf[16];
                snprintf(ip_buf, sizeof(ip_buf), "%u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);
                doc["ip_address"] = ip_buf;
            }
            doc["hostname"] = WiFi.getHostname();
        }
    } else {
        doc["wifi_rssi"] = nullptr;

        if (include_ip_and_channel) {
            doc["wifi_channel"] = nullptr;
            doc["ip_address"] = nullptr;
            doc["hostname"] = nullptr;
        }
    }
}

static void get_memory_snapshot(
    size_t *out_heap_free,
    size_t *out_heap_min,
    size_t *out_heap_largest,
    size_t *out_internal_free,
    size_t *out_internal_min,
    size_t *out_psram_free,
    size_t *out_psram_min,
    size_t *out_psram_largest
) {
    if (out_heap_free) *out_heap_free = ESP.getFreeHeap();
    if (out_heap_min) *out_heap_min = ESP.getMinFreeHeap();

    if (out_heap_largest) {
        // Keep this consistent with ESP.getFreeHeap() (internal heap): use INTERNAL 8-bit largest block.
        *out_heap_largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }

    if (out_internal_free) {
        *out_internal_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    if (out_internal_min) {
        *out_internal_min = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }

#if SOC_SPIRAM_SUPPORTED
    if (out_psram_free) {
        *out_psram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    }
    if (out_psram_min) {
        *out_psram_min = heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM);
    }
    if (out_psram_largest) {
        *out_psram_largest = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
    }
#else
    if (out_psram_free) *out_psram_free = 0;
    if (out_psram_min) *out_psram_min = 0;
    if (out_psram_largest) *out_psram_largest = 0;
#endif
}