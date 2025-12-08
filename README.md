# ESP32 Development Template

Skip the boilerplate and start building. ESP32 Arduino template with automated builds, captive portal WiFi setup, OTA updates, and flexible multi-board build system. Add new board variants in seconds with a simple config change, customize board-specific code when needed. Same build system powers local development and GitHub Actions workflows for CI/CD and releases.

## ✨ Features

- **🚀 Get Started Instantly**
  - **Zero Dependencies**: Local `arduino-cli` installation (no sudo, no system packages)
  - **One-Command Setup**: `./setup.sh` downloads toolchain and configures ESP32 platform
  - **Simple Scripts**: Build with `./build.sh`, upload with `./upload.sh`, monitor with `./monitor.sh`

- **📡 Production-Ready WiFi Management**
  - **Auto-AP Mode**: Device creates captive portal when unconfigured
  - **Web Configuration**: User-friendly interface at `192.168.4.1` for WiFi setup
  - **REST API**: Full `/api/*` endpoints for device control and monitoring
  - **OTA Updates**: Upload new firmware over WiFi without USB cable

- **🎯 Multi-Board Made Easy**
  - **Flexible Build System**: Add ESP32 variants with single config line
  - **Board-Specific Code**: Optional overrides for different GPIO pins and hardware
  - **Matrix Builds**: GitHub Actions compiles all boards in parallel

- **⚙️ Professional Development Workflow**
  - **Automated Releases**: Tag-based releases with firmware binaries and changelogs
  - **CI/CD Integration**: Same build scripts work locally and in GitHub Actions
  - **Version Management**: Semantic versioning with automatic firmware stamping

## 🚀 Quick Start

### Prerequisites

- Linux or WSL2 environment
- USB connection to ESP32 device
- Bash shell

### 1. Clone and Setup

```bash
git clone https://github.com/jantielens/esp32-template.git
cd esp32-template
./setup.sh
```

### 2. Configure Project Branding (Optional)

Customize your project name by editing `config.sh`:

```bash
PROJECT_NAME="my-iot-device"              # Slug for filenames/artifacts
PROJECT_DISPLAY_NAME="My IoT Device"      # Human-readable name for UI
```

This updates web portal titles, device names, AP SSID, and release artifacts. See [Build and Release Process](docs/build-and-release-process.md#project-branding-configuration) for details.

### 3. Build, Upload, Monitor

```bash
./build.sh              # Compile firmware for all boards
./build.sh esp32        # Compile for specific board
BOARD_PROFILE=psram ./build.sh esp32  # Optional build profile (if defined in config.sh)
./upload.sh esp32       # Upload to ESP32 (board name required if multiple boards)
./monitor.sh            # View serial output

# Or use convenience scripts:
./bum.sh esp32          # Build + Upload + Monitor
./um.sh esp32           # Upload + Monitor
```

### 4. Start Developing

Edit `src/app/app.ino` with your code and repeat step 2.

## 📁 Project Structure

```
esp32-template-wifi/
├── bin/                           # Local arduino-cli installation
├── build/                         # Compiled firmware binaries
│   ├── esp32/                     # ESP32 Dev Module builds
│   └── esp32c3/                   # ESP32-C3 Super Mini builds
├── docs/                          # Documentation
│   ├── scripts.md                 # Script usage guide
│   ├── web-portal.md              # Web portal guide
│   ├── wsl-development.md         # WSL/USB setup
│   └── library-management.md      # Library management
├── src/
│   ├── app/
│   │   ├── app.ino                # Main sketch file
│   │   ├── board_config.h         # Default board configuration
│   │   ├── config_manager.cpp/h   # NVS config storage
│   │   ├── web_portal.cpp/h       # Web server & API
│   │   ├── web_assets.cpp/h       # Embedded HTML/CSS/JS
│   │   └── web/
│   │       ├── portal.html        # Portal interface
│   │       ├── portal.css         # Styles
│   │       └── portal.js          # Client logic
│   ├── boards/                    # Board-specific overrides (optional)
│   │   └── esp32c3/               # Sample ESP32-C3 Super Mini config
│   │       ├── board_config.h     # Sample LED GPIO8 + custom function (commented out)
│   │       └── board_config.cpp   # Sample Board-specific implementation (commented out)
│   └── version.h                  # Firmware version tracking
├── .github/
│   └── workflows/
│       └── build.yml              # CI/CD pipeline
├── config.sh                      # Common configuration
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

## 🌐 Web Configuration Portal

The template includes a full-featured web portal for device configuration and monitoring.

### Portal Modes

**Core Mode (AP)**: Runs when WiFi is not configured
- Device creates Access Point: `ESP32-XXXXXX`
- Captive portal at: `http://192.168.4.1`
- Configure WiFi credentials and device settings

**Full Mode (WiFi)**: Runs when connected to WiFi
- Access at device IP or mDNS hostname
- All configuration options available
- Real-time health monitoring
- OTA firmware updates

### Device Discovery

When connected to WiFi, devices can be discovered using multiple methods:

- **📡 Router/DHCP Table**: Device appears with configured hostname (e.g., `esp32-1234`)
- **🔍 mDNS/Bonjour**: Access via `<hostname>.local` (e.g., `esp32-1234.local`)
  - Works on macOS, Linux, iOS, Android
  - Includes rich service metadata (version, model, device type, features, URL)
- **📊 Network Scanners**: Tools like Fing, WiFiMan show device with hostname

The hostname is automatically set from the device name and includes the last 4 hex digits of the chip ID for uniqueness (e.g., "ESP32 1234" → hostname "esp32-1234").

### REST API Endpoints

| Method | Endpoint | Purpose |
|--------|----------|----------|
| GET | `/api/info` | Device info (firmware, chip, cores, flash, PSRAM, hostname, MAC) |
| GET | `/api/health` | Real-time health stats (CPU, memory, WiFi, uptime, hostname) |
| GET | `/api/config` | Current configuration |
| POST | `/api/config` | Save configuration (triggers reboot) |
| DELETE | `/api/config` | Reset to defaults (triggers reboot) |
| GET | `/api/mode` | Portal mode (core vs full) |
| POST | `/api/update` | OTA firmware upload |
| POST | `/api/reboot` | Reboot device |

See [docs/web-portal.md](docs/web-portal.md) for detailed guide.

## 🛠️ Available Scripts

| Script | Purpose | Usage |
|--------|---------|-------|
| `config.sh` | Common configuration (sourced by scripts) | Sourced automatically |
| `setup.sh` | Install arduino-cli and ESP32 core | Run once during initial setup |
| `build.sh` | Compile the Arduino sketch | `./build.sh [board-name]` |
| `upload.sh` | Upload firmware to ESP32 | `./upload.sh [board-name] [port]` |
| `monitor.sh` | Display serial output | `./monitor.sh [port] [baud]` |
| `bum.sh` | Build + Upload + Monitor (full cycle) | `./bum.sh [board-name] [port]` |
| `um.sh` | Upload + Monitor | `./um.sh [board-name] [port]` |
| `upload-erase.sh` | Completely erase ESP32 flash | `./upload-erase.sh [board-name] [port]` |
| `clean.sh` | Remove build artifacts | `./clean.sh` |
| `library.sh` | Manage Arduino libraries | `./library.sh [command]` |

See [docs/scripts.md](docs/scripts.md) for detailed documentation.

## 🔧 Configuration

### Target Boards

The project supports multiple ESP32 board variants configured in `config.sh`:

```bash
# Default configuration includes:
declare -A FQBN_TARGETS=(
    ["esp32:esp32:esp32"]="esp32"                                        # ESP32 Dev Module
    ["esp32:esp32:nologo_esp32c3_super_mini:CDCOnBoot=cdc"]="esp32c3"  # ESP32-C3 Super Mini
)
```

**Adding/Modifying Boards:**

1. Edit `config.sh` and add/modify entries in `FQBN_TARGETS`
2. Provide custom board name or leave empty to auto-extract from FQBN
3. Build outputs go to `build/<board-name>/` directory

**Board Override Macros & Includes:**
- If `src/boards/<board>/` exists, `build.sh` adds it to the include path and defines:
  - `BOARD_<BOARDNAME>` (uppercased, e.g., `BOARD_ESP32C3`)
  - `BOARD_HAS_OVERRIDE` (allows `src/app/board_config.h` to `#include_next` the board override)
- No changes needed in `app.ino`; overrides are pulled automatically.

**Examples:**
```bash
# Build all boards
./build.sh

# Build specific board
./build.sh esp32
./build.sh esp32c3

# Upload to specific board
./upload.sh esp32
./upload.sh esp32c3 /dev/ttyACM0  # With explicit port
```

### Board-Specific Configuration

The project uses a flexible board configuration system that supports board-specific customization without code duplication.

**Default Configuration**: All boards use `src/app/board_config.h` with common settings (LED pins, WiFi settings, hardware capabilities).

**Board-Specific Overrides** (Optional): Create `src/boards/[board-name]/board_config.h` to customize specific boards:

```bash
# Example: ESP32 Dev Module has built-in LED
mkdir -p src/boards/esp32
cat > src/boards/esp32/board_config.h << 'EOF'
#ifndef BOARD_CONFIG_H
#define BOARD_CONFIG_H

#define BOARD_NAME "ESP32 Dev Module"
#define HAS_BUILTIN_LED true
#define LED_PIN 2

#endif
EOF
```

The build system automatically detects and uses board-specific overrides when present. If no override exists, default configuration is used.

**Configuration Options:**
- Hardware capabilities: `HAS_BUILTIN_LED`
- Pin mappings: `LED_PIN`, `LED_ACTIVE_HIGH`
- WiFi settings: `WIFI_MAX_ATTEMPTS`

See `src/app/board_config.h` for currently available options. Additional configuration options can be added as needed for your project.

### Serial Port

Scripts auto-detect `/dev/ttyUSB0` or `/dev/ttyACM0`. Manually specify if needed:

```bash
./upload.sh esp32 /dev/ttyUSB1
./monitor.sh /dev/ttyACM0 115200
```

## 🖥️ WSL2 Development

For Windows users with WSL2, USB passthrough is required:

```powershell
# Windows PowerShell (Administrator)
usbipd list
usbipd bind --busid 1-4
usbipd attach --wsl --busid 1-4
```

```bash
# WSL Terminal
sudo usermod -a -G dialout $USER
# Restart WSL: wsl --terminate Ubuntu (in PowerShell)
```

See [docs/wsl-development.md](docs/wsl-development.md) for complete guide.

## 📚 Library Management

Libraries are managed via `arduino-libraries.txt` for consistency across local and CI environments.

### Quick Commands

```bash
./library.sh search mqtt          # Find libraries
./library.sh add PubSubClient     # Add and install
./library.sh list                 # Show configured libraries
./library.sh installed            # Show installed libraries
```

Libraries in `arduino-libraries.txt` are automatically installed by `setup.sh` and in GitHub Actions.

**Note:** The template starts with no libraries configured. Uncomment or add libraries as needed for your project.

See [docs/library-management.md](docs/library-management.md) for detailed guide.

## 🔍 Version Management

Firmware versions are tracked in `src/version.h`:

```cpp
#define VERSION_MAJOR 1
#define VERSION_MINOR 0
#define VERSION_PATCH 0
```

Version is automatically displayed at startup. Update before releases.

## 🧪 Testing & CI/CD

GitHub Actions automatically validates builds on push:

- Compiles firmware for all configured board variants (matrix build)
- Caches arduino-cli and libraries for faster builds
- Uploads separate build artifacts per board (.bin and .elf files)
- Generates build summary with firmware sizes

## 🚀 Release Process

Releases are automated via GitHub Actions when version tags are pushed.

### Creating a Release

#### Option 1: Automated Script (Recommended)

```bash
# Run the release preparation script
./create-release.sh 0.0.5 "Improved logging and stability"

# This will:
# 1. Create release/v0.0.5 branch
# 2. Update src/version.h with version numbers
# 3. Update CHANGELOG.md (move [Unreleased] to [0.0.5])
# 4. Commit and push changes
# 5. Provide PR creation URL

# After PR is merged:
git checkout main
git pull
git tag -a v0.0.5 -m "Release v0.0.5"
git push origin v0.0.5
```

#### Option 2: Manual Process

```bash
# 1. Create release branch
git checkout -b release/v0.0.5

# 2. Update src/version.h
#    VERSION_MAJOR 0
#    VERSION_MINOR 0
#    VERSION_PATCH 5

# 3. Update CHANGELOG.md
#    Move [Unreleased] items to [0.0.5] - YYYY-MM-DD

# 4. Commit and push
git add src/version.h CHANGELOG.md
git commit -m "chore: bump version to 0.0.5"
git push origin release/v0.0.5

# 5. Create and merge PR: release/v0.0.5 → main

# 6. Tag the release
git checkout main
git pull
git tag -a v0.0.5 -m "Release v0.0.5"
git push origin v0.0.5
```

### What Happens Automatically

When you push a tag matching `v*.*.*`:

1. **GitHub Actions triggers** `.github/workflows/release.yml`
2. **Builds firmware** for all board variants
3. **Creates GitHub Release** with:
   - Release notes extracted from CHANGELOG.md
   - Firmware binaries: `<board>-firmware-vX.Y.Z.bin`
   - Debug symbols: `<board>-firmware-vX.Y.Z.elf`
   - Build metadata: `build-info-<board>.txt`
   - SHA256 checksums: `SHA256SUMS.txt`
4. **Marks as pre-release** if version contains hyphen (e.g., `v1.0.0-beta.1`)

### Release Artifacts

Each release includes firmware binaries for all board variants:
- `esp32-firmware-v0.0.5.bin` - Ready to flash
- `esp32c3-firmware-v0.0.5.bin`
- `esp32c6-firmware-v0.0.5.bin`
- `SHA256SUMS.txt` - Checksums for verification

Debug symbols (`.elf`) and build metadata (`.txt`) are available in the workflow artifacts but not included in releases to keep downloads lightweight.

### Pre-Release Testing

Test release workflow with pre-release tags:

```bash
git tag -a v0.0.5-beta.1 -m "Pre-release v0.0.5-beta.1"
git push origin v0.0.5-beta.1
```

Pre-release tags automatically mark the release as "Pre-release" on GitHub.

## 🐛 Troubleshooting

### Permission Denied on Serial Port

```bash
sudo usermod -a -G dialout $USER
# Logout and login, or restart WSL
```

### arduino-cli Not Found

```bash
./setup.sh  # Reinstall arduino-cli
```

### Build Directory Not Found

```bash
./build.sh  # Build before uploading
```

### Device Not Detected

```bash
# Check if device is connected
ls -l /dev/ttyUSB* /dev/ttyACM*

# For WSL, check Windows PowerShell
usbipd list
```

## 📖 Documentation

- [Web Portal Guide](docs/web-portal.md) - Configuration portal & REST API
- [Script Reference](docs/scripts.md) - Detailed script usage
- [Build and Release Process](docs/build-and-release-process.md) - Project branding, build system, and release workflow
- [WSL Development Guide](docs/wsl-development.md) - Windows/WSL setup
- [Library Management](docs/library-management.md) - Managing dependencies
- [Changelog](CHANGELOG.md) - Version history and release notes

## 📄 License

This project is licensed under the MIT License - see the [LICENSE](LICENSE) file for details.

---

**Made with ❤️ for ESP32 developers**
