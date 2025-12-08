#!/bin/bash

# ESP32 Template Project Configuration
# This file contains common configuration and helper functions used by all scripts
# Source this file at the beginning of each script

# ============================================================================
# PROJECT BRANDING CONFIGURATION
# ============================================================================
# These values define your project's identity across the entire system.
# Change these to customize your project name and branding.
#
# PROJECT_NAME (slug format - no spaces, lowercase with hyphens)
#   Used for:
#   - Build artifact filenames: {PROJECT_NAME}-{board}-v{version}.bin
#   - GitHub release files: {PROJECT_NAME}-esp32-v0.0.5.bin
#   - GitHub artifact names in CI/CD workflows
#   - WiFi Access Point SSID: {PROJECT_NAME}-1A2B3C4D (uppercase)
#   Example: "esp32-template-wifi" → "ESP32-TEMPLATE-WIFI-1A2B3C4D"
#
# PROJECT_DISPLAY_NAME (human-readable format)
#   Used for:
#   - Web portal page title: "{PROJECT_DISPLAY_NAME} Configuration Portal"
#   - Web portal header: "{PROJECT_DISPLAY_NAME} Configuration"
#   - Default device name: "{PROJECT_DISPLAY_NAME} 1A2B"
#   - Logs page title: "Device Logs - {PROJECT_DISPLAY_NAME}"
#   - REST API /api/info response (both values included)
#   Example: "ESP32 Template WiFi"
#
PROJECT_NAME="esp32-template-wifi"
PROJECT_DISPLAY_NAME="ESP32 Template WiFi"

# Board configuration (FQBN - Fully Qualified Board Name)
# Define target boards as an associative array: [FQBN]="board-name"
# - Provide custom board name for clean directory naming
# - Omit board name (or leave empty) to auto-extract from FQBN (3rd segment)
#
# USB Serial Support (CDCOnBoot):
# - ESP32-C3, C6, S3: Add `:CDCOnBoot=cdc` to enable USB serial output
# - ESP32 (classic): No USB-OTG, uses hardware UART - no CDC parameter needed
# - To check if a board needs CDC: `arduino-cli board details <FQBN> | grep CDCOnBoot`
#
# Examples:
#   ["esp32:esp32:esp32"]="esp32"                                       # Classic ESP32 - hardware UART
#   ["esp32:esp32:nologo_esp32c3_super_mini:CDCOnBoot=cdc"]="esp32c3"   # C3 with USB CDC enabled
#   ["esp32:esp32:dfrobot_firebeetle2_esp32c6:CDCOnBoot=cdc"]="esp32c6" # C6 with USB CDC enabled
#   ["esp32:esp32:esp32c6:CDCOnBoot=cdc"]="esp32c6supermini"            # C6 with USB CDC enabled (generic Super Mini variant)
declare -A FQBN_TARGETS=(
    ["esp32:esp32:esp32"]="esp32"
    ["esp32:esp32:nologo_esp32c3_super_mini:CDCOnBoot=cdc"]="esp32c3"
)

# Default board (used when only one board is configured)
DEFAULT_BOARD=""

# Get script directory (works when sourced)
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# Color definitions for terminal output
BLUE='\033[0;34m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
RED='\033[0;31m'
CYAN='\033[0;36m'
NC='\033[0m' # No Color

# Project paths
SKETCH_PATH="$SCRIPT_DIR/src/app/app.ino"
BUILD_PATH="$SCRIPT_DIR/build"

# Find arduino-cli executable
# Checks for local installation first, then falls back to system-wide
find_arduino_cli() {
    if [ -f "$SCRIPT_DIR/bin/arduino-cli" ]; then
        echo "$SCRIPT_DIR/bin/arduino-cli"
    elif command -v arduino-cli &> /dev/null; then
        echo "arduino-cli"
    else
        echo "Error: arduino-cli is not found" >&2
        echo "Please run setup.sh or install arduino-cli system-wide" >&2
        exit 1
    fi
}

# Auto-detect serial port
# Returns /dev/ttyUSB0 if exists, otherwise /dev/ttyACM0
# Returns exit code 1 if no port found
find_serial_port() {
    if [ -e /dev/ttyUSB0 ]; then
        echo "/dev/ttyUSB0"
        return 0
    elif [ -e /dev/ttyACM0 ]; then
        echo "/dev/ttyACM0"
        return 0
    else
        return 1
    fi
}

# Get board name for a given FQBN
# If custom name is provided in FQBN_TARGETS, use it
# Otherwise, extract board ID (3rd segment) from FQBN
get_board_name() {
    local fqbn="$1"
    local board_name="${FQBN_TARGETS[$fqbn]}"
    
    if [[ -n "$board_name" ]]; then
        echo "$board_name"
    else
        # Extract board ID (3rd segment) from FQBN
        # Example: "esp32:esp32:nologo_esp32c3_super_mini:CDCOnBoot=cdc" → "nologo_esp32c3_super_mini"
        echo "$fqbn" | cut -d':' -f3
    fi
}

# List all configured boards
list_boards() {
    echo -e "${CYAN}Available boards:${NC}"
    for fqbn in "${!FQBN_TARGETS[@]}"; do
        local board_name=$(get_board_name "$fqbn")
        echo -e "  ${GREEN}$board_name${NC} → $fqbn"
    done
}

# Get FQBN for a given board name
get_fqbn_for_board() {
    local target_board="$1"
    for fqbn in "${!FQBN_TARGETS[@]}"; do
        local board_name=$(get_board_name "$fqbn")
        if [[ "$board_name" == "$target_board" ]]; then
            echo "$fqbn"
            return 0
        fi
    done
    return 1
}

# Parse board and port arguments for scripts that need them
# Usage: parse_board_and_port_args "$@"
# Sets global variables: BOARD, PORT, FQBN
# Behavior:
#   - Multiple boards: 1st arg = board (required), 2nd arg = port (optional)
#   - Single board: accepts either
#       * 1st arg = PORT (legacy behavior)
#       * or 1st arg = BOARD, 2nd arg = PORT (friendly/explicit form)
#   - Exits with error if board name is required but not provided
parse_board_and_port_args() {
    local board_count="${#FQBN_TARGETS[@]}"
    
    if [[ $board_count -gt 1 ]]; then
        # Multiple boards: first arg is board name, second is optional port
        if [[ -z "$1" ]]; then
            echo -e "${RED}Error: Board name required when multiple boards are configured${NC}"
            echo ""
            list_boards
            echo ""
            echo "Usage: ${0##*/} <board-name> [port]"
            exit 1
        fi
        BOARD="$1"
        PORT="$2"
    else
        # Single board: allow either explicit BOARD (arg1) or PORT (arg1)
        local arg1="$1"
        local arg2="$2"
        local default_board
        default_board=$(get_board_name "${!FQBN_TARGETS[@]}")

        if [[ -n "$arg1" ]] && get_fqbn_for_board "$arg1" >/dev/null 2>&1; then
            # Explicit board name provided even though only one board exists
            BOARD="$arg1"
            PORT="$arg2"
        else
            # Legacy behavior: arg1 is treated as port (or empty)
            BOARD="$default_board"
            PORT="$arg1"
        fi
    fi
    
    # Get FQBN for the board
    FQBN=$(get_fqbn_for_board "$BOARD") || true
    if [[ -z "$FQBN" ]]; then
        echo -e "${RED}Error: Board '$BOARD' not found${NC}"
        echo ""
        list_boards
        exit 1
    fi
}
