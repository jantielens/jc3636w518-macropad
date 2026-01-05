#!/usr/bin/env bash

# Install/register custom partition tables into the Arduino ESP32 core.
#
# This is intentionally a standalone script so:
# - Local dev can run: ./tools/install-custom-partitions.sh
# - setup.sh can call it automatically
# - CI workflows can call it before compiling boards that use PartitionScheme=...

set -euo pipefail

SCRIPT_DIR="$(CDPATH="" cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(CDPATH="" cd "$SCRIPT_DIR/.." && pwd)"

PARTITION_FILE_SRC_C3="$REPO_ROOT/partitions/partitions_ota_1_9mb.csv"
PARTITION_SCHEME_ID_C3="ota_1_9mb"
PARTITION_FILE_BASENAME_C3="partitions_ota_1_9mb.csv"
PARTITION_NAME_NO_EXT_C3="partitions_ota_1_9mb"

BOARD_ID_C3="nologo_esp32c3_super_mini"
UPLOAD_MAX_SIZE_C3="1966080"  # 0x1E0000

PARTITION_FILE_SRC_S3="$REPO_ROOT/partitions/app3M_fat9M_16MB_big_nvs.csv"
PARTITION_SCHEME_ID_S3="app3M_fat9M_16MB_big_nvs"
PARTITION_FILE_BASENAME_S3="app3M_fat9M_16MB_big_nvs.csv"
PARTITION_NAME_NO_EXT_S3="app3M_fat9M_16MB_big_nvs"

BOARD_ID_S3="esp32s3"
UPLOAD_MAX_SIZE_S3="3145728"  # 0x300000

ESP32_HW_BASE="$HOME/.arduino15/packages/esp32/hardware/esp32"

if [[ ! -f "$PARTITION_FILE_SRC_C3" ]]; then
  echo "Error: partition file not found: $PARTITION_FILE_SRC_C3" >&2
  echo "Did you delete or rename the file under partitions/?" >&2
  exit 1
fi

if [[ ! -f "$PARTITION_FILE_SRC_S3" ]]; then
  echo "Error: partition file not found: $PARTITION_FILE_SRC_S3" >&2
  echo "Did you delete or rename the file under partitions/?" >&2
  exit 1
fi

if [[ ! -d "$ESP32_HW_BASE" ]]; then
  echo "Error: ESP32 Arduino core not found at $ESP32_HW_BASE" >&2
  echo "Run ./setup.sh first to install esp32:esp32." >&2
  exit 1
fi

# Pick the latest installed ESP32 core directory.
# Example: ~/.arduino15/packages/esp32/hardware/esp32/3.0.7
ESP32_DIR="$(ls -1d "$ESP32_HW_BASE"/*/ 2>/dev/null | sort -V | tail -n 1 || true)"
ESP32_DIR="${ESP32_DIR%/}"

if [[ -z "$ESP32_DIR" || ! -d "$ESP32_DIR" ]]; then
  echo "Error: could not locate an installed ESP32 core version under $ESP32_HW_BASE" >&2
  exit 1
fi

PARTITION_DIR="$ESP32_DIR/tools/partitions"
BOARDS_TXT="$ESP32_DIR/boards.txt"

if [[ ! -d "$PARTITION_DIR" ]]; then
  echo "Error: partition directory not found: $PARTITION_DIR" >&2
  exit 1
fi

if [[ ! -f "$BOARDS_TXT" ]]; then
  echo "Error: boards.txt not found: $BOARDS_TXT" >&2
  exit 1
fi

echo "Installing custom partition table into Arduino ESP32 core..."
echo "- Core:       $ESP32_DIR"
echo "- Partitions: $PARTITION_DIR"

printf "\nInstalling custom partition schemes...\n"

# 1) Copy partition CSVs
cp "$PARTITION_FILE_SRC_C3" "$PARTITION_DIR/$PARTITION_FILE_BASENAME_C3"
echo "✓ Installed $PARTITION_FILE_BASENAME_C3"

cp "$PARTITION_FILE_SRC_S3" "$PARTITION_DIR/$PARTITION_FILE_BASENAME_S3"
echo "✓ Installed $PARTITION_FILE_BASENAME_S3"

# 2) Register PartitionScheme in boards.txt (idempotent)
# We key off the specific menu entry to avoid false positives.
printf "\nRegistering PartitionScheme menu entries...\n"

if grep -q "^${BOARD_ID_C3}\.menu\.PartitionScheme\.${PARTITION_SCHEME_ID_C3}=" "$BOARDS_TXT"; then
  echo "✓ PartitionScheme '$PARTITION_SCHEME_ID_C3' already registered in boards.txt"
else
  {
    echo ""
    echo "# Custom OTA partition scheme (installed by $REPO_ROOT/tools/install-custom-partitions.sh)"
    echo "${BOARD_ID_C3}.menu.PartitionScheme.${PARTITION_SCHEME_ID_C3}=Custom OTA (1.9MB APP×2)"
    echo "${BOARD_ID_C3}.menu.PartitionScheme.${PARTITION_SCHEME_ID_C3}.build.partitions=${PARTITION_NAME_NO_EXT_C3}"
    echo "${BOARD_ID_C3}.menu.PartitionScheme.${PARTITION_SCHEME_ID_C3}.upload.maximum_size=${UPLOAD_MAX_SIZE_C3}"
  } >> "$BOARDS_TXT"

  echo "✓ Registered PartitionScheme '$PARTITION_SCHEME_ID_C3' for board '$BOARD_ID_C3'"
fi

if grep -q "^${BOARD_ID_S3}\.menu\.PartitionScheme\.${PARTITION_SCHEME_ID_S3}=" "$BOARDS_TXT"; then
  echo "✓ PartitionScheme '$PARTITION_SCHEME_ID_S3' already registered in boards.txt"
else
  {
    echo ""
    echo "# Custom 16MB partition scheme w/ larger NVS (installed by $REPO_ROOT/tools/install-custom-partitions.sh)"
    echo "${BOARD_ID_S3}.menu.PartitionScheme.${PARTITION_SCHEME_ID_S3}=3MB APP×2 + FFat (Big NVS)"
    echo "${BOARD_ID_S3}.menu.PartitionScheme.${PARTITION_SCHEME_ID_S3}.build.partitions=${PARTITION_NAME_NO_EXT_S3}"
    echo "${BOARD_ID_S3}.menu.PartitionScheme.${PARTITION_SCHEME_ID_S3}.upload.maximum_size=${UPLOAD_MAX_SIZE_S3}"
  } >> "$BOARDS_TXT"

  echo "✓ Registered PartitionScheme '$PARTITION_SCHEME_ID_S3' for board '$BOARD_ID_S3'"
fi

echo "Done."