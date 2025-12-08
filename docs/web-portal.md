# Web Configuration Portal

The ESP32 template includes a full-featured web portal for device configuration, monitoring, and firmware updates. The portal uses an async web server with captive portal support for initial setup.

## Overview

The web portal provides:
- WiFi configuration via captive portal
- Real-time device health monitoring
- Over-the-air (OTA) firmware updates
- REST API for programmatic access
- Responsive web interface (desktop & mobile)

## Portal Modes

### Core Mode (AP with Captive Portal)

**When it runs:**
- WiFi credentials not configured
- Configuration reset
- WiFi connection failed

**Access:**
- SSID: `ESP32-XXXXXX` (where XXXXXX is chip ID)
- IP: `http://192.168.4.1`
- Captive portal auto-redirects to configuration page

**Features:**
- WiFi SSID and password setup
- Device name configuration
- Fixed IP settings (optional)

### Full Mode (WiFi Connected)

**When it runs:**
- Device successfully connected to WiFi network

**Access:**
- Device IP address (displayed in serial monitor)
- mDNS hostname: `http://<device-name>.local`

**Features:**
- All Core Mode features
- Real-time health monitoring
- OTA firmware updates
- Device reboot

## Device Discovery

When connected to WiFi, devices can be discovered using multiple methods:

### WiFi Hostname (DHCP)

The device sets its WiFi hostname using `WiFi.setHostname()` before connecting. This hostname appears in:
- Router DHCP client lists
- Network scanning tools (Fing, WiFiMan, etc.)
- DHCP server logs

**Format:** Sanitized device name (e.g., "ESP32 1234" → `esp32-1234`)

### mDNS (Bonjour)

The device advertises itself via mDNS with enhanced service records:

**Access:** `http://<hostname>.local` (e.g., `http://esp32-1234.local`)

**Platforms:**
- ✅ macOS (native support)
- ✅ Linux (requires `avahi-daemon`)
- ✅ iOS/iPadOS (native support)
- ✅ Android (native support in modern versions)
- ⚠️ Windows (requires Bonjour service)

**Service TXT Records:**
- `version` - Firmware version
- `model` - Chip model (ESP32/ESP32-C3/etc)
- `mac` - Last 4 hex digits of MAC address
- `ty` - Device type ("iot-device")
- `mf` - Manufacturer ("ESP32-Tmpl")
- `features` - Capabilities ("wifi,http,api")
- `note` - Description ("WiFi Portal Device")
- `url` - Configuration URL ("http://hostname.local")

**Discovery Example:**
```bash
# macOS/Linux
avahi-browse -rt _http._tcp

# Output includes TXT records:
# esp32-1234._http._tcp
#   hostname = [esp32-1234.local]
#   txt = ["version=0.0.1" "model=ESP32-C3" "mac=1234" "ty=iot-device" 
#          "mf=ESP32-Tmpl" "features=wifi,http,api" "note=WiFi Portal Device" 
#          "url=http://esp32-1234.local"]
```

### Router Discovery

Most routers display connected devices with their hostnames:
- Check router web interface → DHCP clients
- Look for device name (e.g., "esp32-1234")
- Note the assigned IP address

### Network Scanning Apps

Recommended apps for finding devices:
- **Fing** (iOS/Android) - Shows hostname and MAC
- **WiFiMan** (iOS/Android) - Network scanner
- **Angry IP Scanner** (Desktop) - Fast network scanner
- **nmap** (Linux/macOS) - Command-line scanner

## User Interface

### Header Badges

The portal displays 6 real-time device capability badges:

| Badge | Color | Example | Description |
|-------|-------|---------|-------------|
| Firmware | Purple | `Firmware v0.0.1` | Firmware version |
| Chip | Orange | `ESP32-C3 rev 4` | Chip model and revision |
| Cores | Green | `1 Core` / `2 Cores` | Number of CPU cores |
| Frequency | Yellow | `160 MHz` | CPU frequency |
| Flash | Cyan | `4 MB Flash` | Flash memory size |
| PSRAM | Teal | `No PSRAM` / `2 MB PSRAM` | PSRAM status |

### Health Monitoring Widget

Floating status widget with compact and expanded views:

**Compact View:**
- Green breathing dot (pulses on updates)
- Current CPU usage percentage
- Click to expand

**Expanded View (11 metrics):**
- **Uptime**: Device runtime
- **Reset Reason**: Why device last restarted
- **CPU Usage**: Percentage based on IDLE task
- **Core Temp**: Internal temperature sensor (ESP32-C3/S2/S3/C2/C6/H2)
- **Free Heap**: Available RAM
- **Min Heap**: Minimum free heap since boot
- **Fragmentation**: Heap fragmentation percentage
- **Flash Usage**: Used firmware space
- **RSSI**: WiFi signal strength (when connected)
- **IP Address**: Current IP (when connected)

**Update Intervals:**
- Compact: 10 seconds
- Expanded: 5 seconds

### Configuration Sections

- **📶 WiFi Settings**: SSID, password, fixed IP configuration
- **🔧 Device Settings**: Device name (used for mDNS hostname)
- **🌐 Network Settings**: Fixed IP, subnet, gateway, DNS servers
- **⚙️ Additional Settings**: Custom application settings
- **📦 Firmware Update**: OTA binary upload (Full Mode only)

## Automatic Reconnection After Reboot

When the device reboots (after saving settings, firmware update, or manual reboot), the portal automatically attempts to reconnect and redirect you to the device.

### Reconnection Behavior

**Unified Dialog:**
All reboot scenarios (save config, OTA update, manual reboot) use a single dialog that:
- Shows current operation status
- Displays best-effort automatic reconnection messages
- Provides manual fallback address immediately
- Updates with real-time connection attempts

**Polling Strategy:**
- **Initial delay:** 2 seconds (device starts rebooting)
- **Polling interval:** 3 seconds between connection checks
- **Total timeout:** 122 seconds (2s initial + 40 attempts × 3s)
- **Endpoint used:** `/api/info` (lightweight health check)

**Progress Display:**
```
Checking connection (attempt 5/40, 17s elapsed)...
```

### Scenarios

#### Config Save / Manual Reboot
1. Dialog shows: *"Configuration saved. Device is rebooting..."*
2. Displays: *"Attempting automatic reconnection..."*
3. Shows manual fallback: *"If this fails, manually navigate to: http://device-name.local"*
4. Polls both new hostname (if device name changed) and current location
5. On success: Redirects automatically
6. On timeout: Provides clickable manual link with troubleshooting hints

#### OTA Firmware Update
1. Progress bar shows upload (0-100%)
2. At 95%+: *"Installing firmware & rebooting..."*
3. Transitions to reconnection polling after 2s delay
4. Same polling behavior as config save
5. Redirects to same location (firmware update doesn't change hostname)

#### Factory Reset
1. Dialog shows: *"Configuration reset. Device restarting in AP mode..."*
2. Message: *"You must manually reconnect to the WiFi access point"*
3. **No automatic polling** (user must manually reconnect to AP)
4. Dialog remains visible with instructions

### Timeout Handling

If reconnection fails after 122 seconds:

```
Automatic reconnection failed

Could not reconnect after 122 seconds.

Please manually navigate to:
http://device-name.local

Possible issues: WiFi connection failed, incorrect credentials, 
or device taking longer to boot.
```

### Best Practices

**For Users:**
- Keep the browser tab open during reboot (don't close immediately)
- If automatic reconnection fails, use the provided manual link
- For device name changes, bookmark the new address
- On AP mode reset, reconnect to the WiFi access point before accessing portal

**For Developers:**
- Automatic reconnection is best-effort (network conditions vary)
- Always provide manual fallback addresses
- DNS propagation for hostname changes may take additional time
- Some networks/browsers block cross-origin polling

## REST API Reference

All endpoints return JSON responses with proper HTTP status codes.

### Device Information

#### `GET /api/info`

Returns comprehensive device information.

**Response:**
```json
{
  "version": "0.0.1",
  "build_date": "Nov 25 2025",
  "build_time": "14:30:00",
  "chip_model": "ESP32-C3",
  "chip_revision": 4,
  "chip_cores": 1,
  "cpu_freq": 160,
  "flash_chip_size": 4194304,
  "psram_size": 0,
  "free_heap": 250000,
  "sketch_size": 1048576,
  "free_sketch_space": 2097152,
  "mac_address": "AA:BB:CC:DD:EE:FF",
  "wifi_hostname": "esp32-1234",
  "mdns_name": "esp32-1234.local",
  "hostname": "esp32-1234"
}
```

**Discovery Fields:**
- `mac_address`: Device MAC address
- `wifi_hostname`: WiFi/DHCP hostname
- `mdns_name`: Full mDNS name (hostname + `.local`)
- `hostname`: Short hostname

### Health Monitoring

#### `GET /api/health`

Returns real-time device health statistics.

**Response:**
```json
{
  "uptime_seconds": 3600,
  "reset_reason": "Power On",
  "cpu_usage": 15,
  "cpu_freq": 160,
  "temperature": 42,
  "heap_free": 250000,
  "heap_min": 240000,
  "heap_size": 327680,
  "heap_fragmentation": 5,
  "flash_used": 1048576,
  "flash_total": 3145728,
  "wifi_rssi": -45,
  "wifi_channel": 6,
  "ip_address": "192.168.1.100",
  "hostname": "esp32-1234"
}
```

**Notes:**
- `temperature`: `null` on chips without internal sensor (original ESP32)
- `wifi_rssi`, `wifi_channel`, `ip_address`, `hostname`: `null` when not connected

### Configuration Management

#### `GET /api/config`

Returns current device configuration (passwords excluded).

**Response:**
```json
{
  "wifi_ssid": "MyNetwork",
  "wifi_password": "",
  "device_name": "esp32-device",
  "device_name_sanitized": "esp32-device",
  "fixed_ip": "",
  "subnet_mask": "",
  "gateway": "",
  "dns1": "",
  "dns2": "",
  "dummy_setting": ""
}
```

#### `POST /api/config`

Save new configuration. Device reboots after successful save.

**Request Body:**
```json
{
  "wifi_ssid": "NewNetwork",
  "wifi_password": "password123",
  "device_name": "my-esp32",
  "fixed_ip": "192.168.1.100",
  "subnet_mask": "255.255.255.0",
  "gateway": "192.168.1.1",
  "dns1": "8.8.8.8",
  "dns2": "8.8.4.4",
  "dummy_setting": "value"
}
```

**Response:**
```json
{
  "success": true,
  "message": "Configuration saved"
}
```

**Notes:**
- Only fields present in request are updated
- Password field: empty string = no change, non-empty = update
- Device automatically reboots after successful save
- Web portal automatically polls for reconnection (see [Automatic Reconnection](#automatic-reconnection-after-reboot))

#### `DELETE /api/config`

Reset configuration to factory defaults. Device reboots after reset.

**Response:**
```json
{
  "success": true,
  "message": "Configuration reset"
}
```

**Notes:**
- Device reboots into AP mode after reset
- **No automatic reconnection** - user must manually reconnect to WiFi access point

### Portal Mode

#### `GET /api/mode`

Returns current portal operating mode.

**Response:**
```json
{
  "mode": "core",
  "ap_active": true
}
```

**Modes:**
- `"core"`: AP mode with captive portal
- `"full"`: WiFi connected mode

### System Control

#### `POST /api/reboot`

Reboot the device without saving configuration changes.

**Response:**
```json
{
  "success": true,
  "message": "Rebooting device..."
}
```

**Notes:**
- Web portal automatically polls for reconnection (see [Automatic Reconnection](#automatic-reconnection-after-reboot))

### OTA Firmware Update

#### `POST /api/update`

Upload new firmware binary for over-the-air update.

**Request:**
- Content-Type: `multipart/form-data`
- File field: firmware `.bin` file

**Response (Success):**
```json
{
  "success": true,
  "message": "Update successful! Rebooting..."
}
```

**Response (Error):**
```json
{
  "success": false,
  "message": "Error description"
}
```

**Notes:**
- Only `.bin` files accepted
- File size must fit in OTA partition
- Device automatically reboots after successful update
- Progress logged to serial monitor
- Web portal shows upload progress bar, then automatically polls for reconnection (see [Automatic Reconnection](#automatic-reconnection-after-reboot))

## Implementation Details

### Architecture

**Backend (C++):**
- `web_portal.cpp/h` - ESPAsyncWebServer with REST endpoints
- `config_manager.cpp/h` - NVS (Non-Volatile Storage) for configuration
- `web_assets.cpp/h` - PROGMEM embedded HTML/CSS/JS (gzip compressed)
- `log_manager.cpp/h` - Print-compatible logging with nested blocks (serial output only)

**Frontend (HTML/CSS/JS):**
- `web/portal.html` - Semantic HTML structure
- `web/portal.css` - Minimalist card-based design with gradients
- `web/portal.js` - Vanilla JavaScript (no frameworks)

**Asset Compression:**
- All web assets are automatically minified and gzip compressed during build
- Reduces flash storage and bandwidth by ~80%
- Assets served with `Content-Encoding: gzip` header
- Browser automatically decompresses (transparent to user)

### CPU Usage Calculation

CPU usage is calculated using FreeRTOS IDLE task monitoring:

```cpp
TaskStatus_t task_stats[16];
uint32_t total_runtime;
int task_count = uxTaskGetSystemState(task_stats, 16, &total_runtime);

uint32_t idle_runtime = 0;
for (int i = 0; i < task_count; i++) {
    if (strstr(task_stats[i].pcTaskName, "IDLE") != nullptr) {
        idle_runtime += task_stats[i].ulRunTimeCounter;
    }
}

float cpu_percent = 100.0 - ((float)idle_runtime / total_runtime) * 100.0;
```

### Temperature Sensor

Internal temperature sensor is available on newer ESP32 variants:
- ESP32-C3, ESP32-S2, ESP32-S3, ESP32-C2, ESP32-C6, ESP32-H2

Code uses compile-time detection:
```cpp
#if SOC_TEMP_SENSOR_SUPPORTED
    // Use driver/temperature_sensor.h
#else
    // Return null for original ESP32
#endif
```

### Configuration Storage

Device configuration is stored in NVS (Non-Volatile Storage):
- Namespace: `device_config`
- Survives reboots and power cycles
- Factory reset available via REST API or button (if implemented)

### Captive Portal

DNS server redirects all requests to device IP in AP mode:
- Listens on port 53
- Wildcard DNS: `*` → `192.168.4.1`
- Works with most mobile OS captive portal detection

## Development Workflow

### Modifying the Web Interface

1. Edit files in `src/app/web/`:
   - `portal.html` - Structure
   - `portal.css` - Styling
   - `portal.js` - Client logic

2. Rebuild to embed assets:
   ```bash
   ./build.sh
   ```
   
   This automatically:
   - Minifies HTML (removes comments, collapses whitespace)
   - Minifies CSS using `csscompressor`
   - Minifies JavaScript using `rjsmin`
   - Gzip compresses all assets (level 9)
   - Generates `web_assets.h` with embedded byte arrays
   
   The build script shows compression statistics:
   ```
   Asset Summary (Original → Minified → Gzipped):
     HTML portal.html: 11695 → 8261 → 2399 bytes (-80% total)
     CSS  portal.css:  14348 → 10539 → 2864 bytes (-81% total)
     JS   portal.js:   32032 → 19700 → 4931 bytes (-85% total)
   ```

3. Upload and test:
   ```bash
   ./upload.sh
   ./monitor.sh
   ```

### Adding REST Endpoints

1. Add handler function in `web_portal.cpp`:
   ```cpp
   void handleNewEndpoint(AsyncWebServerRequest *request) {
       JsonDocument doc;
       doc["data"] = "value";
       String response;
       serializeJson(doc, response);
       request->send(200, "application/json", response);
   }
   ```

2. Register route in `web_portal_init()`:
   ```cpp
   server->on("/api/newpoint", HTTP_GET, handleNewEndpoint);
   ```

3. Update `web_portal.h` documentation comments

4. Document in this file (REST API section)

### Testing Portal Modes

**Test Core Mode:**
```bash
# Reset config to trigger AP mode
curl -X DELETE http://192.168.1.100/api/config
# Or erase flash
./upload-erase.sh
./upload.sh
```

**Test Full Mode:**
```bash
# Configure WiFi via portal, then access at device IP
curl http://192.168.1.100/api/info
```

## Troubleshooting

### Cannot Access Portal in AP Mode

- Check SSID: Should be `ESP32-XXXXXX`
- IP address: Always `192.168.4.1`
- Disable mobile data (can interfere with captive portal)

### Portal Not Responding After WiFi Config

- Check serial monitor for IP address
- Verify WiFi credentials are correct
- Check router DHCP settings
- Use fixed IP if DHCP fails

### WiFi Connection Issues After OTA or Reboot

If WiFi fails to connect after firmware update or reboot:
- Device has automatic reconnection with 10-second watchdog
- Hardware reset sequence clears stale WiFi state on each connection attempt
- Check serial logs for detailed connection status (SSID not found, wrong password, etc.)
- If persistent, use physical reset button to fully power-cycle WiFi hardware
- Auto-reconnect and event handlers ensure recovery from temporary drops

### OTA Update Fails

- Verify `.bin` file (not `.elf` or other format)
- Check file size vs available OTA partition space
- Ensure stable power supply during update
- Monitor serial output for error details

### Health Monitoring Shows "N/A"

- **Temperature N/A**: Normal on original ESP32 (no internal sensor)
- **WiFi stats N/A**: Normal when not connected to WiFi
- **CPU 0%**: Check FreeRTOS configuration

### Cannot Connect to mDNS Hostname

- Windows: Install Bonjour service
- Linux: Install `avahi-daemon`
- Some networks block mDNS (use IP instead)

## Security Considerations

**Current Implementation:**
- No authentication required
- Suitable for local/trusted networks only
- Configuration API accessible to all clients

**Production Recommendations:**
- Add HTTP Basic Auth or API keys
- Use HTTPS with self-signed certificates
- Implement rate limiting on sensitive endpoints
- Add CSRF protection for POST/DELETE operations
- Whitelist allowed WiFi SSIDs (prevent evil twin attacks)

## Related Documentation

- [Script Reference](scripts.md) - Build and upload workflows
- [Library Management](library-management.md) - Adding dependencies
- [WSL Development](wsl-development.md) - Windows development setup
