#pragma once

#include "board_config.h"

#if HAS_DISPLAY && HAS_ICONS

#include <stddef.h>
#include <stdint.h>

#include "icon_registry.h"
#include "macros_config.h"

#ifdef __cplusplus
extern "C" {
#endif

// Looks up a compiled icon first; if not found, tries to load a cached icon from FFat.
// Returns true on success.
bool icon_store_lookup(const char* icon_id, IconRef* out);

// Returns true if FFat is available for icon persistence.
bool icon_store_ffat_ready();

// Installs (stores) a pre-converted icon blob.
// Expected format:
// - header: magic "ICN1" (4 bytes)
// - width (u16 LE)
// - height (u16 LE)
// - format (u8): 1 = RGB565+Alpha (LV_IMG_CF_TRUE_COLOR_ALPHA-compatible, 3 bytes/pixel)
// - reserved (3 bytes)
// - data_len (u32 LE)
// - data payload
//
// Writes to /icons/<icon_id>.bin on FFat.
bool icon_store_install_blob(const char* icon_id, const uint8_t* blob, size_t blob_len, char* err, size_t err_len);

// Lists installed icon IDs (filenames under /icons/*.bin). Returns count written.
size_t icon_store_list_installed(char* out_json, size_t out_json_len);

// Deletes unused installed icons under /icons/*.bin based on the current macro configuration.
// Policy:
// - Only considers deleting managed prefixes (currently "emoji_" and "user_")
// - Keeps any icon IDs referenced by macro buttons where icon.type is Emoji or Asset
//
// Returns true on success. On failure, returns false and writes a short error message to `err`.
bool icon_store_gc_unused_from_macros(const MacroConfig* cfg, size_t* out_deleted_count, size_t* out_bytes_freed, char* err, size_t err_len);

#ifdef __cplusplus
}
#endif

#endif // HAS_DISPLAY && HAS_ICONS
