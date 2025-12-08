/*
 * Configuration Manager Implementation
 * 
 * Uses ESP32 Preferences library (NVS wrapper) for persistent storage.
 * Stores configuration in "device_cfg" namespace.
 */

#include "config_manager.h"
#include "web_assets.h"
#include "log_manager.h"
#include <Preferences.h>

// NVS namespace
#define CONFIG_NAMESPACE "device_cfg"

// Preferences keys
#define KEY_WIFI_SSID      "wifi_ssid"
#define KEY_WIFI_PASS      "wifi_pass"
#define KEY_DEVICE_NAME    "device_name"
#define KEY_FIXED_IP       "fixed_ip"
#define KEY_SUBNET_MASK    "subnet_mask"
#define KEY_GATEWAY        "gateway"
#define KEY_DNS1           "dns1"
#define KEY_DNS2           "dns2"
#define KEY_DUMMY          "dummy"
#define KEY_MAGIC          "magic"

static Preferences preferences;

// Initialize NVS
void config_manager_init() {
    Logger.logMessage("Config", "NVS initialized");
}

// Get default device name with unique chip ID
String config_manager_get_default_device_name() {
    uint32_t chipId = 0;
    for (int i = 0; i < 17; i = i + 8) {
        chipId |= ((ESP.getEfuseMac() >> (40 - i)) & 0xff) << i;
    }
    char name[32];
    snprintf(name, sizeof(name), PROJECT_DISPLAY_NAME " %04X", (uint16_t)(chipId & 0xFFFF));
    return String(name);
}

// Sanitize device name for mDNS (lowercase, alphanumeric + hyphens only)
void config_manager_sanitize_device_name(const char *input, char *output, size_t max_len) {
    if (!input || !output || max_len == 0) return;
    
    size_t j = 0;
    for (size_t i = 0; input[i] != '\0' && j < max_len - 1; i++) {
        char c = input[i];
        
        // Convert to lowercase
        if (c >= 'A' && c <= 'Z') {
            c = c + ('a' - 'A');
        }
        
        // Keep alphanumeric and convert spaces/special chars to hyphens
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) {
            output[j++] = c;
        } else if (c == ' ' || c == '_' || c == '-') {
            // Don't add hyphen if previous char was already a hyphen
            if (j > 0 && output[j-1] != '-') {
                output[j++] = '-';
            }
        }
    }
    
    // Remove trailing hyphen if present
    if (j > 0 && output[j-1] == '-') {
        j--;
    }
    
    output[j] = '\0';
}

// Load configuration from NVS
bool config_manager_load(DeviceConfig *config) {
    if (!config) {
        Logger.logMessage("Config", "Load failed: NULL pointer");
        return false;
    }
    
    Logger.logBegin("Config Load");
    
    preferences.begin(CONFIG_NAMESPACE, true); // Read-only mode
    
    // Check magic number first
    uint32_t magic = preferences.getUInt(KEY_MAGIC, 0);
    if (magic != CONFIG_MAGIC) {
        preferences.end();
        Logger.logEnd("No config found");
        return false;
    }
    
    // Load WiFi settings
    preferences.getString(KEY_WIFI_SSID, config->wifi_ssid, CONFIG_SSID_MAX_LEN);
    preferences.getString(KEY_WIFI_PASS, config->wifi_password, CONFIG_PASSWORD_MAX_LEN);
    
    // Load device settings
    String default_name = config_manager_get_default_device_name();
    preferences.getString(KEY_DEVICE_NAME, config->device_name, CONFIG_DEVICE_NAME_MAX_LEN);
    if (strlen(config->device_name) == 0) {
        strlcpy(config->device_name, default_name.c_str(), CONFIG_DEVICE_NAME_MAX_LEN);
    }
    
    // Load fixed IP settings
    preferences.getString(KEY_FIXED_IP, config->fixed_ip, CONFIG_IP_STR_MAX_LEN);
    preferences.getString(KEY_SUBNET_MASK, config->subnet_mask, CONFIG_IP_STR_MAX_LEN);
    preferences.getString(KEY_GATEWAY, config->gateway, CONFIG_IP_STR_MAX_LEN);
    preferences.getString(KEY_DNS1, config->dns1, CONFIG_IP_STR_MAX_LEN);
    preferences.getString(KEY_DNS2, config->dns2, CONFIG_IP_STR_MAX_LEN);
    
    // Load dummy setting
    preferences.getString(KEY_DUMMY, config->dummy_setting, CONFIG_DUMMY_MAX_LEN);
    
    config->magic = magic;
    
    preferences.end();
    
    // Validate loaded config
    if (!config_manager_is_valid(config)) {
        Logger.logEnd("Invalid config");
        return false;
    }
    
    config_manager_print(config);
    Logger.logEnd();
    return true;
}

// Save configuration to NVS
bool config_manager_save(const DeviceConfig *config) {
    if (!config) {
        Logger.logMessage("Config", "Save failed: NULL pointer");
        return false;
    }
    
    if (!config_manager_is_valid(config)) {
        Logger.logMessage("Config", "Save failed: Invalid config");
        return false;
    }
    
    Logger.logBegin("Config Save");
    
    preferences.begin(CONFIG_NAMESPACE, false); // Read-write mode
    
    // Save WiFi settings
    preferences.putString(KEY_WIFI_SSID, config->wifi_ssid);
    preferences.putString(KEY_WIFI_PASS, config->wifi_password);
    
    // Save device settings
    preferences.putString(KEY_DEVICE_NAME, config->device_name);
    
    // Save fixed IP settings
    preferences.putString(KEY_FIXED_IP, config->fixed_ip);
    preferences.putString(KEY_SUBNET_MASK, config->subnet_mask);
    preferences.putString(KEY_GATEWAY, config->gateway);
    preferences.putString(KEY_DNS1, config->dns1);
    preferences.putString(KEY_DNS2, config->dns2);
    
    // Save dummy setting
    preferences.putString(KEY_DUMMY, config->dummy_setting);
    
    // Save magic number last (indicates valid config)
    preferences.putUInt(KEY_MAGIC, CONFIG_MAGIC);
    
    preferences.end();
    
    config_manager_print(config);
    Logger.logEnd();
    return true;
}

// Reset configuration (erase from NVS)
bool config_manager_reset() {
    Logger.logBegin("Config Reset");
    
    preferences.begin(CONFIG_NAMESPACE, false);
    bool success = preferences.clear();
    preferences.end();
    
    if (success) {
        Logger.logEnd();
    } else {
        Logger.logEnd("Failed to reset");
    }
    
    return success;
}

// Check if configuration is valid
bool config_manager_is_valid(const DeviceConfig *config) {
    if (!config) return false;
    if (config->magic != CONFIG_MAGIC) return false;
    if (strlen(config->wifi_ssid) == 0) return false;
    if (strlen(config->device_name) == 0) return false;
    return true;
}

// Print configuration (for debugging)
void config_manager_print(const DeviceConfig *config) {
    if (!config) return;
    
    Logger.logLinef("Device: %s", config->device_name);
    
    // Show sanitized name for mDNS
    char sanitized[CONFIG_DEVICE_NAME_MAX_LEN];
    config_manager_sanitize_device_name(config->device_name, sanitized, CONFIG_DEVICE_NAME_MAX_LEN);
    Logger.logLinef("mDNS: %s.local", sanitized);
    
    Logger.logLinef("WiFi SSID: %s", config->wifi_ssid);
    Logger.logLinef("WiFi Pass: %s", strlen(config->wifi_password) > 0 ? "***" : "(none)");
    
    if (strlen(config->fixed_ip) > 0) {
        Logger.logLinef("IP: %s", config->fixed_ip);
        Logger.logLinef("Subnet: %s", config->subnet_mask);
        Logger.logLinef("Gateway: %s", config->gateway);
        Logger.logLinef("DNS: %s, %s", config->dns1, strlen(config->dns2) > 0 ? config->dns2 : "(none)");
    } else {
        Logger.logLine("IP: DHCP");
    }
}
