#pragma once

#include "screen.h"

#if HAS_DISPLAY

#include <lvgl.h>

class SpotifyScreen : public Screen {
public:
    SpotifyScreen();
    ~SpotifyScreen() override;

    void create() override;
    void destroy() override;
    void show() override;
    void hide() override;
    void update() override;

private:
    lv_obj_t* scr = nullptr;
    lv_obj_t* img = nullptr;
    lv_obj_t* title = nullptr;
    lv_obj_t* subtitle = nullptr;
    lv_obj_t* btn_prev = nullptr;
    lv_obj_t* btn_next = nullptr;

    lv_img_dsc_t img_dsc{};
    uint16_t* pixel_buf = nullptr;
    size_t pixel_buf_bytes = 0;

    unsigned long last_ui_update_ms = 0;

    static void prev_cb(lv_event_t* e);
    static void next_cb(lv_event_t* e);

    void freePixelBuf();
    void maybeAdoptNewImage();
};

#endif
