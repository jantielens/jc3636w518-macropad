/*
 * Configuration Manager
 * 
 * Manages persistent storage of device configuration in ESP32 NVS.
 * Provides load/save/reset functionality with validation.
 * 
 * USAGE:
 *   config_manager_init();           // Initialize NVS
 *   if (config_manager_load()) {     // Try to load saved config
 *       // Config loaded, use it
 *   } else {
 *       // No config found, need to configure
 *   }
 *   config_manager_save();           // Save after user configures
 *   config_manager_reset();          // Erase all config
 */

#ifndef CONFIG_MANAGER_H
#define CONFIG_MANAGER_H

#include <Arduino.h>

// Maximum string lengths
#define CONFIG_SSID_MAX_LEN 32
#define CONFIG_PASSWORD_MAX_LEN 64
#define CONFIG_DEVICE_NAME_MAX_LEN 32
#define CONFIG_IP_STR_MAX_LEN 16
#define CONFIG_DUMMY_MAX_LEN 64

// Configuration structure
struct DeviceConfig {
    // WiFi credentials
    char wifi_ssid[CONFIG_SSID_MAX_LEN];
    char wifi_password[CONFIG_PASSWORD_MAX_LEN];
    
    // Device settings
    char device_name[CONFIG_DEVICE_NAME_MAX_LEN];
    
    // Optional fixed IP configuration
    char fixed_ip[CONFIG_IP_STR_MAX_LEN];
    char subnet_mask[CONFIG_IP_STR_MAX_LEN];
    char gateway[CONFIG_IP_STR_MAX_LEN];
    char dns1[CONFIG_IP_STR_MAX_LEN];
    char dns2[CONFIG_IP_STR_MAX_LEN];
    
    // Dummy setting (example for extensibility)
    char dummy_setting[CONFIG_DUMMY_MAX_LEN];
    
    // Validation flag (magic number to detect valid config)
    uint32_t magic;
};

// Magic number for config validation
#define CONFIG_MAGIC 0xDEADBEEF

// API Functions
void config_manager_init();                           // Initialize NVS
bool config_manager_load(DeviceConfig *config);       // Load config from NVS
bool config_manager_save(const DeviceConfig *config); // Save config to NVS
bool config_manager_reset();                          // Erase config from NVS
bool config_manager_is_valid(const DeviceConfig *config); // Check if config is valid
void config_manager_print(const DeviceConfig *config); // Debug print config
void config_manager_sanitize_device_name(const char *input, char *output, size_t max_len); // Sanitize name for mDNS
String config_manager_get_default_device_name();      // Get default device name with chip ID

#endif // CONFIG_MANAGER_H
