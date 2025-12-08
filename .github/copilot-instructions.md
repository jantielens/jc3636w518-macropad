# Copilot Instructions for ESP32 Template Project

## Project Overview

ESP32 Arduino development template using `arduino-cli` for headless builds. Designed for WSL2/Linux environments with local toolchain installation (no system dependencies).

## Architecture

- **Build System**: Custom bash scripts wrapping `arduino-cli` (installed locally to `./bin/`)
- **Sketch Location**: Main Arduino file at `src/app/app.ino`
- **Board Configuration**: Flexible system with optional board-specific overrides
  - `src/app/board_config.h` - Default configuration for all boards
  - `src/boards/[board-name]/board_config.h` - Optional board-specific overrides (if directory exists)
  - Build system automatically detects and applies overrides when present
- **Web Portal**: Async web server with captive portal support
  - `web_portal.cpp/h` - Server and REST API implementation
  - `web_assets.cpp/h` - Embedded HTML/CSS/JS (from `src/app/web/`)
  - `config_manager.cpp/h` - NVS configuration storage
- **Output**: Compiled binaries in `./build/<board-name>/` directories
- **Board Targets**: Multi-board support via `FQBN_TARGETS` array in `config.sh`
  - `esp32:esp32:esp32` → `build/esp32/` (ESP32 Dev Module)
  - `esp32:esp32:nologo_esp32c3_super_mini:CDCOnBoot=cdc` → `build/esp32c3/` (ESP32-C3 Super Mini)

## Critical Developer Workflows

### First-time Setup
```bash
./setup.sh  # Downloads arduino-cli, installs ESP32 core, configures environment
```

### Build-Upload-Monitor Cycle
```bash
# Build all configured boards
./build.sh

# Or build specific board
./build.sh esp32        # Compile for ESP32 Dev Module
./build.sh esp32c3      # Compile for ESP32-C3 Super Mini

# Upload (board name required when multiple boards configured)
./upload.sh esp32       # Auto-detects /dev/ttyUSB0 or /dev/ttyACM0
./upload.sh esp32c3     # Auto-detects /dev/ttyUSB0 or /dev/ttyACM0

# Monitor
./monitor.sh            # Serial monitor at 115200 baud

# Convenience scripts
./bum.sh esp32          # Build + Upload + Monitor
./um.sh esp32c3         # Upload + Monitor
```

All scripts use absolute paths via `SCRIPT_DIR` resolution - they work from any directory.

## Project-Specific Conventions

### Script Design Pattern
- All scripts source `config.sh` for common configuration and helper functions
- `config.sh` provides:
  - `SCRIPT_DIR` resolution for absolute path handling
  - `find_arduino_cli()` - Checks local `$SCRIPT_DIR/bin/arduino-cli` first, then system-wide
  - `find_serial_port()` - Auto-detects `/dev/ttyUSB0` first, fallback to `/dev/ttyACM0`
  - Project constants: `PROJECT_NAME`, `SKETCH_PATH`, `BUILD_PATH`
  - Board management: `FQBN_TARGETS` array, `get_board_name()`, `list_boards()`, `get_fqbn_for_board()`
- Scripts work from any directory due to absolute path resolution
- Multi-board scripts require board name parameter when multiple targets are configured

### Arduino Code Standards
- Use `Serial.begin(115200)` for consistency with monitor.sh default
- Include startup diagnostics (chip model, revision, CPU freq, flash size) using `ESP.*` functions
- Implement heartbeat pattern with `millis()` for long-running loops (5s interval)

### Web Portal Conventions

**Portal Modes**:
- **Core Mode**: AP with captive portal (192.168.4.1) - WiFi not configured
- **Full Mode**: Connected to WiFi - portal at device IP/hostname

**REST API Design**:
- All endpoints under `/api/*` namespace
- Use semantic names: `/api/info` (device info), `/api/health` (real-time stats), `/api/config` (settings)
- Return JSON responses with proper HTTP status codes
- POST operations that modify state trigger device reboot for consistency

**Health Monitoring**:
- `/api/health` provides real-time metrics (CPU, memory, WiFi, temperature, uptime)
- CPU usage calculated via IDLE task: `100 - (idle_runtime/total_runtime * 100)`
- Temperature sensor with `SOC_TEMP_SENSOR_SUPPORTED` guards for cross-platform compatibility
- Update interval: 10s (compact widget), 5s (expanded widget)

**UI Design**:
- Minimalist card-based layout with gradient header
- 6 header badges showing device capabilities:
  - Firmware version (purple badge)
  - Chip info (orange badge)
  - CPU cores (green badge)
  - CPU frequency (yellow badge)
  - Flash size (cyan badge)
  - PSRAM status (teal badge)
- Floating health widget with compact/expanded views
- Breathing animation on status updates

## WSL-Specific Requirements

Serial port access requires:
1. `usbipd-win` to bind USB devices from Windows host
2. User must be in `dialout` group: `sudo usermod -a -G dialout $USER`
3. Full WSL restart after group change: `wsl --terminate Ubuntu` (PowerShell)

See `docs/wsl-development.md` for complete USB/IP setup guide.

## Key Files

### Scripts
- `config.sh` - Common configuration and helper functions (sourced by all scripts)
- `setup.sh` - Downloads arduino-cli v0.latest, configures ESP32 platform, installs libraries
- `build.sh` - Compiles to `./build/<board-name>/*.bin` files (all boards or specific board)
- `upload.sh` - Flashes firmware via serial (requires board name with multiple boards)
- `upload-erase.sh` - Completely erases ESP32 flash memory (requires board name with multiple boards)
- `monitor.sh` - Serial console (Ctrl+C to exit)
- `clean.sh` - Removes all build artifacts (all board directories)
- `library.sh` - Manages Arduino library dependencies
- `bum.sh` - Build + Upload + Monitor convenience script
- `um.sh` - Upload + Monitor convenience script

### Source
- `src/app/app.ino` - Main sketch file (standard Arduino structure)
- `src/app/board_config.h` - Default board configuration (LED pins, WiFi settings)
- `src/boards/[board-name]/board_config.h` - Optional board-specific overrides
- `src/boards/[board-name]/board_config.cpp` - Optional board-specific implementations
- `src/app/web_portal.cpp/h` - Async web server and REST API endpoints
- `src/app/web_assets.cpp/h` - Embedded HTML/CSS/JS from `web/` directory
- `src/app/config_manager.cpp/h` - NVS-based configuration storage
- `src/app/web/portal.html` - Web interface markup
- `src/app/web/portal.css` - Styles (gradients, animations, responsive)
- `src/app/web/portal.js` - Client-side logic (API calls, health updates)
- `src/version.h` - Firmware version tracking

### Configuration
- `config.sh` - Project paths, FQBN_TARGETS array, and helper functions
- `arduino-libraries.txt` - List of required Arduino libraries (auto-installed by setup.sh)
- `.github/workflows/build.yml` - CI/CD pipeline with matrix builds for all board variants

## Library Management

- **Configuration File**: `arduino-libraries.txt` lists all required Arduino libraries
- **Management Script**: `./library.sh` provides commands to search, add, remove, and list libraries
- **Auto-Installation**: `setup.sh` reads `arduino-libraries.txt` and installs all listed libraries
- **CI/CD Integration**: GitHub Actions automatically installs libraries via `setup.sh`
- **Required Libraries**:
  - `ArduinoJson@7.2.1` - JSON serialization for REST API
  - `ESP Async WebServer@3.9.0` - Non-blocking web server
  - `Async TCP@3.4.9` - Async TCP dependency

### Library Commands
```bash
./library.sh search <keyword>    # Find libraries
./library.sh add <library>       # Add to config and install
./library.sh list                # Show configured libraries
```

## Common Pitfalls

- **Permission denied on /dev/ttyUSB0**: User not in dialout group or WSL not restarted
- **arduino-cli not found**: Scripts support both local (`./bin/arduino-cli`) and system-wide installations
- **Upload with sudo fails**: Root user lacks ESP32 core installation; use dialout group instead

## Copilot Agent Guidelines

### Before Making Significant Changes

Before implementing significant changes or starting major work, the agent must:

1. **Create a concise summary** of the proposed changes including:
   - What will be changed and why
   - Which files will be affected
   - Expected impact on the project
   - Any potential risks or breaking changes

2. **Ask for user approval and/or feedback** and wait for confirmation

3. **Only proceed with implementation** after receiving user approval

### After Significant Changes

After every significant change, the agent must:

1. **Verify the changes by building** the code:
   - Run `./build.sh` to ensure the code compiles successfully
   - Check for any compilation errors or warnings
   - Only proceed if the build completes without errors

2. **Check if documentation needs updates** by reviewing:
   - `README.md` - Main project documentation
   - `docs/web-portal.md` - Web portal and REST API guide
   - `docs/scripts.md` - Script usage guide
   - `docs/library-management.md` - Library management guide
   - `docs/build-and-release-process.md` - Project branding, build system, and release workflow guide
   - `docs/wsl-development.md` - WSL setup guide
   - `.github/copilot-instructions.md` - This file
   - `.github/workflows/build.yml` - CI/CD build pipeline
   - `.github/workflows/release.yml` - CI/CD release pipeline

3. **Update existing documentation** if changes affect documented behavior

4. **Before creating new documentation files**, ask the user:
   - "Should I create a new doc file for [topic]?"
   - Wait for confirmation before creating new docs

### Examples of Significant Changes

- Adding new scripts or tools
- Modifying build/upload/monitor workflows
- Changing project structure
- Adding new dependencies or requirements
- Updating CI/CD pipeline
- Changing library management approach

### Examples of Documentation Updates Needed

- New script added → Update `README.md` script table and `docs/scripts.md`
- Library management changed → Update `docs/library-management.md`
- Build workflow modified → Update `README.md` CI/CD section and `docs/build-and-release-process.md`
- Board configuration system changed → Update `README.md` board configuration section and `docs/build-and-release-process.md`
- Release workflow modified → Update `docs/build-and-release-process.md` and `README.md` release section
- New requirement added → Update `README.md` prerequisites
- REST API endpoint added/changed → Update `docs/web-portal.md` and `README.md` API table
- Web UI feature changed → Update `docs/web-portal.md` features section
- New version released → Update `CHANGELOG.md` with changes, update `src/version.h` with new version number
- Release process changed → Update `docs/build-and-release-process.md` with new workflow

### Build Verification

Always verify code changes by building:

```bash
./build.sh  # Must complete successfully after code changes
```

If the build fails:
- Review and fix compilation errors
- Check library dependencies in `arduino-libraries.txt`
- Verify Arduino code syntax and ESP32 compatibility
