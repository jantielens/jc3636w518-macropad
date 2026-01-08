#ifndef ERROR_SCREEN_H
#define ERROR_SCREEN_H

#include "screen.h"
#include <lvgl.h>

class DisplayManager;

class ErrorScreen : public Screen {
private:
    lv_obj_t* screen = nullptr;
    DisplayManager* displayMgr = nullptr;

    lv_obj_t* titleLabel = nullptr;
    lv_obj_t* messageLabel = nullptr;
    lv_obj_t* hintLabel = nullptr;
    lv_obj_t* iconImg = nullptr;

    static void touchEventCallback(lv_event_t* e);

public:
    explicit ErrorScreen(DisplayManager* manager);
    ~ErrorScreen();

    void setError(const char* title, const char* message);

    void create() override;
    void destroy() override;
    void show() override;
    void hide() override;
    void update() override;
};

#endif // ERROR_SCREEN_H
