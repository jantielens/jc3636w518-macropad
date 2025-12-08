#include "../version.h"
#include "board_config.h"
#include "config_manager.h"
#include "web_portal.h"
#include "log_manager.h"
#include <WiFi.h>
#include <ESPmDNS.h>
#include <lwip/netif.h>

// Configuration
DeviceConfig device_config;
bool config_loaded = false;

// WiFi retry settings
const unsigned long WIFI_BACKOFF_BASE = 3000; // 3 seconds base (DHCP typically needs 2-3s)

// Heartbeat interval
const unsigned long HEARTBEAT_INTERVAL = 60000; // 60 seconds
unsigned long lastHeartbeat = 0;

// WiFi watchdog for connection monitoring
const unsigned long WIFI_CHECK_INTERVAL = 10000; // 10 seconds
unsigned long lastWiFiCheck = 0;

// WiFi event handlers for connection lifecycle monitoring
void onWiFiConnected(WiFiEvent_t event, WiFiEventInfo_t info) {
  Logger.logMessage("WiFi", "Connected to AP - waiting for IP");
}

void onWiFiGotIP(WiFiEvent_t event, WiFiEventInfo_t info) {
  Logger.logMessagef("WiFi", "Got IP: %s", WiFi.localIP().toString().c_str());
}

void onWiFiDisconnected(WiFiEvent_t event, WiFiEventInfo_t info) {
  uint8_t reason = info.wifi_sta_disconnected.reason;
  Logger.logMessagef("WiFi", "Disconnected - reason: %d", reason);
  
  // Common disconnect reasons:
  // 2 = AUTH_EXPIRE, 3 = AUTH_LEAVE, 4 = ASSOC_EXPIRE
  // 8 = ASSOC_LEAVE, 15 = 4WAY_HANDSHAKE_TIMEOUT
  // 201 = NO_AP_FOUND, 202 = AUTH_FAIL, 205 = HANDSHAKE_TIMEOUT
}

void setup()
{
  // Initialize log manager (wraps Serial for web streaming)
  Logger.begin(115200);
  delay(1000);
  
  // Register WiFi event handlers for connection lifecycle
  WiFi.onEvent(onWiFiConnected, ARDUINO_EVENT_WIFI_STA_CONNECTED);
  WiFi.onEvent(onWiFiGotIP, ARDUINO_EVENT_WIFI_STA_GOT_IP);
  WiFi.onEvent(onWiFiDisconnected, ARDUINO_EVENT_WIFI_STA_DISCONNECTED);
  
  Logger.logBegin("System Boot");
  Logger.logLinef("Firmware: v%s", FIRMWARE_VERSION);
  Logger.logLinef("Chip: %s (Rev %d)", ESP.getChipModel(), ESP.getChipRevision());
  Logger.logLinef("CPU: %d MHz", ESP.getCpuFreqMHz());
  Logger.logLinef("Flash: %d MB", ESP.getFlashChipSize() / (1024 * 1024));
  Logger.logLinef("MAC: %s", WiFi.macAddress().c_str());
  #if HAS_BUILTIN_LED
  Logger.logLinef("LED: GPIO%d (active %s)", LED_PIN, LED_ACTIVE_HIGH ? "HIGH" : "LOW");
  #endif
  // Example: Call board-specific function if available
  // #ifdef HAS_CUSTOM_IDENTIFIER
  // Logger.logLinef("Board: %s", board_get_custom_identifier());
  // #endif
  Logger.logEnd();
  
  // Initialize board-specific hardware
  #if HAS_BUILTIN_LED
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LED_ACTIVE_HIGH ? LOW : HIGH); // LED off initially
  #endif
  
  // Initialize configuration manager
  config_manager_init();
  
  // Try to load saved configuration
  config_loaded = config_manager_load(&device_config);
  
  if (!config_loaded) {
    // No config found - set default device name
    String default_name = config_manager_get_default_device_name();
    strlcpy(device_config.device_name, default_name.c_str(), CONFIG_DEVICE_NAME_MAX_LEN);
    device_config.magic = CONFIG_MAGIC;
  }
  
  // Start WiFi BEFORE initializing web server (critical for ESP32-C3)
  if (!config_loaded) {
    Logger.logMessage("Main", "No config - starting AP mode");
    web_portal_start_ap();
  } else {
    Logger.logMessage("Main", "Config loaded - connecting to WiFi");
    if (connect_wifi()) {
      start_mdns();
    } else {
      // Hard reset retry - WiFi hardware may be in bad state
      Logger.logMessage("Main", "WiFi failed - attempting hard reset");
      Logger.logBegin("WiFi Hard Reset");
      WiFi.mode(WIFI_OFF);
      delay(1000);  // Longer delay to fully reset hardware
      WiFi.mode(WIFI_STA);
      delay(500);
      Logger.logEnd("Reset complete");
      
      // One more attempt after hard reset
      if (connect_wifi()) {
        start_mdns();
      } else {
        Logger.logMessage("Main", "WiFi failed after reset - fallback to AP");
        web_portal_start_ap();
      }
    }
  }
  
  // Initialize web portal AFTER WiFi is started
  web_portal_init(&device_config);
  
  lastHeartbeat = millis();
  Logger.logMessage("Main", "Setup complete");
}

void loop()
{
  // Handle web portal (DNS for captive portal)
  web_portal_handle();
  
  unsigned long currentMillis = millis();
  
  // WiFi watchdog - monitor connection and reconnect if needed
  // Only run if we're not in AP mode (AP mode is the fallback, should stay active)
  if (config_loaded && !web_portal_is_ap_mode() && currentMillis - lastWiFiCheck >= WIFI_CHECK_INTERVAL) {
    if (WiFi.status() != WL_CONNECTED && strlen(device_config.wifi_ssid) > 0) {
      Logger.logMessage("WiFi Watchdog", "Connection lost - attempting reconnect");
      if (connect_wifi()) {
        start_mdns();
      }
    }
    lastWiFiCheck = currentMillis;
  }
  
  // Check if it's time for heartbeat
  if (currentMillis - lastHeartbeat >= HEARTBEAT_INTERVAL) {
    if (WiFi.status() == WL_CONNECTED) {
      Logger.logQuickf("Heartbeat", "Up: %ds | Heap: %d | WiFi: %s (%s)", 
        currentMillis / 1000, ESP.getFreeHeap(), 
        WiFi.localIP().toString().c_str(), WiFi.getHostname());
    } else {
      Logger.logQuickf("Heartbeat", "Up: %ds | Heap: %d | WiFi: Disconnected", 
        currentMillis / 1000, ESP.getFreeHeap());
    }
    
    lastHeartbeat = currentMillis;
  }
  
  delay(10);
}

// Connect to WiFi with exponential backoff
bool connect_wifi() {
  Logger.logBegin("WiFi Connection");
  Logger.logLinef("SSID: %s", device_config.wifi_ssid);
  
  // === WiFi Hardware Reset Sequence ===
  // Disable persistent WiFi config (we manage our own via NVS)
  WiFi.persistent(false);
  
  // Full WiFi reset to clear stale state and prevent hardware corruption
  WiFi.disconnect(true);  // Disconnect + erase stored credentials
  delay(100);
  WiFi.mode(WIFI_OFF);    // Turn off WiFi hardware
  delay(500);             // Wait for hardware to settle
  WiFi.mode(WIFI_STA);    // Back to station mode
  delay(100);
  
  // Enable auto-reconnect at WiFi stack level
  WiFi.setAutoReconnect(true);
  
  // Prepare sanitized hostname
  char sanitized[CONFIG_DEVICE_NAME_MAX_LEN];
  config_manager_sanitize_device_name(device_config.device_name, sanitized, CONFIG_DEVICE_NAME_MAX_LEN);
  
  // Set WiFi hostname for mDNS and internal use
  // NOTE: ESP32's lwIP stack has limited DHCP Option 12 (hostname) support
  // The hostname may not always appear in router DHCP tables due to ESP-IDF/lwIP limitations
  // Use mDNS (.local) or NetBIOS for reliable device discovery instead
  if (strlen(sanitized) > 0) {
    // Set via WiFi library
    WiFi.setHostname(sanitized);
    Logger.logLinef("Hostname: %s", sanitized);
    
    // Also set via esp_netif API (for compatibility)
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (netif != NULL) {
      esp_netif_set_hostname(netif, sanitized);
    }
  }
  
  // Configure fixed IP if provided
  if (strlen(device_config.fixed_ip) > 0) {
    Logger.logBegin("Fixed IP Config");
    
    IPAddress local_ip, gateway, subnet, dns1, dns2;
    
    if (!local_ip.fromString(device_config.fixed_ip)) {
      Logger.logEnd("Invalid IP address");
      Logger.logEnd("Connection failed");
      return false;
    }
    
    if (!subnet.fromString(device_config.subnet_mask)) {
      Logger.logEnd("Invalid subnet mask");
      Logger.logEnd("Connection failed");
      return false;
    }
    
    if (!gateway.fromString(device_config.gateway)) {
      Logger.logEnd("Invalid gateway");
      Logger.logEnd("Connection failed");
      return false;
    }
    
    // DNS1: use provided, or default to gateway
    if (strlen(device_config.dns1) > 0) {
      dns1.fromString(device_config.dns1);
    } else {
      dns1 = gateway;
    }
    
    // DNS2: optional
    if (strlen(device_config.dns2) > 0) {
      dns2.fromString(device_config.dns2);
    } else {
      dns2 = IPAddress(0, 0, 0, 0);
    }
    
    if (!WiFi.config(local_ip, gateway, subnet, dns1, dns2)) {
      Logger.logEnd("Configuration failed");
      Logger.logEnd("Connection failed");
      return false;
    }
    
    Logger.logLinef("IP: %s", device_config.fixed_ip);
    Logger.logEnd();
  }
  
  WiFi.begin(device_config.wifi_ssid, device_config.wifi_password);
  
  // Try to connect with exponential backoff
  for (int attempt = 0; attempt < WIFI_MAX_ATTEMPTS; attempt++) {
    unsigned long backoff = WIFI_BACKOFF_BASE * (attempt + 1);
    unsigned long start = millis();
    
    Logger.logLinef("Attempt %d/%d (timeout %ds)", attempt + 1, WIFI_MAX_ATTEMPTS, backoff / 1000);
    
    while (millis() - start < backoff) {
      wl_status_t status = WiFi.status();
      if (status == WL_CONNECTED) {
        Logger.logLinef("IP: %s", WiFi.localIP().toString().c_str());
        Logger.logLinef("Hostname: %s", WiFi.getHostname());
        Logger.logLinef("MAC: %s", WiFi.macAddress().c_str());
        Logger.logLinef("Signal: %d dBm", WiFi.RSSI());
        Logger.logLine("");
        Logger.logLine("Access via:");
        Logger.logLinef("  http://%s", WiFi.localIP().toString().c_str());
        Logger.logLinef("  http://%s.local", WiFi.getHostname());
        Logger.logEnd("Connected");
        
        return true;
      }
      delay(100);
    }
    
    // Log detailed failure reason for diagnostics
    wl_status_t status = WiFi.status();
    if (status != WL_CONNECTED) {
      const char* reason = 
        (status == WL_NO_SSID_AVAIL) ? "SSID not found" :
        (status == WL_CONNECT_FAILED) ? "Connect failed (wrong password?)" :
        (status == WL_CONNECTION_LOST) ? "Connection lost" :
        (status == WL_DISCONNECTED) ? "Disconnected" :
        "Unknown";
      Logger.logLinef("Status: %s (%d)", reason, status);
    }
  }
  
  Logger.logEnd("All attempts failed");
  return false;
}

// Start mDNS service with enhanced TXT records
void start_mdns() {
  Logger.logBegin("mDNS");
  
  char sanitized[CONFIG_DEVICE_NAME_MAX_LEN];
  config_manager_sanitize_device_name(device_config.device_name, sanitized, CONFIG_DEVICE_NAME_MAX_LEN);
  
  if (strlen(sanitized) == 0) {
    Logger.logEnd("Empty hostname");
    return;
  }
  
  if (MDNS.begin(sanitized)) {
    Logger.logLinef("Name: %s.local", sanitized);
    
    // Add HTTP service
    MDNS.addService("http", "tcp", 80);
    
    // Add TXT records with device metadata (per RFC 6763)
    // Keep keys ≤9 chars, total TXT record <400 bytes
    
    // Core device identification
    MDNS.addServiceTxt("http", "tcp", "version", FIRMWARE_VERSION);
    MDNS.addServiceTxt("http", "tcp", "model", ESP.getChipModel());
    
    // MAC address (last 4 hex digits for identification)
    String mac = WiFi.macAddress();
    mac.replace(":", "");
    String mac_short = mac.substring(mac.length() - 4);
    MDNS.addServiceTxt("http", "tcp", "mac", mac_short.c_str());
    
    // Device type and manufacturer
    MDNS.addServiceTxt("http", "tcp", "ty", "iot-device");
    MDNS.addServiceTxt("http", "tcp", "mf", "ESP32-Tmpl");
    
    // Capabilities
    MDNS.addServiceTxt("http", "tcp", "features", "wifi,http,api");
    
    // User-friendly description
    MDNS.addServiceTxt("http", "tcp", "note", "WiFi Portal Device");
    
    // Configuration URL
    String config_url = "http://";
    config_url += sanitized;
    config_url += ".local";
    MDNS.addServiceTxt("http", "tcp", "url", config_url.c_str());
    
    Logger.logLine("TXT records: version, model, mac, ty, features");
    Logger.logEnd();
  } else {
    Logger.logEnd("Failed to start");
  }
}