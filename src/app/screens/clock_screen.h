#ifndef CLOCK_SCREEN_H
#define CLOCK_SCREEN_H

#include "screen.h"

#include <lvgl.h>

class DisplayManager;

class ClockScreen : public Screen {
public:
    explicit ClockScreen(DisplayManager* manager);
    ~ClockScreen() override;

    void create() override;
    void destroy() override;
    void show() override;
    void hide() override;
    void update() override;

private:
    struct DigitWidget {
        ClockScreen* owner = nullptr;
        lv_obj_t* root = nullptr;
        lv_obj_t* topHalf = nullptr;
        lv_obj_t* bottomHalf = nullptr;
        lv_obj_t* hinge = nullptr;
        lv_obj_t* topLabel = nullptr;
        lv_obj_t* bottomLabel = nullptr;

        lv_obj_t* topFlap = nullptr;
        lv_obj_t* bottomFlap = nullptr;
        lv_obj_t* topFlapLabel = nullptr;
        lv_obj_t* bottomFlapLabel = nullptr;

        int16_t w = 0;
        int16_t h = 0;
        int16_t halfH = 0;

        char current = '-';
        char pending = '-';
        char flipFrom = '-';
        char flipTo = '-';
        bool animating = false;
    };

    DisplayManager* displayMgr;

    lv_obj_t* screen;
    lv_obj_t* container;

    DigitWidget digits[6];

    uint32_t lastTickMs;

    uint32_t lastDriftMs;
    int16_t driftX;
    int16_t driftY;

    bool timeValid;

    void layout();
    void positionDigitLabels(DigitWidget& d);
    void initDigit(DigitWidget& d, lv_obj_t* parent, int16_t w, int16_t h);
    void setDigitImmediate(DigitWidget& d, char c);
    void requestDigit(DigitWidget& d, char c);
    void startFlip(DigitWidget& d, char from, char to);

    void updateAntiBurnInDrift();

    static void onScreenClicked(lv_event_t* e);

    static bool isTimeValid();
    static void getHms(char out[7], bool* ok);

    static void animSetTopFlapHeight(void* var, int32_t v);
    static void animSetBottomFlapHeight(void* var, int32_t v);
    static void animTopFlapReady(lv_anim_t* a);
    static void animBottomFlapReady(lv_anim_t* a);
};

#endif // CLOCK_SCREEN_H
