#!/bin/bash

# ESP32 Build Script
# Compiles the Arduino sketch to board-specific build directories
# Usage: ./build.sh [board-name]
#   - No parameter: Build all configured boards
#   - With board name: Build only that specific board

set -e

# Load common configuration and functions
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/config.sh"

# Get arduino-cli path
ARDUINO_CLI=$(find_arduino_cli)

# Parse command line arguments
TARGET_BOARD="$1"
# Optional board profile for build properties (e.g., PSRAM/flash options)
PROFILE="${BOARD_PROFILE:-${PROFILE:-}}"

# Build a single board
build_board() {
    local fqbn="$1"
    local board_name="$2"
    local board_build_path="$BUILD_PATH/$board_name"
    local board_override_dir="$SCRIPT_DIR/src/boards/$board_name"
    
    echo -e "${CYAN}=== Building for $board_name ===${NC}"
    echo "Board:     $board_name"
    echo "FQBN:      $fqbn"
    echo "Output:    $board_build_path"
    
    # Check for board-specific configuration overrides
    EXTRA_FLAGS=()
    if [[ -d "$board_override_dir" ]]; then
        echo -e "${YELLOW}Config:    Using board-specific overrides from src/boards/$board_name/${NC}"
        # Force include path and board macro define; include file via board_config.h #include_next directive
        board_macro="BOARD_${board_name^^}"
        EXTRA_FLAGS+=(--build-property "compiler.cpp.extra_flags=-I$board_override_dir -D$board_macro -DBOARD_HAS_OVERRIDE=1")
    else
        echo "Config:    Using default configuration"
    fi
    echo ""
    
    # Create board-specific build directory
    mkdir -p "$board_build_path"
    
    # Board-specific build properties (from config.sh), optional profile
    BUILD_PROPS_ARR=()
    if declare -f get_build_props_for_board >/dev/null; then
        mapfile -t BUILD_PROPS_ARR < <(get_build_props_for_board "$board_name" "$PROFILE")
        if [[ ${#BUILD_PROPS_ARR[@]} -gt 0 ]]; then
            echo "Build props: ${BUILD_PROPS_ARR[*]}"
        fi
    fi

    # Compile the sketch with optional board-specific includes and build props
    "$ARDUINO_CLI" compile \
        --fqbn "$fqbn" \
        "${EXTRA_FLAGS[@]}" \
        "${BUILD_PROPS_ARR[@]}" \
        --output-dir "$board_build_path" \
        "$SKETCH_PATH"
    
    echo ""
    echo -e "${GREEN}✓ Build complete for $board_name${NC}"
    ls -lh "$board_build_path"/*.bin 2>/dev/null || echo "Binary files generated"
    echo ""
}

# Generate web assets (once for all builds)
echo "Generating web assets..."
"$SCRIPT_DIR/tools/minify-web-assets.sh" "$PROJECT_NAME" "$PROJECT_DISPLAY_NAME"
echo ""

# Determine which boards to build
if [[ -n "$TARGET_BOARD" ]]; then
    # Build specific board
    FQBN=$(get_fqbn_for_board "$TARGET_BOARD")
    if [[ -z "$FQBN" ]]; then
        echo -e "${RED}Error: Board '$TARGET_BOARD' not found${NC}"
        echo ""
        list_boards
        exit 1
    fi
    
    build_board "$FQBN" "$TARGET_BOARD"
else
    # Build all configured boards
    echo -e "${CYAN}=== Building ESP32 Firmware for All Boards ===${NC}"
    echo "Project:   $PROJECT_NAME"
    echo "Sketch:    $SKETCH_PATH"
    echo ""
    
    for fqbn in "${!FQBN_TARGETS[@]}"; do
        board_name=$(get_board_name "$fqbn")
        build_board "$fqbn" "$board_name"
    done
    
    echo -e "${GREEN}=== All Builds Complete ===${NC}"
fi
