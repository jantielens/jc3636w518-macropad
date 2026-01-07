#ifndef MACROPAD_SCREEN_H
#define MACROPAD_SCREEN_H

#include "screen.h"
#include <lvgl.h>

#include "../macros_config.h"

#include <stdint.h>

class BleKeyboardManager;
class DisplayManager;

namespace macropad_layout {
struct MacroPadLayoutContext;
}

class MacroPadScreen : public Screen {
public:
    MacroPadScreen(DisplayManager* manager = nullptr, uint8_t screenIndex = 0);
    ~MacroPadScreen();

    void configure(DisplayManager* manager, uint8_t screenIndex);

    void create() override;
    void destroy() override;
    void show() override;
    void hide() override;
    void update() override;

private:
    struct ButtonCtx {
        MacroPadScreen* self;
        uint8_t buttonIndex;
    };

    struct PressAnimStyles {
        bool inited;
        lv_style_transition_dsc_t btnTrans;

        lv_style_t btnBase[MACROS_BUTTONS_PER_SCREEN];
        lv_style_t btnPressed[MACROS_BUTTONS_PER_SCREEN];

        lv_style_t segBase[8];
        lv_style_t segPressed[8];

        PressAnimStyles() : inited(false) {}
    };

    static constexpr uint32_t kMinPressCueMs = 100;

    DisplayManager* displayMgr;
    uint8_t screenIndex;

    lv_obj_t* screen;
    lv_obj_t* buttons[MACROS_BUTTONS_PER_SCREEN];
    lv_obj_t* labels[MACROS_BUTTONS_PER_SCREEN];
    lv_obj_t* icons[MACROS_BUTTONS_PER_SCREEN];

    // Pie template helpers (round_pie_8): we use a full-screen hit layer for
    // polar hit-testing and draw ring segments separately.
    lv_obj_t* pieHitLayer;
    lv_obj_t* pieSegments[8];
    lv_obj_t* emptyStateLabel;
    ButtonCtx buttonCtx[MACROS_BUTTONS_PER_SCREEN];

    PressAnimStyles pressStyles;
    int8_t pressedPieSlot;

    lv_timer_t* pressHoldTimer;
    uint32_t pressDownTick[MACROS_BUTTONS_PER_SCREEN];
    uint32_t pendingClearTick[MACROS_BUTTONS_PER_SCREEN];

    uint32_t lastUpdateMs;

    char lastTemplateId[MACROS_TEMPLATE_ID_MAX_LEN];

    void layoutButtons();
    void refreshButtons(bool force);

    void updateButtonLayout(uint8_t index, bool hasIcon, bool hasLabel);

    void updateEmptyState(bool anyButtonConfigured);

    const MacroConfig* getMacroConfig() const;
    BleKeyboardManager* getBleKeyboard() const;

    const char* resolveTemplateId(const MacroConfig* cfg) const;
    void buildLayoutContext(macropad_layout::MacroPadLayoutContext& out);

    void ensurePressStylesInited();

    void notePressed(uint8_t slotIndex);
    void scheduleReleaseClear(uint8_t slotIndex);
    void cancelPendingClear(uint8_t slotIndex);
    void clearPressedVisual(uint8_t slotIndex);
    static void pressHoldTimerCallback(lv_timer_t* t);

    void handleButtonClick(uint8_t buttonIndex);
    static void pieEventCallback(lv_event_t* e);

    static void buttonEventCallback(lv_event_t* e);
};

#endif // MACROPAD_SCREEN_H
