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
    lv_obj_t* emptyStateLabel;
    ButtonCtx buttonCtx[MACROS_BUTTONS_PER_SCREEN];

    uint32_t lastUpdateMs;

    char lastTemplateId[16];

    void layoutButtons();
    void layoutButtonsRound9();
    void layoutButtonsFiveStack();
    void layoutButtonsWideCenter();
    void layoutButtonsFourSplit();
    bool isSlotUsedByTemplate(uint8_t slot) const;
    void refreshButtons(bool force);

    void updateEmptyState(bool anyButtonConfigured);

    const MacroConfig* getMacroConfig() const;
    BleKeyboardManager* getBleKeyboard() const;

    static void buttonEventCallback(lv_event_t* e);
};

#endif // MACROPAD_SCREEN_H
