#ifndef ANALOG_CLOCK_SCREEN_H
#define ANALOG_CLOCK_SCREEN_H

#include "screen.h"

#include <lvgl.h>

class DisplayManager;

class AnalogClockScreen : public Screen {
public:
    explicit AnalogClockScreen(DisplayManager* manager);
    ~AnalogClockScreen() override;

    void create() override;
    void destroy() override;
    void show() override;
    void hide() override;
    void update() override;

private:
    DisplayManager* displayMgr;

    lv_obj_t* screen;
    lv_obj_t* dial;

    uint32_t lastTickMs;
    uint32_t lastDriftMs;
    int16_t driftX;
    int16_t driftY;

    bool timeValid;

    // For smooth hand motion using a stable time/millis reference.
    time_t baseEpoch;
    uint32_t baseMillis;

    void updateAntiBurnInDrift();
    void updateTimeBaseIfNeeded();
    bool computeHmsFraction(float* outHour, float* outMinute, float* outSecond);

    static void onDialDraw(lv_event_t* e);
    static void onScreenClicked(lv_event_t* e);

    static bool isTimeValid();
};

#endif // ANALOG_CLOCK_SCREEN_H
