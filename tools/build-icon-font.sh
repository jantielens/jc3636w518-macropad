#!/bin/bash
#
# Build Material Design Icon Font for LVGL
# Generates icon font file and header from icons.json
#

set -e  # Exit on error

# Resolve script directory (absolute path)
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

# Paths
ICONS_JSON="$PROJECT_ROOT/icons.json"
CACHE_DIR="$SCRIPT_DIR/.cache"
TTF_FILE="$CACHE_DIR/materialdesignicons-webfont.ttf"
OUTPUT_C="$PROJECT_ROOT/src/app/material_icons_48.c"
OUTPUT_H="$PROJECT_ROOT/src/app/material_icons.h"

# Material Design Icons download URL
MDI_VERSION="7.4.47"
MDI_DOWNLOAD_URL="https://github.com/Templarian/MaterialDesign-Webfont/raw/v${MDI_VERSION}/fonts/materialdesignicons-webfont.ttf"

echo "=== Material Design Icon Font Builder ==="
echo "Project root: $PROJECT_ROOT"
echo "Icons config: $ICONS_JSON"
echo

# Check if icons.json exists
if [[ ! -f "$ICONS_JSON" ]]; then
  echo "ERROR: icons.json not found at $ICONS_JSON"
  exit 1
fi

# Create cache directory
mkdir -p "$CACHE_DIR"

# Download TTF if not cached
if [[ ! -f "$TTF_FILE" ]]; then
  echo "Downloading Material Design Icons font..."
  echo "  URL: $MDI_DOWNLOAD_URL"
  if command -v curl &> /dev/null; then
    curl -L -o "$TTF_FILE" "$MDI_DOWNLOAD_URL"
  elif command -v wget &> /dev/null; then
    wget -O "$TTF_FILE" "$MDI_DOWNLOAD_URL"
  else
    echo "ERROR: curl or wget required to download font"
    exit 1
  fi
  echo "  Downloaded: $TTF_FILE"
else
  echo "Using cached font: $TTF_FILE"
fi

# Check Node.js
if ! command -v node &> /dev/null; then
  echo "ERROR: Node.js is required but not installed"
  echo "Install: sudo apt install nodejs npm"
  exit 1
fi

# Check/install lv_font_conv
if ! command -v lv_font_conv &> /dev/null; then
  echo "Installing lv_font_conv..."
  npm install -g lv_font_conv
fi

# Parse icons.json to extract codepoints and convert to UTF-8 characters
echo
echo "Parsing icons.json..."

# Create a temp file with UTF-8 characters for lv_font_conv
TEMP_CHARS=$(mktemp)
ICON_COUNT=$(node -e "
const fs = require('fs');
const data = JSON.parse(fs.readFileSync('$ICONS_JSON', 'utf8'));
let count = 0;
let chars = '';

for (const [key, value] of Object.entries(data)) {
  if (!key.startsWith('_') && !key.startsWith('comment_') && typeof value === 'string') {
    const codepoint = parseInt(value, 16);
    chars += String.fromCodePoint(codepoint);
    count++;
  }
}

// Write UTF-8 characters to stdout
process.stdout.write(chars);

// Write count to stderr so we can capture it separately
process.stderr.write(count.toString());
" 2>&1 1>"$TEMP_CHARS")

if [[ -z "$ICON_COUNT" ]] || [[ "$ICON_COUNT" == "0" ]]; then
  echo "ERROR: No valid icons found in icons.json"
  rm -f "$TEMP_CHARS"
  exit 1
fi

echo "Found $ICON_COUNT icons"

# Generate LVGL font with UTF-8 characters
echo
echo "Generating LVGL font..."
lv_font_conv \
  --font "$TTF_FILE" \
  --size 48 \
  --format lvgl \
  --bpp 4 \
  --no-compress \
  --lv-include lvgl.h \
  --output "$OUTPUT_C" \
  --symbols "$(cat "$TEMP_CHARS")"

# Clean up temp file
rm -f "$TEMP_CHARS"

if [[ ! -f "$OUTPUT_C" ]]; then
  echo "ERROR: Font generation failed"
  exit 1
fi

echo "  Generated: $OUTPUT_C"

# Generate header file with icon defines
echo
echo "Generating header file..."

cat > "$OUTPUT_H" << 'EOF'
/*
 * Material Design Icons Font Header
 * Auto-generated from icons.json - DO NOT EDIT MANUALLY
 */

#ifndef MATERIAL_ICONS_H
#define MATERIAL_ICONS_H

#include <lvgl.h>

// Font declaration
extern const lv_font_t material_icons_48;

// Icon defines (UTF-8 encoded glyphs)
EOF

# Generate icon defines from icons.json
node -e "
const fs = require('fs');
const data = JSON.parse(fs.readFileSync('$ICONS_JSON', 'utf8'));

function hexToUtf8(hex) {
  const codepoint = parseInt(hex, 16);
  if (codepoint <= 0x7F) {
    return '\\\\x' + hex.substring(hex.length - 2).padStart(2, '0');
  } else if (codepoint <= 0x7FF) {
    const b1 = 0xC0 | (codepoint >> 6);
    const b2 = 0x80 | (codepoint & 0x3F);
    return '\\\\x' + b1.toString(16) + '\\\\x' + b2.toString(16);
  } else if (codepoint <= 0xFFFF) {
    const b1 = 0xE0 | (codepoint >> 12);
    const b2 = 0x80 | ((codepoint >> 6) & 0x3F);
    const b3 = 0x80 | (codepoint & 0x3F);
    return '\\\\x' + b1.toString(16) + '\\\\x' + b2.toString(16) + '\\\\x' + b3.toString(16);
  } else {
    const b1 = 0xF0 | (codepoint >> 18);
    const b2 = 0x80 | ((codepoint >> 12) & 0x3F);
    const b3 = 0x80 | ((codepoint >> 6) & 0x3F);
    const b4 = 0x80 | (codepoint & 0x3F);
    return '\\\\x' + b1.toString(16) + '\\\\x' + b2.toString(16) + '\\\\x' + b3.toString(16) + '\\\\x' + b4.toString(16);
  }
}

for (const [key, value] of Object.entries(data)) {
  if (!key.startsWith('_') && !key.startsWith('comment_') && typeof value === 'string') {
    const define_name = 'ICON_' + key.toUpperCase().replace(/-/g, '_');
    const utf8_bytes = hexToUtf8(value);
    console.log('#define ' + define_name + ' \"' + utf8_bytes + '\"');
  }
}
" >> "$OUTPUT_H"

cat >> "$OUTPUT_H" << 'EOF'

#endif // MATERIAL_ICONS_H
EOF

echo "  Generated: $OUTPUT_H"

# Generate icon lookup implementation
echo
echo "Generating icon lookup..."

OUTPUT_LOOKUP_CPP="$PROJECT_ROOT/src/app/icon_lookup.cpp"

cat > "$OUTPUT_LOOKUP_CPP" << 'EOF'
/*
 * Icon Lookup Helper
 * Maps icon names to UTF-8 encoded Material Design Icons glyphs
 * Auto-generated from icons.json - DO NOT EDIT MANUALLY
 */

#include "icon_lookup.h"
#include "material_icons.h"
#include <string.h>

// Icon name to UTF-8 glyph mapping
struct IconMapping {
  const char* name;
  const char* glyph;
};

// Icon lookup table (auto-generated, sorted alphabetically)
static const IconMapping ICON_LOOKUP_TABLE[] = {
EOF

# Generate icon mappings from icons.json (sorted alphabetically)
node -e "
const fs = require('fs');
const data = JSON.parse(fs.readFileSync('$ICONS_JSON', 'utf8'));

// Extract icon names and sort alphabetically
const icons = [];
for (const [key, value] of Object.entries(data)) {
  if (!key.startsWith('_') && !key.startsWith('comment_') && typeof value === 'string') {
    icons.push(key);
  }
}
icons.sort();

// Generate mappings
for (const iconName of icons) {
  const defineName = 'ICON_' + iconName.toUpperCase().replace(/-/g, '_');
  console.log('  {\"' + iconName + '\", ' + defineName + '},');
}
" >> "$OUTPUT_LOOKUP_CPP"

cat >> "$OUTPUT_LOOKUP_CPP" << 'EOF'
};

static const int ICON_LOOKUP_TABLE_SIZE = sizeof(ICON_LOOKUP_TABLE) / sizeof(IconMapping);

const char* icon_lookup(const char* icon_name) {
  if (!icon_name || strlen(icon_name) == 0) {
    return nullptr;
  }
  
  // Linear search (250 icons is small enough, binary search not needed)
  for (int i = 0; i < ICON_LOOKUP_TABLE_SIZE; i++) {
    if (strcmp(icon_name, ICON_LOOKUP_TABLE[i].name) == 0) {
      return ICON_LOOKUP_TABLE[i].glyph;
    }
  }
  
  return nullptr;  // Icon not found
}
EOF

echo "  Generated: $OUTPUT_LOOKUP_CPP"

# Generate icon reference markdown
echo
echo "Generating icon reference..."

OUTPUT_ICON_REF="$PROJECT_ROOT/docs/icon-reference.md"

# Calculate file size before writing
FILE_SIZE=$(stat -f%z "$OUTPUT_C" 2>/dev/null || stat -c%s "$OUTPUT_C")
FILE_SIZE_KB=$((FILE_SIZE / 1024))

# Write header with variable substitution
cat > "$OUTPUT_ICON_REF" << HEADER
# Icon Reference

Auto-generated list of all Material Design Icons available in the macropad firmware.

**Total Icons:** $ICON_COUNT

**Font Size:** ${FILE_SIZE_KB} KB

**Source:** [Material Design Icons v7.4.47](https://pictogrammers.com/library/mdi/)

> **Note:** Icon glyphs in the first column may display as empty boxes in VS Code or other editors if the Material Design Icons font is not installed on your system. The codepoint column shows the Unicode value for reference. To preview icons visually, view this file on GitHub or visit the [Material Design Icons website](https://pictogrammers.com/library/mdi/).
HEADER

# Append rest without variable substitution (contains code blocks with backticks)
cat >> "$OUTPUT_ICON_REF" << 'EOF'

---

## Available Icons

Icons are organized by category for easy reference. Use the icon name when configuring your macropad buttons.

EOF

# Generate categorized icon listings from icons.json using temp file
TEMP_JS=$(mktemp)
cat > "$TEMP_JS" << 'NODEJS'
const fs = require('fs');
const iconsJsonPath = process.argv[2];
const data = JSON.parse(fs.readFileSync(iconsJsonPath, 'utf8'));

// Group icons by category (based on comment_ keys) and store with codepoints
const categories = {};
let currentCategory = 'Other';

for (const [key, value] of Object.entries(data)) {
  if (key.startsWith('comment_')) {
    // Extract category name from comment
    currentCategory = value;
  } else if (!key.startsWith('_') && typeof value === 'string') {
    if (!categories[currentCategory]) {
      categories[currentCategory] = [];
    }
    categories[currentCategory].push({ name: key, codepoint: value });
  }
}

// Output markdown tables for each category with actual icon glyphs
for (const [category, icons] of Object.entries(categories)) {
  console.log('### ' + category + '\n');
  console.log('| Icon | Icon Name | Codepoint |');
  console.log('|------|-----------|-----------|');
  
  for (const icon of icons) {
    const codepoint = parseInt(icon.codepoint, 16);
    const glyph = String.fromCodePoint(codepoint);
    const codepointHex = 'U+' + icon.codepoint;
    console.log('| ' + glyph + ' | `' + icon.name + '` | `' + codepointHex + '` |');
  }
  
  console.log('');
}
NODEJS

node "$TEMP_JS" "$ICONS_JSON" >> "$OUTPUT_ICON_REF"
rm -f "$TEMP_JS"

echo "  Generated: $OUTPUT_ICON_REF"

# Summary
echo
echo "✓ Icon font generation complete"
echo "  Icons: $ICON_COUNT"
echo "  Font file: $FILE_SIZE_KB KB"
echo

