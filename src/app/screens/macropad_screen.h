#ifndef MACROPAD_SCREEN_H
#define MACROPAD_SCREEN_H

#include "screen.h"
#include <lvgl.h>

#include "../macros_config.h"

#include <stdint.h>

class BleKeyboardManager;
class DisplayManager;

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

    uint32_t lastUpdateMs;

    char lastTemplateId[MACROS_TEMPLATE_ID_MAX_LEN];

    void layoutButtons();
    void layoutButtonsRound9();
    void layoutButtonsPie8();
    void layoutButtonsFiveStack();
    void layoutButtonsWideCenter();
    void layoutButtonsFourSplit();
    bool isSlotUsedByTemplate(uint8_t slot) const;
    void refreshButtons(bool force);

    void updateButtonLayout(uint8_t index, bool hasIcon, bool hasLabel);

    void updateEmptyState(bool anyButtonConfigured);

    const MacroConfig* getMacroConfig() const;
    BleKeyboardManager* getBleKeyboard() const;

    void handleButtonClick(uint8_t buttonIndex);
    int pieSlotFromPoint(int x, int y) const;

    static void pieEventCallback(lv_event_t* e);

    static void buttonEventCallback(lv_event_t* e);
};

#endif // MACROPAD_SCREEN_H
