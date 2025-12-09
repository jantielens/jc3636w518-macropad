/*
 * Device Statistics
 * 
 * Collects and formats device health statistics.
 * Used by both web portal (/api/health) and home screen UI.
 */

#ifndef DEVICE_STATS_H
#define DEVICE_STATS_H

#include <Arduino.h>

// Get system uptime as formatted string (e.g., "2h 15m" or "3d 5h")
String device_stats_get_uptime();

// Get CPU usage percentage (0-100)
// Returns -1 if measurement not yet available
int device_stats_get_cpu_usage();

// Get internal temperature in Celsius
// Returns -1 if temperature sensor not supported or unavailable
int device_stats_get_temperature();

// Get free heap memory as formatted string (e.g., "234 KB")
String device_stats_get_heap_free();

// Get heap size as formatted string (e.g., "320 KB")
String device_stats_get_heap_size();

// Get WiFi RSSI in dBm
// Returns 0 if WiFi not connected
int device_stats_get_wifi_rssi();

// Get WiFi signal quality as human-readable string
// Returns: "Excellent" (>-50), "Good" (>-60), "Fair" (>-70), "Poor" (<-70), "No Signal"
String device_stats_get_wifi_signal_quality();

// Get IP address as string
// Returns empty string if WiFi not connected
String device_stats_get_ip_address();

// Get hostname
String device_stats_get_hostname();

// Get firmware version from version.h
String device_stats_get_firmware_version();

// Reset CPU usage tracking (call when stats module initializes)
void device_stats_init();

#endif // DEVICE_STATS_H
