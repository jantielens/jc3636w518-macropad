#ifndef WATCHLIST_SCREEN_H
#define WATCHLIST_SCREEN_H

#include "screen.h"
#include "../config_manager.h"

#if HAS_DISPLAY

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

class DisplayManager;

class WatchlistScreen : public Screen {
public:
    WatchlistScreen(DeviceConfig* deviceConfig, DisplayManager* manager);
    ~WatchlistScreen() override;

    void create() override;
    void destroy() override;
    void show() override;
    void hide() override;
    void update() override;

private:
    enum class ItemType : uint8_t {
        Stock,
        Crypto,
        None,
    };

    enum class TickDir : int8_t {
        Unknown = 0,
        Up = 1,
        Down = -1,
    };

    struct SlotState {
        bool enabled;
        ItemType type;
        char displaySymbol[16];
        char resolvedSymbol[24];
        float lastPrice;
        bool hasPrice;
        TickDir dir;
        bool hasError;
    };

    struct UiSlotCache {
        bool initialized;
        char symbol[16];
        char price[24];
        uint32_t priceColor32;
    };

    DeviceConfig* config;
    DisplayManager* displayMgr;

    lv_obj_t* screen;

    // Slot labels
    lv_obj_t* heroSymbol;
    lv_obj_t* heroPrice;

    lv_obj_t* slot2Symbol;
    lv_obj_t* slot2Price;

    lv_obj_t* slot3Symbol;
    lv_obj_t* slot3Price;

    lv_obj_t* separatorLine;

    // Fetch loop task
    TaskHandle_t fetchTaskHandle;
    StaticTask_t* fetchTaskTcb;
    StackType_t* fetchTaskStack;
    uint32_t fetchTaskStackDepth;
    volatile bool stopRequested;
    SemaphoreHandle_t dataMutex;

    SlotState slots[3];
    UiSlotCache uiCache[3];
    uint32_t lastUiUpdateMs;

    static void fetchTask(void* pvParameter);

    void parseSlotConfig(int slotIndex, const char* raw);
    bool fetchSlot(int slotIndex);

    bool fetchStockStooq(const char* stooqSymbol, float* outPrice);
    bool fetchCryptoCoinGecko(const char* symbol, float* outPrice);

    static bool startsWithIgnoreCase(const char* s, const char* prefix);
    static void trimInPlace(char* s);

    static TickDir computeTickDir(float prev, float next);

    static lv_color_t colorNeutral();
    static lv_color_t colorUp();
    static lv_color_t colorDown();

    void applySlotToUi(int slotIndex, lv_obj_t* symLabel, lv_obj_t* priceLabel);

    static void touchEventCallback(lv_event_t* e);
};

#endif // HAS_DISPLAY

#endif // WATCHLIST_SCREEN_H
