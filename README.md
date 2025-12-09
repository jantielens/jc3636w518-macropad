# JC3636W518 Macropad

ESP32-S3 round display macropad with WiFi configuration portal and LVGL GUI.

## ✨ Features
- **360x360 Round Display**: ST77916 LCD with capacitive touch (CST816S)
- **WiFi Configuration Portal**: Web-based setup with captive portal
- **LVGL GUI**: Splash screen with boot status and connection details
- **REST API**: Full device control and configuration
- **OTA Updates**: Firmware updates over WiFi
- **mDNS Support**: Access device via `http://[hostname].local`

## 🚀 Quick Start

### First-time Setup
```bash
./setup.sh              # Install arduino-cli, ESP32 core, and libraries
```

### Build, Upload, Monitor
```bash
./build.sh jc3636w518   # Compile firmware
./upload.sh jc3636w518  # Upload to device
./monitor.sh            # View serial output

# Or use convenience scripts:
./bum.sh jc3636w518     # Build + Upload + Monitor
./um.sh jc3636w518      # Upload + Monitor
```

### Hardware Requirements
- **Board**: JC3636W518 (ESP32-S3 with 360x360 round display)
- **Display**: ST77916 LCD (QSPI interface)
- **Touch**: CST816S capacitive touch controller (I2C)
- **Memory**: 16MB Flash, 8MB PSRAM
- **Connection**: USB-C (CDC serial)

## 📁 Project Structure

```
jc3636w518-macropad/
├── bin/                           # Local arduino-cli installation
├── build/
│   └── jc3636w518/                # Compiled firmware binaries
├── docs/                          # Documentation
│   ├── scripts.md                 # Script usage guide
│   ├── web-portal.md              # Web portal & REST API guide
│   ├── wsl-development.md         # WSL/USB setup
│   └── library-management.md      # Library management
├── src/
│   ├── app/
│   │   ├── app.ino                # Main sketch file
│   │   ├── board_config.h         # Default board configuration
│   │   ├── lv_conf.h              # LVGL configuration
│   │   ├── config_manager.cpp/h   # NVS config storage
│   │   ├── log_manager.cpp/h      # Serial logging with web streaming
│   │   ├── web_portal.cpp/h       # Web server & REST API
│   │   ├── web_assets.h           # Embedded HTML/CSS/JS (auto-generated)
│   │   ├── ui/                    # LVGL UI framework
│   │   │   ├── base_screen.h/cpp      # Abstract screen base class
│   │   │   ├── screen_manager.h/cpp   # Screen navigation
│   │   │   ├── ui_events.h/cpp        # FreeRTOS event system
│   │   │   └── screens/
│   │   │       └── splash_screen.h/cpp # Boot splash screen
│   │   └── web/                   # Web portal sources
│   │       ├── index.html             # ⚠️ AUTO-GENERATED (DO NOT EDIT)
│   │       ├── network.html           # ⚠️ AUTO-GENERATED (DO NOT EDIT)
│   │       ├── update.html            # ⚠️ AUTO-GENERATED (DO NOT EDIT)
│   │       ├── portal.css             # ✏️ Editable: Shared styles
│   │       ├── portal.js              # ✏️ Editable: Shared client logic
│   │       ├── shared/                # ✏️ Editable: Reusable components
│   │       │   ├── header.html        # Page header with badges
│   │       │   ├── nav.html           # Navigation tabs
│   │       │   ├── footer.html        # Page footer
│   │       │   ├── health-widget.html # Health monitoring widget
│   │       │   └── reboot-overlay.html# Reboot/reconnection dialogs
│   │       └── pages/                 # ✏️ Editable: Page-specific content
│   │           ├── index-content.html # Macropad settings form
│   │           ├── network-content.html# Network settings form
│   │           └── update-content.html# OTA upload & factory reset
│   ├── boards/
│   │   └── jc3636w518/            # JC3636W518-specific configuration
│   │       ├── board_config.h         # Display dimensions, flags
│   │       ├── board_config.cpp       # Board implementation stub
│   │       ├── pincfg.h               # Pin definitions (TFT, touch, SD, audio)
│   │       ├── display_driver.h       # Display API interface
│   │       └── display_driver.cpp     # ST77916 LCD + CST816S touch driver
│   └── version.h                  # Firmware version tracking
├── tools/
│   ├── build-html-pages.sh        # Assemble HTML from components
│   ├── minify-web-assets.sh       # Web asset minification & embedding
│   └── extract-changelog.sh       # Extract release notes
├── .github/
│   └── workflows/
│       └── build.yml              # CI/CD pipeline
├── config.sh                      # Project configuration & board definitions
├── build.sh                       # Compile sketch
├── upload.sh                      # Upload firmware
├── upload-erase.sh                # Erase flash memory
├── monitor.sh                     # Serial monitor
├── clean.sh                       # Clean build artifacts
├── library.sh                     # Library management
├── bum.sh / um.sh                 # Convenience scripts
├── setup.sh                       # Environment setup
└── arduino-libraries.txt          # Library dependencies
```

## 🎨 Display & UI

The project uses LVGL 8.3.11 for the GUI framework:

**Current Screens:**
- **Splash Screen**: Shows firmware version, boot status, WiFi connection details (IP + mDNS hostname)
- **MacroPad Screen**: _(To be implemented)_

**Display Specifications:**
- **Resolution**: 360x360 pixels
- **Controller**: ST77916 (QSPI interface at 50MHz)
- **Touch**: CST816S capacitive touch
- **Backlight**: PWM-controlled (GPIO 15)
- **Draw Buffer**: PSRAM-backed (16 rows, fallback to 8/4)


## 🌐 Web Portal & REST API

### ⚠️ CRITICAL: HTML File Generation

**DO NOT directly edit these files** - they are auto-generated during build:
- `src/app/web/index.html`
- `src/app/web/network.html`
- `src/app/web/update.html`

**Instead, edit the template files:**
- `src/app/web/shared/` - Header, nav, footer, widgets (used by all pages)
- `src/app/web/pages/` - Page-specific content forms
- `src/app/web/portal.css` - Styles (editable)
- `src/app/web/portal.js` - Client-side logic (editable)

The build process runs `tools/build-html-pages.sh` which assembles complete HTML files from templates.

### Portal Access

Access the configuration portal at:
- **AP Mode** (no WiFi configured): `http://192.168.4.1`
- **Connected Mode**: `http://[device-ip]` or `http://[hostname].local`

### Multi-Page Portal

The portal has 3 pages with consistent navigation:

| Page | URL | Purpose |
|------|-----|----------|
| **Macropad** | `/` | Macropad-specific settings |
| **Network** | `/network.html` | WiFi, device name, IP configuration |
| **Update** | `/update.html` | OTA firmware updates, factory reset |

**Standardized Controls:**
All configuration pages include:
- **Save & Reboot** - Save changes and restart device
- **Save Only** - Save without restarting (instant apply)
- **Reboot** - Restart without saving changes

### REST API Endpoints

| Method | Endpoint | Query Params | Purpose |
|--------|----------|--------------|----------|
| GET | `/api/info` | - | Device info (firmware, chip, cores, flash, PSRAM, hostname, MAC) |
| GET | `/api/health` | - | Real-time health stats (CPU, memory, WiFi, uptime, hostname) |
| GET | `/api/config` | - | Current configuration |
| POST | `/api/config` | `?reboot=true/false` | Save configuration (reboot optional) |
| DELETE | `/api/config` | - | Reset to defaults (triggers reboot) |
| GET | `/api/mode` | - | Portal mode (core vs full) |
| POST | `/api/update` | - | OTA firmware upload |
| POST | `/api/reboot` | - | Reboot device |

See [docs/web-portal.md](docs/web-portal.md) for detailed guide.

## 📚 Dependencies

The project uses the following Arduino libraries (installed via `setup.sh`):

- **ArduinoJson@7.2.1**: JSON serialization for REST API
- **ESP Async WebServer@3.9.0**: Non-blocking web server
- **Async TCP@3.4.9**: Async TCP dependency
- **lvgl@8.3.11**: Graphics library for display
- **ESP32_Display_Panel@0.1.4**: Display panel drivers
- **ESP32_IO_Expander@0.0.2**: I/O expander support

See `arduino-libraries.txt` for complete list.

## 📄 License

This project is licensed under the MIT License - see the [LICENSE](LICENSE) file for details.

---

**Made with ❤️ for ESP32 developers**
