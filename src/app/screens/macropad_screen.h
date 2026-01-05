#ifndef MACROPAD_SCREEN_H
#define MACROPAD_SCREEN_H

#include "screen.h"
#include <lvgl.h>

#include <stdint.h>

struct MacroConfig;
struct MacroButtonConfig;
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
    lv_obj_t* buttons[9];
    lv_obj_t* labels[9];
    ButtonCtx buttonCtx[9];

    uint32_t lastUpdateMs;

    void layoutButtons();
    void refreshButtons(bool force);

    const MacroConfig* getMacroConfig() const;
    BleKeyboardManager* getBleKeyboard() const;

    static void buttonEventCallback(lv_event_t* e);
};

#endif // MACROPAD_SCREEN_H
