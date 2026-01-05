#!/bin/bash

# ESP32 Upload Script
# Uploads compiled firmware to ESP32 via serial port
# Usage: ./upload.sh [board-name] [port]
#   - board-name: Required when multiple boards configured
#   - port: Optional, auto-detected if not provided

set -e

# Load common configuration and functions
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/config.sh"

# Get arduino-cli path
ARDUINO_CLI=$(find_arduino_cli)

# Use esptool.py for robust flashing when PartitionScheme is specified.
# This preserves NVS (does not flash a full merged image), and ensures the app
# is written to the correct offset for custom partition tables.

extract_fqbn_option() {
    local fqbn="$1"
    local key="$2"
    # Options are in the 4th FQBN segment, comma-separated: key=value
    # Example: esp32:esp32:esp32s3:FlashSize=16M,PartitionScheme=foo
    local opts
    opts=$(echo "$fqbn" | cut -d':' -f4-)
    echo "$opts" | tr ',' '\n' | awk -F= -v k="$key" '$1==k {print $2; exit 0}'
}

find_latest_esp32_core_dir() {
    local base="$HOME/.arduino15/packages/esp32/hardware/esp32"
    if [[ ! -d "$base" ]]; then
        echo "";
        return 1
    fi
    local dir
    dir=$(ls -1d "$base"/*/ 2>/dev/null | sort -V | tail -n 1 || true)
    dir="${dir%/}"
    if [[ -z "$dir" || ! -d "$dir" ]]; then
        echo "";
        return 1
    fi
    echo "$dir"
}

find_esptool_py() {
    # ESP32 core tool layout varies by version:
    # - Newer cores ship an `esptool` executable under tools/esptool_py/<ver>/
    # - Some older setups may provide `esptool.py`
    local base="$HOME/.arduino15/packages/esp32/tools/esptool_py"
    if [[ -d "$base" ]]; then
        local tool
        tool=$(find "$base" -maxdepth 3 -type f -name "esptool" 2>/dev/null | sort -V | tail -n 1 || true)
        if [[ -n "$tool" ]]; then
            echo "$tool"
            return 0
        fi

        tool=$(find "$base" -maxdepth 3 -type f -name "esptool.py" 2>/dev/null | sort -V | tail -n 1 || true)
        if [[ -n "$tool" ]]; then
            echo "$tool"
            return 0
        fi
    fi

    # Fallback to PATH
    if command -v esptool >/dev/null 2>&1; then
        command -v esptool
        return 0
    fi
    if command -v esptool.py >/dev/null 2>&1; then
        command -v esptool.py
        return 0
    fi

    return 1
}

parse_partition_csv_offset() {
    local csv="$1"
    local name="$2"
    # CSV format: Name, Type, SubType, Offset, Size, Flags
    # Strip comments/blank lines, trim whitespace, match on Name.
    awk -F',' -v n="$name" '
        $0 ~ /^\s*#/ {next}
        $0 ~ /^\s*$/ {next}
        {
            gsub(/^[ \t]+|[ \t]+$/, "", $1);
            gsub(/^[ \t]+|[ \t]+$/, "", $4);
            if ($1 == n) {print $4; exit 0}
        }
    ' "$csv"
}

upload_with_esptool_partitions() {
    local fqbn="$1"
    local port="$2"
    local board_build_path="$3"
    local flash_mode="$4"  # "app" (default) or "full"

    local partition_scheme
    partition_scheme=$(extract_fqbn_option "$fqbn" "PartitionScheme" || true)
    if [[ -z "$partition_scheme" ]]; then
        echo "" >&2
        echo "Error: upload_with_esptool_partitions called without PartitionScheme in FQBN" >&2
        return 1
    fi

    local esp32_dir
    esp32_dir=$(find_latest_esp32_core_dir || true)
    if [[ -z "$esp32_dir" ]]; then
        echo "" >&2
        echo "Error: ESP32 Arduino core not found under ~/.arduino15/packages/esp32/hardware/esp32" >&2
        return 1
    fi

    local csv="$esp32_dir/tools/partitions/${partition_scheme}.csv"
    if [[ ! -f "$csv" ]]; then
        echo "" >&2
        echo "Error: Partition CSV not found: $csv" >&2
        echo "Run: ./tools/install-custom-partitions.sh (for custom schemes)" >&2
        return 1
    fi

    local app_offset
    app_offset=$(parse_partition_csv_offset "$csv" "app0" || true)
    if [[ -z "$app_offset" ]]; then
        echo "" >&2
        echo "Error: Could not parse app0 offset from: $csv" >&2
        return 1
    fi

    local esptool
    esptool=$(find_esptool_py || true)
    if [[ -z "$esptool" ]]; then
        echo "" >&2
        echo "Error: esptool.py not found (is esp32 core installed?)" >&2
        return 1
    fi

    # Extract chip type from FQBN (3rd segment contains esp32/esp32s3/etc)
    local chip
    chip=$(echo "$fqbn" | cut -d':' -f3 | grep -oE 'esp32[a-z0-9]*' | head -n1)
    if [[ -z "$chip" ]]; then
        chip="esp32"
    fi

    local app_bin="$board_build_path/app.ino.bin"

    if [[ ! -f "$app_bin" ]]; then
        echo "" >&2
        echo "Error: Missing build artifact: $app_bin" >&2
        echo "Please run: ./build.sh $BOARD" >&2
        return 1
    fi

    local bootloader_bin="$board_build_path/app.ino.bootloader.bin"
    local partitions_bin="$board_build_path/app.ino.partitions.bin"

    # boot_app0.bin is provided by the ESP32 Arduino core.
    # It's used for OTA selection. If it exists, flash it too.
    local boot_app0_bin="$esp32_dir/tools/partitions/boot_app0.bin"

    echo "Using esptool.py for PartitionScheme=$partition_scheme"
    echo "- Partition CSV: $csv"
    echo "- app0 offset:   $app_offset"

    # Default to flashing ONLY the app binary.
    # This avoids overwriting the partition table and bootloader, which can indirectly
    # cause NVS data to appear lost if offsets/layout change.
    if [[ -z "${flash_mode:-}" ]]; then
        flash_mode="app"
    fi

    local baud="921600"
    if [[ "$flash_mode" == "full" ]]; then
        if [[ ! -f "$bootloader_bin" || ! -f "$partitions_bin" ]]; then
            echo "" >&2
            echo "Error: Full flash requested but missing bootloader/partitions bins in $board_build_path" >&2
            echo "Expected: app.ino.bootloader.bin and app.ino.partitions.bin" >&2
            return 1
        fi

        echo "Flashing FULL image set (bootloader + partitions + app0 selection + app)..."
        if [[ -f "$boot_app0_bin" ]]; then
            "$esptool" --chip "$chip" --port "$port" --baud "$baud" write_flash -z \
                0x0 "$bootloader_bin" \
                0x8000 "$partitions_bin" \
                0xe000 "$boot_app0_bin" \
                "$app_offset" "$app_bin"
        else
            "$esptool" --chip "$chip" --port "$port" --baud "$baud" write_flash -z \
                0x0 "$bootloader_bin" \
                0x8000 "$partitions_bin" \
                "$app_offset" "$app_bin"
        fi
    else
        echo "Flashing app only (preserves NVS/FFat data)..."
        "$esptool" --chip "$chip" --port "$port" --baud "$baud" write_flash -z \
            "$app_offset" "$app_bin"
    fi
}

# Parse board and port arguments
parse_board_and_port_args "$@"

# Board-specific build directory
BOARD_BUILD_PATH="$BUILD_PATH/$BOARD"

# Auto-detect port if not specified
if [[ -z "$PORT" ]]; then
    if PORT=$(find_serial_port); then
        echo -e "${GREEN}Auto-detected port: $PORT${NC}"
    else
        echo -e "${RED}Error: No serial port detected${NC}"
        if [[ "${#FQBN_TARGETS[@]}" -gt 1 ]]; then
            echo "Usage: ${0##*/} <board-name> [port]"
            echo "Example: ${0##*/} $BOARD /dev/ttyUSB0"
        else
            echo "Usage: ${0##*/} [port]"
            echo "Example: ${0##*/} /dev/ttyUSB0"
        fi
        exit 1
    fi
fi

echo -e "${CYAN}=== Uploading ESP32 Firmware ===${NC}"

# Check if board build directory exists
if [ ! -d "$BOARD_BUILD_PATH" ]; then
    echo -e "${RED}Error: Build directory not found for board '$BOARD'${NC}"
    echo "Expected: $BOARD_BUILD_PATH"
    echo "Please run: ./build.sh $BOARD"
    exit 1
fi

# Display upload configuration
echo "Board: $BOARD"
echo "FQBN:  $FQBN"
echo "Port:  $PORT"
echo "Build: $BOARD_BUILD_PATH"
echo ""

# Upload firmware
PARTITION_SCHEME=$(extract_fqbn_option "$FQBN" "PartitionScheme" || true)
if [[ -n "$PARTITION_SCHEME" ]]; then
    # Use UPLOAD_FLASH_MODE=full to also flash bootloader/partitions.
    upload_with_esptool_partitions "$FQBN" "$PORT" "$BOARD_BUILD_PATH" "${UPLOAD_FLASH_MODE:-app}"
else
    "$ARDUINO_CLI" upload \
        --fqbn "$FQBN" \
        --port "$PORT" \
        --input-dir "$BOARD_BUILD_PATH"
fi

echo ""
echo -e "${GREEN}=== Upload Complete ===${NC}"
echo "Run ./monitor.sh to view serial output"
