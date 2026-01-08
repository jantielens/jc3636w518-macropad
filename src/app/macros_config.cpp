#include "macros_config.h"

#include "log_manager.h"

#include <Preferences.h>

#if defined(ARDUINO_ARCH_ESP32)
// Prefer filesystem storage when available (e.g. 16MB + FFat partition schemes).
// This avoids NVS partition size limits for the ~22KB macro blob.
#include <FFat.h>
#include <esp_log.h>
#include <esp_partition.h>
#endif

#define MACROS_NAMESPACE "macros"

// NVS keys (must be <= 15 chars for NVS key limits)
#define KEY_MAGIC "mg"
#define KEY_VER   "v"
#define KEY_BLOB  "b"

#define MACROS_MAGIC 0x4D414352u // 'MACR'
#define MACROS_VERSION 8

static Preferences prefs;

// ===== Optional FFat storage (preferred when mounted) =====
static bool ffat_attempted = false;
static bool ffat_ready = false;
static const char* kMacrosPath = "/macros.bin";
static const char* kMacrosTmpPath = "/macros.tmp";

struct MacrosFileHeader {
    uint32_t magic;
    uint8_t version;
    uint8_t reserved0;
    uint16_t reserved1;
    uint32_t size;
};

static bool ensure_ffat() {
#if defined(ARDUINO_ARCH_ESP32)
    if (ffat_attempted) return ffat_ready;
    ffat_attempted = true;

    // Only try mounting if an FFat partition exists.
    // If the partition doesn't exist, calling FFat.begin() can emit scary FatFS errors.
    const esp_partition_t* ffat_part = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA,
        ESP_PARTITION_SUBTYPE_DATA_FAT,
        "ffat");
    if (!ffat_part) {
        ffat_ready = false;
        return false;
    }

    // Try mounting without formatting first.
    const esp_log_level_t prev = esp_log_level_get("vfs_fat_spiflash");
    esp_log_level_set("vfs_fat_spiflash", ESP_LOG_NONE);
    ffat_ready = FFat.begin(false);
    esp_log_level_set("vfs_fat_spiflash", prev);

    // If the partition exists but is not formatted yet, auto-format so we can
    // store large macro payloads (NVS has practical blob-size limits).
    if (!ffat_ready) {
        Logger.logLine("[Macros] FFat mount failed; formatting...");
        const esp_log_level_t prev2 = esp_log_level_get("vfs_fat_spiflash");
        esp_log_level_set("vfs_fat_spiflash", ESP_LOG_NONE);
        ffat_ready = FFat.begin(true);
        esp_log_level_set("vfs_fat_spiflash", prev2);

        if (!ffat_ready) {
            Logger.logLine("[Macros] FFat not available; using NVS");
        }
    }
    return ffat_ready;
#else
    return false;
#endif
}

static bool macros_load_from_ffat(MacroConfig* cfg) {
    if (!cfg) return false;
    if (!ensure_ffat()) return false;

#if defined(ARDUINO_ARCH_ESP32)
    File f = FFat.open(kMacrosPath, FILE_READ);
    if (!f) return false;

    MacrosFileHeader hdr;
    if (f.readBytes(reinterpret_cast<char*>(&hdr), sizeof(hdr)) != sizeof(hdr)) {
        f.close();
        return false;
    }

    const size_t expected = sizeof(MacroConfig);
    if (hdr.magic != MACROS_MAGIC || hdr.version != MACROS_VERSION || hdr.size != expected) {
        f.close();
        return false;
    }

    const size_t read = f.readBytes(reinterpret_cast<char*>(cfg), expected);
    f.close();
    return read == expected;
#else
    return false;
#endif
}

static bool macros_save_to_ffat(const MacroConfig* cfg) {
    if (!cfg) return false;
    if (!ensure_ffat()) return false;

#if defined(ARDUINO_ARCH_ESP32)
    // Write to a temp file then rename for basic atomicity.
    FFat.remove(kMacrosTmpPath);
    File f = FFat.open(kMacrosTmpPath, FILE_WRITE);
    if (!f) return false;

    MacrosFileHeader hdr;
    hdr.magic = MACROS_MAGIC;
    hdr.version = MACROS_VERSION;
    hdr.reserved0 = 0;
    hdr.reserved1 = 0;
    hdr.size = (uint32_t)sizeof(MacroConfig);

    if (f.write(reinterpret_cast<const uint8_t*>(&hdr), sizeof(hdr)) != sizeof(hdr)) {
        f.close();
        FFat.remove(kMacrosTmpPath);
        return false;
    }

    const size_t expected = sizeof(MacroConfig);
    if (f.write(reinterpret_cast<const uint8_t*>(cfg), expected) != expected) {
        f.close();
        FFat.remove(kMacrosTmpPath);
        return false;
    }

    f.flush();
    f.close();

    FFat.remove(kMacrosPath);
    if (!FFat.rename(kMacrosTmpPath, kMacrosPath)) {
        FFat.remove(kMacrosTmpPath);
        return false;
    }

    return true;
#else
    return false;
#endif
}

static bool macros_reset_ffat() {
    if (!ensure_ffat()) return false;
#if defined(ARDUINO_ARCH_ESP32)
    const bool ok1 = !FFat.exists(kMacrosPath) || FFat.remove(kMacrosPath);
    const bool ok2 = !FFat.exists(kMacrosTmpPath) || FFat.remove(kMacrosTmpPath);
    return ok1 && ok2;
#else
    return false;
#endif
}

void macros_config_set_defaults(MacroConfig* cfg) {
    if (!cfg) return;
    memset(cfg, 0, sizeof(MacroConfig));

    // Global defaults (RGB only: 0xRRGGBB)
    cfg->default_screen_bg = 0x000000; // black
    cfg->default_button_bg = 0x1E1E1E; // rgb(30,30,30)
    cfg->default_icon_color = 0xFFFFFF; // white
    cfg->default_label_color = 0xFFFFFF; // white

    for (int s = 0; s < MACROS_SCREEN_COUNT; s++) {
        cfg->screen_bg[s] = MACROS_COLOR_UNSET;
    }

    for (int s = 0; s < MACROS_SCREEN_COUNT; s++) {
        strlcpy(cfg->template_id[s], "round_ring_9", sizeof(cfg->template_id[s]));
    }

    for (int s = 0; s < MACROS_SCREEN_COUNT; s++) {
        for (int b = 0; b < MACROS_BUTTONS_PER_SCREEN; b++) {
            cfg->buttons[s][b].action = MacroButtonAction::None;
            cfg->buttons[s][b].label[0] = '\0';
            cfg->buttons[s][b].payload[0] = '\0';

            cfg->buttons[s][b].icon.type = MacroIconType::None;
            cfg->buttons[s][b].icon.id[0] = '\0';
            cfg->buttons[s][b].icon.display[0] = '\0';

            cfg->buttons[s][b].button_bg = MACROS_COLOR_UNSET;
            cfg->buttons[s][b].icon_color = MACROS_COLOR_UNSET;
            cfg->buttons[s][b].label_color = MACROS_COLOR_UNSET;
        }
    }
}

static bool begin_readonly() {
    return prefs.begin(MACROS_NAMESPACE, true);
}

static bool begin_readwrite() {
    return prefs.begin(MACROS_NAMESPACE, false);
}

bool macros_config_load(MacroConfig* cfg) {
    if (!cfg) return false;

    Logger.logBegin("Macros Load");

    // Prefer FFat when available (avoids NVS size limits on large macro payloads).
    if (macros_load_from_ffat(cfg)) {
        Logger.logEnd("OK (FFat)");
        return true;
    }

    if (!begin_readonly()) {
        Logger.logEnd("Preferences begin failed");
        macros_config_set_defaults(cfg);
        return false;
    }

    const uint32_t magic = prefs.getUInt(KEY_MAGIC, 0);
    const uint8_t version = prefs.getUChar(KEY_VER, 0);

    if (magic != MACROS_MAGIC || version != MACROS_VERSION) {
        prefs.end();
        Logger.logEnd("No macros config");
        macros_config_set_defaults(cfg);
        return false;
    }

    const size_t expected = sizeof(MacroConfig);
    const size_t got = prefs.getBytesLength(KEY_BLOB);
    if (got == 0) {
        prefs.end();
        Logger.logEnd("No macros config");
        macros_config_set_defaults(cfg);
        return false;
    }

    if (got != expected) {
        prefs.end();
        Logger.logLinef("Size mismatch: got=%u expected=%u", (unsigned)got, (unsigned)expected);

        // Most commonly this happens after a failed/partial save (e.g. old NVS too small)
        // or after a struct size change. Clear the namespace so we don't keep erroring.
        if (begin_readwrite()) {
            prefs.clear();
            prefs.end();
            Logger.logLine("Cleared stored macros (size mismatch)");
        }

        Logger.logEnd("Invalid macros config");
        macros_config_set_defaults(cfg);
        return false;
    }

    const size_t read = prefs.getBytes(KEY_BLOB, cfg, expected);
    prefs.end();

    if (read != expected) {
        Logger.logLinef("Read failed: %u/%u", (unsigned)read, (unsigned)expected);
        Logger.logEnd("Invalid macros config");
        macros_config_set_defaults(cfg);
        return false;
    }

    Logger.logEnd("OK");
    return true;
}

bool macros_config_save(const MacroConfig* cfg) {
    if (!cfg) return false;

    Logger.logBegin("Macros Save");

    // Prefer FFat when available.
    if (macros_save_to_ffat(cfg)) {
        Logger.logEnd("OK (FFat)");
        return true;
    }

    if (!begin_readwrite()) {
        Logger.logEnd("Preferences begin failed");
        return false;
    }

    const size_t expected = sizeof(MacroConfig);
    const size_t written = prefs.putBytes(KEY_BLOB, cfg, expected);

    // Only mark the blob as valid if the full write succeeded.
    if (written == expected) {
        prefs.putUInt(KEY_MAGIC, MACROS_MAGIC);
        prefs.putUChar(KEY_VER, MACROS_VERSION);
    }

    prefs.end();

    if (written != expected) {
        Logger.logLinef("Write failed: %u/%u", (unsigned)written, (unsigned)expected);
        if (written == 0) {
            Logger.logLine("Hint: NVS partition may be too small for macros blob");
        }
        Logger.logEnd("FAILED");
        return false;
    }

    Logger.logEnd("OK");
    return true;
}

bool macros_config_reset() {
    Logger.logBegin("Macros Reset");

    // Prefer removing the FFat file if present.
    if (macros_reset_ffat()) {
        Logger.logEnd("OK (FFat)");
        return true;
    }

    if (!begin_readwrite()) {
        Logger.logEnd("Preferences begin failed");
        return false;
    }

    const bool ok = prefs.clear();
    prefs.end();

    Logger.logEnd(ok ? "OK" : "FAILED");
    return ok;
}
