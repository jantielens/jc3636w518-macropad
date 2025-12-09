/*
 * Device Statistics Implementation
 * 
 * Collects and formats device health statistics.
 * Used by both web portal (/api/health) and home screen UI.
 */

#include "device_stats.h"
#include "config_manager.h"
#include "../version.h"
#include <WiFi.h>
#include <esp_timer.h>
#include <esp_system.h>

// Temperature sensor support (ESP32-C3, ESP32-S2, ESP32-S3, ESP32-C2, ESP32-C6, ESP32-H2)
#if SOC_TEMP_SENSOR_SUPPORTED
#include "driver/temperature_sensor.h"
#endif

// CPU usage tracking (static variables)
static uint32_t last_idle_runtime = 0;
static uint32_t last_total_runtime = 0;
static unsigned long last_cpu_check = 0;

// Initialize stats tracking
void device_stats_init() {
    last_idle_runtime = 0;
    last_total_runtime = 0;
    last_cpu_check = 0;
}

// Get uptime as formatted string
String device_stats_get_uptime() {
    uint64_t uptime_us = esp_timer_get_time();
    uint64_t uptime_seconds = uptime_us / 1000000;
    
    uint32_t days = uptime_seconds / 86400;
    uint32_t hours = (uptime_seconds % 86400) / 3600;
    uint32_t minutes = (uptime_seconds % 3600) / 60;
    
    String result = "";
    
    if (days > 0) {
        result += String(days) + "d ";
    }
    
    if (hours > 0 || days > 0) {
        result += String(hours) + "h ";
    }
    
    result += String(minutes) + "m";
    
    return result;
}

// Get CPU usage percentage (0-100)
int device_stats_get_cpu_usage() {
    TaskStatus_t task_stats[16];
    uint32_t total_runtime;
    int task_count = uxTaskGetSystemState(task_stats, 16, &total_runtime);
    
    // Find IDLE task(s) and sum their runtime
    uint32_t idle_runtime = 0;
    for (int i = 0; i < task_count; i++) {
        if (strstr(task_stats[i].pcTaskName, "IDLE") != nullptr) {
            idle_runtime += task_stats[i].ulRunTimeCounter;
        }
    }
    
    // Calculate CPU usage based on delta since last measurement
    unsigned long now = millis();
    int cpu_usage = -1;
    
    if (last_cpu_check > 0 && (now - last_cpu_check) > 100) {  // Minimum 100ms between measurements
        uint32_t idle_delta = idle_runtime - last_idle_runtime;
        uint32_t total_delta = total_runtime - last_total_runtime;
        
        if (total_delta > 0) {
            float idle_percent = ((float)idle_delta / total_delta) * 100.0;
            cpu_usage = (int)(100.0 - idle_percent);
            // Clamp to valid range
            if (cpu_usage < 0) cpu_usage = 0;
            if (cpu_usage > 100) cpu_usage = 100;
        }
    }
    
    // Update tracking variables
    last_idle_runtime = idle_runtime;
    last_total_runtime = total_runtime;
    last_cpu_check = now;
    
    return cpu_usage;
}

// Get temperature in Celsius
int device_stats_get_temperature() {
#if SOC_TEMP_SENSOR_SUPPORTED
    float temp_celsius = 0;
    temperature_sensor_handle_t temp_sensor = NULL;
    temperature_sensor_config_t temp_sensor_config = TEMPERATURE_SENSOR_CONFIG_DEFAULT(-10, 80);
    
    if (temperature_sensor_install(&temp_sensor_config, &temp_sensor) == ESP_OK) {
        if (temperature_sensor_enable(temp_sensor) == ESP_OK) {
            if (temperature_sensor_get_celsius(temp_sensor, &temp_celsius) == ESP_OK) {
                temperature_sensor_disable(temp_sensor);
                temperature_sensor_uninstall(temp_sensor);
                return (int)temp_celsius;
            }
            temperature_sensor_disable(temp_sensor);
        }
        temperature_sensor_uninstall(temp_sensor);
    }
#endif
    return -1;  // Not supported or unavailable
}

// Get free heap as formatted string
String device_stats_get_heap_free() {
    size_t free_bytes = ESP.getFreeHeap();
    
    if (free_bytes >= 1024 * 1024) {
        // MB
        float mb = free_bytes / (1024.0 * 1024.0);
        return String(mb, 1) + " MB";
    } else if (free_bytes >= 1024) {
        // KB
        int kb = free_bytes / 1024;
        return String(kb) + " KB";
    } else {
        // Bytes
        return String(free_bytes) + " B";
    }
}

// Get heap size as formatted string
String device_stats_get_heap_size() {
    size_t size_bytes = ESP.getHeapSize();
    
    if (size_bytes >= 1024 * 1024) {
        // MB
        float mb = size_bytes / (1024.0 * 1024.0);
        return String(mb, 1) + " MB";
    } else if (size_bytes >= 1024) {
        // KB
        int kb = size_bytes / 1024;
        return String(kb) + " KB";
    } else {
        // Bytes
        return String(size_bytes) + " B";
    }
}

// Get WiFi RSSI in dBm
int device_stats_get_wifi_rssi() {
    if (WiFi.status() == WL_CONNECTED) {
        return WiFi.RSSI();
    }
    return 0;
}

// Get WiFi signal quality as human-readable string
String device_stats_get_wifi_signal_quality() {
    if (WiFi.status() != WL_CONNECTED) {
        return "No Signal";
    }
    
    int rssi = WiFi.RSSI();
    
    if (rssi > -50) {
        return "Excellent";
    } else if (rssi > -60) {
        return "Good";
    } else if (rssi > -70) {
        return "Fair";
    } else {
        return "Poor";
    }
}

// Get IP address as string
String device_stats_get_ip_address() {
    if (WiFi.status() == WL_CONNECTED) {
        return WiFi.localIP().toString();
    }
    return "";
}

// Get hostname
String device_stats_get_hostname() {
    // WiFi hostname is set from device_name on connect
    const char* hostname = WiFi.getHostname();
    if (hostname && strlen(hostname) > 0) {
        return String(hostname);
    }
    // Fallback to default device name if hostname not set yet
    return config_manager_get_default_device_name();
}

// Get firmware version
String device_stats_get_firmware_version() {
    return String(FIRMWARE_VERSION);
}
