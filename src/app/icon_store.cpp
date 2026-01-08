#include "icon_store.h"

#if HAS_DISPLAY && HAS_ICONS

#include <Arduino.h>
#include <FFat.h>
#include <esp_partition.h>

#include <string.h>

namespace {

static portMUX_TYPE g_icons_op_mux = portMUX_INITIALIZER_UNLOCKED;
static bool g_icons_op_in_progress = false;

static bool icons_begin_op(char* err, size_t err_len, const char* msg_busy) {
    bool busy = false;
    portENTER_CRITICAL(&g_icons_op_mux);
    busy = g_icons_op_in_progress;
    if (!busy) g_icons_op_in_progress = true;
    portEXIT_CRITICAL(&g_icons_op_mux);

    if (busy) {
        if (err && err_len) strlcpy(err, msg_busy ? msg_busy : "Icons busy", err_len);
        return false;
    }
    return true;
}

static void icons_end_op() {
    portENTER_CRITICAL(&g_icons_op_mux);
    g_icons_op_in_progress = false;
    portEXIT_CRITICAL(&g_icons_op_mux);
}

static bool ffat_attempted = false;
static bool ffat_ready = false;

static bool ensure_ffat() {
    if (ffat_attempted) return ffat_ready;
    ffat_attempted = true;

    const esp_partition_t* ffat_part = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA,
        ESP_PARTITION_SUBTYPE_DATA_FAT,
        "ffat");

    if (!ffat_part) {
        ffat_ready = false;
        return false;
    }

    ffat_ready = FFat.begin(false);
    return ffat_ready;
}

static bool is_safe_icon_id(const char* s) {
    if (!s || !*s) return false;
    for (const char* p = s; *p; p++) {
        const char c = *p;
        const bool ok = (c >= 'a' && c <= 'z')
            || (c >= '0' && c <= '9')
            || (c == '_');
        if (!ok) return false;
    }
    return true;
}

struct IconFileHeader {
    char magic[4];      // "ICN1"
    uint16_t width;     // LE
    uint16_t height;    // LE
    uint8_t format;     // 1 = RGB565+Alpha
    uint8_t reserved[3];
    uint32_t data_len;  // LE
};

static uint16_t read_u16_le(const uint8_t* p) {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t read_u32_le(const uint8_t* p) {
    return (uint32_t)p[0]
        | ((uint32_t)p[1] << 8)
        | ((uint32_t)p[2] << 16)
        | ((uint32_t)p[3] << 24);
}

struct CacheEntry {
    bool in_use;
    char id[32];
    lv_img_dsc_t dsc;
    uint8_t* data;
    size_t data_len;
    uint32_t last_used_ms;
};

static CacheEntry g_cache[4];

static void cache_free(CacheEntry& e) {
    if (e.data) {
        free(e.data);
        e.data = nullptr;
    }
    e.in_use = false;
    e.id[0] = '\0';
    e.data_len = 0;
    e.last_used_ms = 0;
    memset(&e.dsc, 0, sizeof(e.dsc));
}

static CacheEntry* cache_find(const char* id) {
    for (auto& e : g_cache) {
        if (!e.in_use) continue;
        if (strcmp(e.id, id) == 0) return &e;
    }
    return nullptr;
}

static CacheEntry* cache_alloc_slot() {
    for (auto& e : g_cache) {
        if (!e.in_use) return &e;
    }

    // Evict LRU
    CacheEntry* victim = &g_cache[0];
    for (auto& e : g_cache) {
        if (e.last_used_ms < victim->last_used_ms) victim = &e;
    }
    cache_free(*victim);
    return victim;
}

static bool load_icon_file_to_cache(const char* icon_id, IconRef* out) {
    if (!ensure_ffat()) return false;

    char path[80];
    snprintf(path, sizeof(path), "/icons/%s.bin", icon_id);

    File f = FFat.open(path, "r");
    if (!f) return false;

    const size_t file_size = (size_t)f.size();
    if (file_size < sizeof(IconFileHeader) || file_size > (256 * 1024)) {
        f.close();
        return false;
    }

    uint8_t hdr_buf[sizeof(IconFileHeader)];
    if (f.read(hdr_buf, sizeof(hdr_buf)) != (int)sizeof(hdr_buf)) {
        f.close();
        return false;
    }

    if (!(hdr_buf[0] == 'I' && hdr_buf[1] == 'C' && hdr_buf[2] == 'N' && hdr_buf[3] == '1')) {
        f.close();
        return false;
    }

    const uint16_t w = read_u16_le(hdr_buf + 4);
    const uint16_t h = read_u16_le(hdr_buf + 6);
    const uint8_t fmt = hdr_buf[8];
    const uint32_t data_len = read_u32_le(hdr_buf + 12);

    if (fmt != 1) {
        f.close();
        return false;
    }
    if (w == 0 || h == 0 || w > 256 || h > 256) {
        f.close();
        return false;
    }

    const size_t expected_payload = (size_t)w * (size_t)h * 3;
    if (data_len != expected_payload) {
        f.close();
        return false;
    }

    if (sizeof(IconFileHeader) + (size_t)data_len != file_size) {
        f.close();
        return false;
    }

    uint8_t* payload = nullptr;
#if SOC_SPIRAM_SUPPORTED
    if (psramFound()) {
        payload = (uint8_t*)heap_caps_malloc(data_len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
#endif
    if (!payload) payload = (uint8_t*)malloc(data_len);
    if (!payload) {
        f.close();
        return false;
    }

    const int rd = f.read(payload, data_len);
    f.close();
    if (rd != (int)data_len) {
        free(payload);
        return false;
    }

    CacheEntry* slot = cache_alloc_slot();
    cache_free(*slot);

    slot->in_use = true;
    strlcpy(slot->id, icon_id, sizeof(slot->id));
    slot->data = payload;
    slot->data_len = data_len;
    slot->last_used_ms = millis();

    slot->dsc.header.cf = LV_IMG_CF_TRUE_COLOR_ALPHA;
    slot->dsc.header.always_zero = 0;
    slot->dsc.header.reserved = 0;
    slot->dsc.header.w = w;
    slot->dsc.header.h = h;
    slot->dsc.data_size = data_len;
    slot->dsc.data = payload;

    out->dsc = &slot->dsc;
    out->kind = IconKind::Color;
    return true;
}

static void set_err(char* err, size_t err_len, const char* msg) {
    if (!err || err_len == 0) return;
    strlcpy(err, msg ? msg : "", err_len);
}

static bool has_prefix(const char* s, const char* prefix) {
    if (!s || !prefix) return false;
    while (*prefix) {
        if (*s != *prefix) return false;
        s++;
        prefix++;
    }
    return true;
}

struct KeepSet {
    static constexpr size_t kMax = (size_t)MACROS_SCREEN_COUNT * (size_t)MACROS_BUTTONS_PER_SCREEN;
    char ids[kMax][MACROS_ICON_ID_MAX_LEN];
    size_t count = 0;

    bool contains(const char* id) const {
        if (!id || !*id) return false;
        for (size_t i = 0; i < count; i++) {
            if (strcmp(ids[i], id) == 0) return true;
        }
        return false;
    }

    void add(const char* id) {
        if (!id || !*id) return;
        if (contains(id)) return;
        if (count >= kMax) return;
        strlcpy(ids[count], id, sizeof(ids[count]));
        count++;
    }
};

} // namespace

bool icon_store_ffat_ready() {
    return ensure_ffat();
}

bool icon_store_lookup(const char* icon_id, IconRef* out) {
    if (!out) return false;

    // Prefer compiled icons.
    if (icon_registry_lookup(icon_id, out)) return true;

    if (!icon_id || !*icon_id) return false;

    // Cache.
    if (CacheEntry* e = cache_find(icon_id)) {
        e->last_used_ms = millis();
        out->dsc = &e->dsc;
        out->kind = IconKind::Color;
        return true;
    }

    return load_icon_file_to_cache(icon_id, out);
}

bool icon_store_install_blob(const char* icon_id, const uint8_t* blob, size_t blob_len, char* err, size_t err_len) {
    set_err(err, err_len, "");

    if (!is_safe_icon_id(icon_id)) {
        set_err(err, err_len, "Invalid icon id (expected [a-z0-9_]+)");
        return false;
    }

    if (!ensure_ffat()) {
        set_err(err, err_len, "FFat not available on this partition scheme");
        return false;
    }

    // Validate blob before taking the global icon op lock.
    if (!blob || blob_len < sizeof(IconFileHeader) || blob_len > (256 * 1024)) {
        set_err(err, err_len, "Invalid blob");
        return false;
    }

    if (!(blob[0] == 'I' && blob[1] == 'C' && blob[2] == 'N' && blob[3] == '1')) {
        set_err(err, err_len, "Bad magic");
        return false;
    }

    const uint16_t w = read_u16_le(blob + 4);
    const uint16_t h = read_u16_le(blob + 6);
    const uint8_t fmt = blob[8];
    const uint32_t data_len = read_u32_le(blob + 12);

    if (fmt != 1) {
        set_err(err, err_len, "Unsupported format");
        return false;
    }
    if (w == 0 || h == 0 || w > 256 || h > 256) {
        set_err(err, err_len, "Invalid dimensions");
        return false;
    }

    const size_t expected_payload = (size_t)w * (size_t)h * 3;
    if (data_len != expected_payload) {
        set_err(err, err_len, "Unexpected payload size");
        return false;
    }

    if ((size_t)sizeof(IconFileHeader) + (size_t)data_len != blob_len) {
        set_err(err, err_len, "Blob length mismatch");
        return false;
    }

    if (!icons_begin_op(err, err_len, "Icon operation in progress")) {
        return false;
    }

    struct IconsOpGuard {
        ~IconsOpGuard() { icons_end_op(); }
    } guard;

    // Ensure /icons exists.
    if (!FFat.exists("/icons")) {
        FFat.mkdir("/icons");
    }

    char path[80];
    snprintf(path, sizeof(path), "/icons/%s.bin", icon_id);

    File f = FFat.open(path, "w");
    if (!f) {
        set_err(err, err_len, "Failed to open file for writing");
        return false;
    }

    const size_t written = f.write(blob, blob_len);
    f.close();

    if (written != blob_len) {
        set_err(err, err_len, "Short write");
        return false;
    }

    // Drop from cache so next lookup reloads cleanly.
    if (CacheEntry* e = cache_find(icon_id)) {
        cache_free(*e);
    }

    return true;
}

bool icon_store_gc_unused_from_macros(const MacroConfig* cfg, size_t* out_deleted_count, size_t* out_bytes_freed, char* err, size_t err_len) {
    set_err(err, err_len, "");
    if (out_deleted_count) *out_deleted_count = 0;
    if (out_bytes_freed) *out_bytes_freed = 0;

    if (!cfg) {
        set_err(err, err_len, "Missing macro config");
        return false;
    }

    if (!ensure_ffat()) {
        set_err(err, err_len, "FFat not available on this partition scheme");
        return false;
    }

    if (!icons_begin_op(err, err_len, "Icon operation in progress")) {
        return false;
    }

    if (!FFat.exists("/icons")) {
        // Nothing to delete.
        icons_end_op();
        return true;
    }

    KeepSet keep;
    for (size_t s = 0; s < (size_t)MACROS_SCREEN_COUNT; s++) {
        for (size_t b = 0; b < (size_t)MACROS_BUTTONS_PER_SCREEN; b++) {
            const auto& icon = cfg->buttons[s][b].icon;
            if (icon.type == MacroIconType::Emoji || icon.type == MacroIconType::Asset) {
                keep.add(icon.id);
            }
        }
    }

    File dir = FFat.open("/icons");
    if (!dir || !dir.isDirectory()) {
        icons_end_op();
        return true;
    }

    size_t deleted = 0;
    size_t bytes = 0;

    File f = dir.openNextFile();
    while (f) {
        if (!f.isDirectory()) {
            const char* name = f.name();
            const size_t sz = (size_t)f.size();

            const char* base = name ? strrchr(name, '/') : nullptr;
            base = base ? (base + 1) : name;
            const char* ext = base ? strrchr(base, '.') : nullptr;

            if (base && ext && strcmp(ext, ".bin") == 0) {
                const size_t base_len = (size_t)(ext - base);
                if (base_len > 0 && base_len < 64) {
                    char id[64];
                    memset(id, 0, sizeof(id));
                    memcpy(id, base, base_len);
                    id[base_len] = '\0';

                    const bool managed = has_prefix(id, "emoji_") || has_prefix(id, "user_");
                    if (managed && !keep.contains(id)) {
                        char path[96];
                        snprintf(path, sizeof(path), "/icons/%s.bin", id);
                        if (FFat.remove(path)) {
                            deleted++;
                            bytes += sz;
                        }
                    }
                }
            }
        }
        f = dir.openNextFile();
    }

    if (out_deleted_count) *out_deleted_count = deleted;
    if (out_bytes_freed) *out_bytes_freed = bytes;

    icons_end_op();
    return true;
}

size_t icon_store_list_installed(char* out_json, size_t out_json_len) {
    if (!out_json || out_json_len == 0) return 0;
    out_json[0] = '\0';

    if (!ensure_ffat()) {
        strlcpy(out_json, "{\"success\":true,\"source\":\"ffat\",\"icons\":[]}", out_json_len);
        return 0;
    }

    if (!FFat.exists("/icons")) {
        strlcpy(out_json, "{\"success\":true,\"source\":\"ffat\",\"icons\":[]}", out_json_len);
        return 0;
    }

    File dir = FFat.open("/icons");
    if (!dir || !dir.isDirectory()) {
        strlcpy(out_json, "{\"success\":true,\"source\":\"ffat\",\"icons\":[]}", out_json_len);
        return 0;
    }

    // Very small JSON builder (bounded output).
    size_t count = 0;
    size_t used = 0;
    bool overflow = false;
    auto append = [&](const char* s) -> bool {
        if (!s || overflow) return false;
        const size_t n = strlen(s);
        if (used + n + 1 >= out_json_len) {
            overflow = true;
            return false;
        }
        memcpy(out_json + used, s, n);
        used += n;
        out_json[used] = '\0';
        return true;
    };

    append("{\"success\":true,\"source\":\"ffat\",\"icons\":[");

    File f = dir.openNextFile();
    bool first = true;
    while (f && !overflow) {
        if (!f.isDirectory()) {
            const char* name = f.name();
            if (name) {
                const char* base = strrchr(name, '/');
                base = base ? (base + 1) : name;
                const char* ext = strrchr(base, '.');
                const size_t base_len = ext ? (size_t)(ext - base) : strlen(base);

                if (ext && strcmp(ext, ".bin") == 0 && base_len > 0 && base_len < 64) {
                    char tmp[64];
                    memset(tmp, 0, sizeof(tmp));
                    memcpy(tmp, base, base_len);
                    tmp[base_len] = '\0';

                    char item[120];
                    // `tmp` is derived from filename and restricted to [a-z0-9_]+, so no escaping needed.
                    snprintf(item, sizeof(item), "{\"id\":\"%s\",\"kind\":\"color\"}", tmp);

                    if (!first) {
                        if (!append(",")) break;
                    }
                    if (!append(item)) break;
                    first = false;
                    count++;
                }
            }
        }
        f = dir.openNextFile();
    }

    // Always close JSON (if we can). If we overflowed, the closing may not fit.
    append("]}");

    if (overflow) {
        // Best-effort valid JSON for tiny buffers.
        if (out_json_len >= 13) {
            strlcpy(out_json, "{\"success\":true,\"source\":\"ffat\",\"icons\":[]}", out_json_len);
        }
    }

    return count;
}

#endif // HAS_DISPLAY && HAS_ICONS
