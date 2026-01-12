#include "watchlist_screen.h"

#if HAS_DISPLAY

#include "../display_manager.h"
#include "../log_manager.h"

#include <esp_heap_caps.h>
#include <esp32-hal-psram.h>

#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <WiFi.h>

#include <ctype.h>
#include <math.h>

namespace {
static constexpr uint32_t kUiUpdateIntervalMs = 1000;
static constexpr uint32_t kHttpTimeoutMs = 12000;

static constexpr size_t kMaxCsvPayload = 512;
static constexpr size_t kMaxJsonPayload = 256;

static constexpr float kTickEpsilon = 0.000001f;

static float parse_float_best_effort(const char* s) {
    if (!s) return NAN;
    char* end = nullptr;
    const float v = strtof(s, &end);
    if (end == s) return NAN;
    return v;
}

static bool csv_get_field(const char* line, int fieldIndex, char* out, size_t outLen) {
    if (!line || !out || outLen == 0) return false;

    int idx = 0;
    const char* start = line;
    const char* p = line;
    while (*p) {
        if (*p == ',') {
            if (idx == fieldIndex) {
                const size_t n = (size_t)(p - start);
                const size_t copyN = (n < outLen - 1) ? n : (outLen - 1);
                memcpy(out, start, copyN);
                out[copyN] = '\0';
                return true;
            }
            idx++;
            start = p + 1;
        }
        p++;
    }

    // Last field
    if (idx == fieldIndex) {
        const size_t n = (size_t)(p - start);
        const size_t copyN = (n < outLen - 1) ? n : (outLen - 1);
        memcpy(out, start, copyN);
        out[copyN] = '\0';
        return true;
    }

    return false;
}

static bool json_extract_usd(const char* json, float* out) {
    if (!json || !out) return false;

    // Very small best-effort parse for patterns like: {"bitcoin":{"usd":91361}}
    const char* p = strstr(json, "\"usd\"");
    if (!p) return false;

    p = strchr(p, ':');
    if (!p) return false;
    p++;

    while (*p == ' ' || *p == '\t') p++;

    const float v = parse_float_best_effort(p);
    if (!isfinite(v)) return false;

    *out = v;
    return true;
}

static const char* coingecko_id_for_symbol(const char* sym) {
    if (!sym) return nullptr;

    // KISS mapping for common symbols. Extend later as needed.
    if (strcasecmp(sym, "BTC") == 0) return "bitcoin";
    if (strcasecmp(sym, "ETH") == 0) return "ethereum";
    if (strcasecmp(sym, "SOL") == 0) return "solana";
    if (strcasecmp(sym, "BNB") == 0) return "binancecoin";
    if (strcasecmp(sym, "ADA") == 0) return "cardano";
    if (strcasecmp(sym, "DOGE") == 0) return "dogecoin";
    if (strcasecmp(sym, "XRP") == 0) return "ripple";
    if (strcasecmp(sym, "DOT") == 0) return "polkadot";
    if (strcasecmp(sym, "LTC") == 0) return "litecoin";
    return nullptr;
}

} // namespace

WatchlistScreen::WatchlistScreen(DeviceConfig* deviceConfig, DisplayManager* manager)
    : config(deviceConfig), displayMgr(manager),
      screen(nullptr),
      heroSymbol(nullptr), heroPrice(nullptr),
      slot2Symbol(nullptr), slot2Price(nullptr),
      slot3Symbol(nullptr), slot3Price(nullptr),
      separatorLine(nullptr),
            fetchTaskHandle(nullptr), fetchTaskTcb(nullptr), fetchTaskStack(nullptr), fetchTaskStackDepth(0), stopRequested(false),
      dataMutex(nullptr),
      lastUiUpdateMs(0) {
    memset(slots, 0, sizeof(slots));
        memset(uiCache, 0, sizeof(uiCache));
}

WatchlistScreen::~WatchlistScreen() {
    destroy();
}

void WatchlistScreen::create() {
    if (screen) return;

    screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(screen, lv_color_black(), 0);

    // Hero
    heroSymbol = lv_label_create(screen);
    lv_label_set_text(heroSymbol, "—");
    lv_obj_set_style_text_color(heroSymbol, lv_color_white(), 0);
    lv_obj_set_style_text_font(heroSymbol, &lv_font_montserrat_18, 0);
    lv_obj_align(heroSymbol, LV_ALIGN_CENTER, 0, -115);

    heroPrice = lv_label_create(screen);
    lv_label_set_text(heroPrice, "—");
    lv_obj_set_style_text_color(heroPrice, colorNeutral(), 0);
    lv_obj_set_style_text_font(heroPrice, &lv_font_montserrat_24, 0);
    lv_obj_align(heroPrice, LV_ALIGN_CENTER, 0, -70);

    separatorLine = lv_obj_create(screen);
    lv_obj_set_size(separatorLine, lv_pct(100), 1);
    lv_obj_set_style_bg_color(separatorLine, lv_color_make(60, 60, 60), 0);
    lv_obj_set_style_border_width(separatorLine, 0, 0);
    lv_obj_set_style_pad_all(separatorLine, 0, 0);
    lv_obj_align(separatorLine, LV_ALIGN_CENTER, 0, 10);

    // Slot 2
    slot2Symbol = lv_label_create(screen);
    lv_label_set_text(slot2Symbol, "—");
    lv_obj_set_style_text_color(slot2Symbol, lv_color_white(), 0);
    lv_obj_set_style_text_font(slot2Symbol, &lv_font_montserrat_18, 0);
    lv_obj_align(slot2Symbol, LV_ALIGN_CENTER, 0, 45);

    slot2Price = lv_label_create(screen);
    lv_label_set_text(slot2Price, "—");
    lv_obj_set_style_text_color(slot2Price, colorNeutral(), 0);
    lv_obj_set_style_text_font(slot2Price, &lv_font_montserrat_18, 0);
    lv_obj_align(slot2Price, LV_ALIGN_CENTER, 0, 70);

    // Slot 3
    slot3Symbol = lv_label_create(screen);
    lv_label_set_text(slot3Symbol, "—");
    lv_obj_set_style_text_color(slot3Symbol, lv_color_white(), 0);
    lv_obj_set_style_text_font(slot3Symbol, &lv_font_montserrat_18, 0);
    lv_obj_align(slot3Symbol, LV_ALIGN_CENTER, 0, 115);

    slot3Price = lv_label_create(screen);
    lv_label_set_text(slot3Price, "—");
    lv_obj_set_style_text_color(slot3Price, colorNeutral(), 0);
    lv_obj_set_style_text_font(slot3Price, &lv_font_montserrat_18, 0);
    lv_obj_align(slot3Price, LV_ALIGN_CENTER, 0, 140);

    // Tap anywhere to go back.
    lv_obj_add_event_cb(screen, touchEventCallback, LV_EVENT_CLICKED, this);
    lv_obj_add_flag(screen, LV_OBJ_FLAG_CLICKABLE);

    dataMutex = xSemaphoreCreateMutex();

    stopRequested = false;
    if (!fetchTaskHandle) {
        constexpr uint32_t kFetchTaskStack = 6144;

        // Network calls should not run inside the LVGL task.
        // Prefer placing this stack in PSRAM when available to reduce internal heap pressure.
#if SOC_SPIRAM_SUPPORTED
        if (psramFound() && !fetchTaskStack && !fetchTaskHandle) {
            fetchTaskStackDepth = kFetchTaskStack;

            fetchTaskStack = (StackType_t*)heap_caps_malloc(
                (size_t)fetchTaskStackDepth * sizeof(StackType_t),
                MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT
            );

            // IMPORTANT: FreeRTOS requires the StaticTask_t (TCB) memory to be in internal RAM.
            // The WatchlistScreen object might live in PSRAM depending on how the app allocates objects.
            if (fetchTaskStack && !fetchTaskTcb) {
                fetchTaskTcb = (StaticTask_t*)heap_caps_malloc(sizeof(StaticTask_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
            }

            if (fetchTaskStack && fetchTaskTcb) {
                memset(fetchTaskTcb, 0, sizeof(*fetchTaskTcb));
                memset(fetchTaskStack, 0, (size_t)fetchTaskStackDepth * sizeof(StackType_t));

                // Avoid xTaskCreateStaticPinnedToCore here: on some Arduino/IDF builds it asserts
                // when task metadata isn't in the exact memory regions it expects.
                fetchTaskHandle = xTaskCreateStatic(fetchTask, "WatchlistFetch", fetchTaskStackDepth, this, 1, fetchTaskStack, fetchTaskTcb);

                if (fetchTaskHandle) {
                    Logger.logLine("Watchlist: Fetch task stack in PSRAM");
                }
            }

            if (!fetchTaskHandle) {
                if (fetchTaskStack) {
                    heap_caps_free(fetchTaskStack);
                    fetchTaskStack = nullptr;
                    fetchTaskStackDepth = 0;
                }
                if (fetchTaskTcb) {
                    heap_caps_free(fetchTaskTcb);
                    fetchTaskTcb = nullptr;
                }
            }
        }
#endif

        if (!fetchTaskHandle) {
#if CONFIG_FREERTOS_UNICORE
            xTaskCreate(fetchTask, "WatchlistFetch", kFetchTaskStack, this, 1, &fetchTaskHandle);
#else
            xTaskCreatePinnedToCore(fetchTask, "WatchlistFetch", kFetchTaskStack, this, 1, &fetchTaskHandle, 1);
#endif
        }
    }
}

void WatchlistScreen::destroy() {
    stopRequested = true;
    // Prefer letting the task exit cleanly to avoid tearing down TLS/HTTP mid-flight.
    if (fetchTaskHandle) {
        const uint32_t start = millis();
        while (fetchTaskHandle && (uint32_t)(millis() - start) < 750) {
            vTaskDelay(pdMS_TO_TICKS(25));
        }
        if (fetchTaskHandle) {
            vTaskDelete(fetchTaskHandle);
            fetchTaskHandle = nullptr;
        }
    }

    if (fetchTaskStack) {
        heap_caps_free(fetchTaskStack);
        fetchTaskStack = nullptr;
        fetchTaskStackDepth = 0;
    }

    if (fetchTaskTcb) {
        heap_caps_free(fetchTaskTcb);
        fetchTaskTcb = nullptr;
    }

    if (dataMutex) {
        vSemaphoreDelete(dataMutex);
        dataMutex = nullptr;
    }

    memset(uiCache, 0, sizeof(uiCache));

    if (screen) {
        lv_obj_del(screen);
        screen = nullptr;
        heroSymbol = nullptr;
        heroPrice = nullptr;
        slot2Symbol = nullptr;
        slot2Price = nullptr;
        slot3Symbol = nullptr;
        slot3Price = nullptr;
        separatorLine = nullptr;
    }
}

void WatchlistScreen::show() {
    if (screen) {
        lv_scr_load(screen);
    }
}

void WatchlistScreen::hide() {
    // LVGL handles screen switching.
}

void WatchlistScreen::update() {
    if (!screen) return;

    const uint32_t now = millis();
    if (lastUiUpdateMs != 0 && (uint32_t)(now - lastUiUpdateMs) < kUiUpdateIntervalMs) {
        return;
    }
    lastUiUpdateMs = now;

    if (!dataMutex) return;

    if (xSemaphoreTake(dataMutex, 0) != pdTRUE) {
        return;
    }

    applySlotToUi(0, heroSymbol, heroPrice);
    applySlotToUi(1, slot2Symbol, slot2Price);
    applySlotToUi(2, slot3Symbol, slot3Price);

    xSemaphoreGive(dataMutex);
}

void WatchlistScreen::fetchTask(void* pvParameter) {
    WatchlistScreen* self = (WatchlistScreen*)pvParameter;
    if (!self) {
        vTaskDelete(NULL);
        return;
    }

    uint32_t lastFetchMs = 0;

    while (!self->stopRequested) {
        const uint32_t now = millis();

        uint16_t refreshSeconds = 60;
        if (self->config) {
            refreshSeconds = self->config->watchlist_refresh_seconds;
            if (refreshSeconds == 0) refreshSeconds = 60;
            if (refreshSeconds < 15) refreshSeconds = 15;
            if (refreshSeconds > 3600) refreshSeconds = 3600;
        }

        const uint32_t refreshMs = (uint32_t)refreshSeconds * 1000UL;

        if (lastFetchMs == 0 || (uint32_t)(now - lastFetchMs) >= refreshMs) {
            lastFetchMs = now;

            if (!self->dataMutex) {
                vTaskDelay(pdMS_TO_TICKS(200));
                continue;
            }

            // Parse latest config into slot definitions.
            if (xSemaphoreTake(self->dataMutex, pdMS_TO_TICKS(250)) == pdTRUE) {
                self->parseSlotConfig(0, self->config ? self->config->watchlist_slot1 : "");
                self->parseSlotConfig(1, self->config ? self->config->watchlist_slot2 : "");
                self->parseSlotConfig(2, self->config ? self->config->watchlist_slot3 : "");
                xSemaphoreGive(self->dataMutex);
            }

            if (WiFi.status() == WL_CONNECTED) {
                (void)self->fetchSlot(0);
                (void)self->fetchSlot(1);
                (void)self->fetchSlot(2);
            } else {
                // Mark as error but keep last known price.
                if (xSemaphoreTake(self->dataMutex, pdMS_TO_TICKS(250)) == pdTRUE) {
                    for (int i = 0; i < 3; i++) {
                        if (self->slots[i].enabled) {
                            self->slots[i].hasError = true;
                        }
                    }
                    xSemaphoreGive(self->dataMutex);
                }
            }
        }

        vTaskDelay(pdMS_TO_TICKS(250));
    }

    // Signal that we're done so destroy() can continue without force deleting us.
    self->fetchTaskHandle = nullptr;
    vTaskDelete(NULL);
}

void WatchlistScreen::parseSlotConfig(int slotIndex, const char* raw) {
    if (slotIndex < 0 || slotIndex >= 3) return;

    SlotState& s = slots[slotIndex];

    char buf[CONFIG_WATCHLIST_SLOT_MAX_LEN];
    strlcpy(buf, raw ? raw : "", sizeof(buf));
    trimInPlace(buf);

    s.enabled = (strlen(buf) > 0);
    s.hasError = false;

    if (!s.enabled) {
        s.type = ItemType::None;
        s.displaySymbol[0] = '\0';
        s.resolvedSymbol[0] = '\0';
        return;
    }

    ItemType type = ItemType::Stock;
    const char* sym = buf;

    if (startsWithIgnoreCase(buf, "crypto:")) {
        type = ItemType::Crypto;
        sym = buf + strlen("crypto:");
    } else if (startsWithIgnoreCase(buf, "stock:")) {
        type = ItemType::Stock;
        sym = buf + strlen("stock:");
    }

    while (*sym == ' ' || *sym == '\t') sym++;

    char symbol[CONFIG_WATCHLIST_SLOT_MAX_LEN];
    strlcpy(symbol, sym, sizeof(symbol));
    trimInPlace(symbol);

    // Normalize to upper for display.
    for (size_t i = 0; symbol[i]; i++) {
        symbol[i] = (char)toupper((unsigned char)symbol[i]);
    }

    s.type = type;

    // Display symbol (no prefix)
    strlcpy(s.displaySymbol, symbol, sizeof(s.displaySymbol));

    if (type == ItemType::Stock) {
        // Stooq convention: accept full symbols like MSFT.US, otherwise default to .US
        if (strchr(symbol, '.') != nullptr) {
            strlcpy(s.resolvedSymbol, symbol, sizeof(s.resolvedSymbol));
        } else {
            snprintf(s.resolvedSymbol, sizeof(s.resolvedSymbol), "%s.US", symbol);
        }
    } else {
        // For CoinGecko we keep the symbol itself; resolution happens via mapping.
        strlcpy(s.resolvedSymbol, symbol, sizeof(s.resolvedSymbol));
    }
}

bool WatchlistScreen::fetchSlot(int slotIndex) {
    if (slotIndex < 0 || slotIndex >= 3) return false;

    if (!dataMutex) return false;

    SlotState snapshot;
    memset(&snapshot, 0, sizeof(snapshot));

    if (xSemaphoreTake(dataMutex, pdMS_TO_TICKS(250)) != pdTRUE) {
        return false;
    }
    snapshot = slots[slotIndex];
    xSemaphoreGive(dataMutex);

    if (!snapshot.enabled || snapshot.type == ItemType::None) {
        return false;
    }

    float price = NAN;
    bool ok = false;

    if (snapshot.type == ItemType::Stock) {
        ok = fetchStockStooq(snapshot.resolvedSymbol, &price);
    } else if (snapshot.type == ItemType::Crypto) {
        ok = fetchCryptoCoinGecko(snapshot.resolvedSymbol, &price);
    }

    if (!isfinite(price)) ok = false;

    if (xSemaphoreTake(dataMutex, pdMS_TO_TICKS(250)) != pdTRUE) {
        return false;
    }

    SlotState& s = slots[slotIndex];

    if (ok) {
        s.hasError = false;
        if (s.hasPrice) {
            s.dir = computeTickDir(s.lastPrice, price);
        } else {
            s.dir = TickDir::Unknown;
        }
        s.lastPrice = price;
        s.hasPrice = true;
    } else {
        s.hasError = true;
        // Keep lastPrice/dir, but UI will show neutral color.
    }

    xSemaphoreGive(dataMutex);
    return ok;
}

bool WatchlistScreen::fetchStockStooq(const char* stooqSymbol, float* outPrice) {
    if (!stooqSymbol || !outPrice) return false;

    char url[192];
    // Note: Stooq redirects HTTP -> HTTPS, so we go direct to HTTPS.
    snprintf(url, sizeof(url), "https://stooq.com/q/l/?s=%s&f=sd2t2c&h&e=csv", stooqSymbol);

    WiFiClientSecure client;
    client.setInsecure();
    client.setTimeout(kHttpTimeoutMs);

    HTTPClient http;
    http.setTimeout(kHttpTimeoutMs);

    if (!http.begin(client, url)) {
        return false;
    }

    const int code = http.GET();
    if (code != 200) {
        http.end();
        return false;
    }

    String payload = http.getString();
    http.end();

    if (payload.length() == 0 || payload.length() > kMaxCsvPayload) {
        return false;
    }

    // Expect header + one row.
    const int nl = payload.indexOf('\n');
    if (nl < 0) return false;

    const String row = payload.substring(nl + 1);

    char line[128];
    strlcpy(line, row.c_str(), sizeof(line));

    // Close is field index 3 for f=sd2t2c
    char closeStr[32];
    if (!csv_get_field(line, 3, closeStr, sizeof(closeStr))) {
        return false;
    }

    const float v = parse_float_best_effort(closeStr);
    if (!isfinite(v)) return false;

    *outPrice = v;
    return true;
}

bool WatchlistScreen::fetchCryptoCoinGecko(const char* symbol, float* outPrice) {
    if (!symbol || !outPrice) return false;

    const char* id = coingecko_id_for_symbol(symbol);
    if (!id) {
        return false;
    }

    char url[192];
    snprintf(url, sizeof(url), "https://api.coingecko.com/api/v3/simple/price?ids=%s&vs_currencies=usd", id);

    WiFiClientSecure client;
    client.setInsecure();
    client.setTimeout(kHttpTimeoutMs);

    HTTPClient http;
    http.setTimeout(kHttpTimeoutMs);

    if (!http.begin(client, url)) {
        return false;
    }

    http.addHeader("Accept", "application/json");

    const int code = http.GET();
    if (code != 200) {
        http.end();
        return false;
    }

    String payload = http.getString();
    http.end();

    if (payload.length() == 0 || payload.length() > kMaxJsonPayload) {
        return false;
    }

    float v = NAN;
    if (!json_extract_usd(payload.c_str(), &v)) {
        return false;
    }

    *outPrice = v;
    return true;
}

bool WatchlistScreen::startsWithIgnoreCase(const char* s, const char* prefix) {
    if (!s || !prefix) return false;
    const size_t n = strlen(prefix);
    if (strlen(s) < n) return false;
    return strncasecmp(s, prefix, n) == 0;
}

void WatchlistScreen::trimInPlace(char* s) {
    if (!s) return;

    // Left trim
    char* p = s;
    while (*p && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) p++;
    if (p != s) {
        memmove(s, p, strlen(p) + 1);
    }

    // Right trim
    const size_t len = strlen(s);
    if (len == 0) return;
    size_t end = len;
    while (end > 0) {
        const char c = s[end - 1];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            end--;
            continue;
        }
        break;
    }
    s[end] = '\0';
}

WatchlistScreen::TickDir WatchlistScreen::computeTickDir(float prev, float next) {
    const float d = next - prev;
    if (!isfinite(d)) return TickDir::Unknown;
    if (fabsf(d) < kTickEpsilon) return TickDir::Unknown;
    return (d > 0) ? TickDir::Up : TickDir::Down;
}

lv_color_t WatchlistScreen::colorNeutral() {
    return lv_color_make(200, 200, 200);
}

lv_color_t WatchlistScreen::colorUp() {
    return lv_color_make(0, 200, 80);
}

lv_color_t WatchlistScreen::colorDown() {
    return lv_color_make(220, 60, 60);
}

void WatchlistScreen::applySlotToUi(int slotIndex, lv_obj_t* symLabel, lv_obj_t* priceLabel) {
    if (slotIndex < 0 || slotIndex >= 3) return;

    const SlotState& s = slots[slotIndex];

    UiSlotCache& cache = uiCache[slotIndex];
    if (!cache.initialized) {
        cache.initialized = true;
        cache.symbol[0] = '\0';
        cache.price[0] = '\0';
        cache.priceColor32 = 0;
    }

    const char* desiredSymbol = "—";
    const char* desiredPrice = "—";
    lv_color_t desiredColor = colorNeutral();

    if (!s.enabled || s.type == ItemType::None) {
        // Keep defaults.
    } else {
        desiredSymbol = s.displaySymbol[0] ? s.displaySymbol : "—";

        if (s.hasPrice) {
            static char priceText[24];
            // KISS formatting: keep 2 decimals for now.
            snprintf(priceText, sizeof(priceText), "%.2f", s.lastPrice);
            desiredPrice = priceText;

            if (s.hasError) {
                desiredColor = colorNeutral();
            } else if (s.dir == TickDir::Up) {
                desiredColor = colorUp();
            } else if (s.dir == TickDir::Down) {
                desiredColor = colorDown();
            } else {
                desiredColor = colorNeutral();
            }
        } else {
            desiredPrice = "—";
            desiredColor = colorNeutral();
        }
    }

    if (symLabel) {
        if (strncmp(cache.symbol, desiredSymbol, sizeof(cache.symbol)) != 0) {
            lv_label_set_text(symLabel, desiredSymbol);
            strlcpy(cache.symbol, desiredSymbol, sizeof(cache.symbol));
        }
    }

    if (!priceLabel) return;

    if (strncmp(cache.price, desiredPrice, sizeof(cache.price)) != 0) {
        lv_label_set_text(priceLabel, desiredPrice);
        strlcpy(cache.price, desiredPrice, sizeof(cache.price));
    }

    const uint32_t color32 = lv_color_to32(desiredColor);
    if (cache.priceColor32 != color32) {
        lv_obj_set_style_text_color(priceLabel, desiredColor, 0);
        cache.priceColor32 = color32;
    }

    return;
}

void WatchlistScreen::touchEventCallback(lv_event_t* e) {
    WatchlistScreen* instance = (WatchlistScreen*)lv_event_get_user_data(e);
    if (instance && instance->displayMgr) {
        (void)instance->displayMgr->goBackOrDefault();
    }
}

#endif // HAS_DISPLAY
