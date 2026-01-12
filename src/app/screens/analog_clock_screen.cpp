#include "analog_clock_screen.h"

#include "../display_manager.h"

#include <Arduino.h>
#include <math.h>
#include <time.h>

namespace {

constexpr uint32_t kAnalogUiTickMs = 33;        // ~30 FPS for smooth second hand
constexpr uint32_t kAnalogDriftTickMs = 15000;  // 15s
constexpr int16_t kAnalogMaxDriftPx = 3;

static lv_color_t c_analog_bg() { return lv_color_black(); }
static lv_color_t c_analog_dial() { return lv_color_white(); }
static lv_color_t c_analog_tick() { return lv_color_black(); }
static lv_color_t c_analog_hand() { return lv_color_black(); }
static lv_color_t c_analog_second() { return lv_color_make(200, 20, 20); }
static lv_color_t c_analog_dim() { return lv_color_make(120, 120, 120); }

static inline float deg2rad(float deg) {
    return deg * (float)M_PI / 180.0f;
}

static inline lv_point_t polarPoint(lv_coord_t cx, lv_coord_t cy, float angleDeg, float radius) {
    const float a = deg2rad(angleDeg);
    lv_point_t p;
    p.x = (lv_coord_t)lroundf((float)cx + cosf(a) * radius);
    p.y = (lv_coord_t)lroundf((float)cy + sinf(a) * radius);
    return p;
}

static void drawFilledCircle(lv_draw_ctx_t* ctx, lv_coord_t cx, lv_coord_t cy, lv_coord_t r, lv_color_t color) {
    lv_draw_rect_dsc_t d;
    lv_draw_rect_dsc_init(&d);
    d.bg_color = color;
    d.bg_opa = LV_OPA_COVER;
    d.border_opa = LV_OPA_TRANSP;
    d.radius = LV_RADIUS_CIRCLE;

    lv_area_t a;
    a.x1 = cx - r;
    a.y1 = cy - r;
    a.x2 = cx + r;
    a.y2 = cy + r;

    ctx->draw_rect(ctx, &d, &a);
}

static void drawLine(lv_draw_ctx_t* ctx, lv_coord_t x1, lv_coord_t y1, lv_coord_t x2, lv_coord_t y2, lv_color_t color, uint8_t width) {
    lv_draw_line_dsc_t d;
    lv_draw_line_dsc_init(&d);
    d.color = color;
    d.width = width;
    d.opa = LV_OPA_COVER;
    d.round_start = 0;
    d.round_end = 0;

    const lv_point_t p1 = {x1, y1};
    const lv_point_t p2 = {x2, y2};
    ctx->draw_line(ctx, &d, &p1, &p2);
}

} // namespace

AnalogClockScreen::AnalogClockScreen(DisplayManager* manager)
    : displayMgr(manager),
      screen(nullptr),
      dial(nullptr),
      lastTickMs(0),
      lastDriftMs(0),
      driftX(0),
      driftY(0),
      timeValid(false),
      baseEpoch(0),
      baseMillis(0) {}

AnalogClockScreen::~AnalogClockScreen() {
    destroy();
}

void AnalogClockScreen::create() {
    if (screen) return;

    screen = lv_obj_create(nullptr);
    lv_obj_set_style_bg_color(screen, c_analog_bg(), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(screen, LV_SCROLLBAR_MODE_OFF);

    lv_obj_add_flag(screen, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(screen, AnalogClockScreen::onScreenClicked, LV_EVENT_CLICKED, this);

    // Single full-screen object with a custom draw handler.
    dial = lv_obj_create(screen);
    lv_obj_set_size(dial, LV_PCT(100), LV_PCT(100));
    lv_obj_center(dial);
    lv_obj_set_style_bg_opa(dial, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(dial, 0, 0);
    lv_obj_set_style_pad_all(dial, 0, 0);
    lv_obj_clear_flag(dial, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(dial, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(dial, AnalogClockScreen::onDialDraw, LV_EVENT_DRAW_MAIN, this);
}

void AnalogClockScreen::destroy() {
    if (screen) {
        lv_obj_del(screen);
        screen = nullptr;
        dial = nullptr;
    }
}

void AnalogClockScreen::show() {
    if (!screen) create();
    if (screen) {
        lv_scr_load(screen);
        lastTickMs = 0;
        lastDriftMs = 0;
        driftX = 0;
        driftY = 0;
        timeValid = false;
        baseEpoch = 0;
        baseMillis = 0;

        if (dial) lv_obj_invalidate(dial);
    }
}

void AnalogClockScreen::hide() {
    // no-op
}

bool AnalogClockScreen::isTimeValid() {
    time_t now = time(nullptr);
    // Consider time valid if it's after 2020-01-01.
    return now > 1577836800;
}

void AnalogClockScreen::updateAntiBurnInDrift() {
    if (!dial) return;

    const uint32_t nowMs = millis();
    if (lastDriftMs != 0 && (uint32_t)(nowMs - lastDriftMs) < kAnalogDriftTickMs) return;
    lastDriftMs = nowMs;

    if (!timeValid) {
        if (driftX != 0 || driftY != 0) {
            driftX = 0;
            driftY = 0;
            lv_obj_invalidate(dial);
        }
        return;
    }

    // Deterministic drift pattern based on wall-clock time.
    time_t now = time(nullptr);
    struct tm t;
    localtime_r(&now, &t);

    const int step = (t.tm_min * 60 + t.tm_sec) / (kAnalogDriftTickMs / 1000);
    const int phase = step % 16; // 4x4 grid

    const int gridX = (phase % 4) - 1; // -1..2
    const int gridY = (phase / 4) - 1; // -1..2

    int16_t newX = (int16_t)(gridX * 2);
    int16_t newY = (int16_t)(gridY * 2);

    if (newX > kAnalogMaxDriftPx) newX = kAnalogMaxDriftPx;
    if (newX < -kAnalogMaxDriftPx) newX = -kAnalogMaxDriftPx;
    if (newY > kAnalogMaxDriftPx) newY = kAnalogMaxDriftPx;
    if (newY < -kAnalogMaxDriftPx) newY = -kAnalogMaxDriftPx;

    if (newX == driftX && newY == driftY) return;
    driftX = newX;
    driftY = newY;
    lv_obj_invalidate(dial);
}

void AnalogClockScreen::updateTimeBaseIfNeeded() {
    if (!timeValid) return;

    const uint32_t nowMs = millis();
    const time_t nowEpoch = time(nullptr);

    // Initialize.
    if (baseEpoch == 0) {
        baseEpoch = nowEpoch;
        baseMillis = nowMs;
        return;
    }

    // If epoch jumps (NTP correction), rebase.
    const time_t predicted = baseEpoch + (time_t)((nowMs - baseMillis) / 1000);
    const time_t delta = nowEpoch - predicted;
    if (delta > 1 || delta < -1) {
        baseEpoch = nowEpoch;
        baseMillis = nowMs;
        return;
    }

    // Periodically rebase exactly on second boundaries to avoid long-term drift.
    if (nowEpoch != predicted) {
        baseEpoch = nowEpoch;
        baseMillis = nowMs;
    }
}

bool AnalogClockScreen::computeHmsFraction(float* outHour, float* outMinute, float* outSecond) {
    if (outHour) *outHour = 0;
    if (outMinute) *outMinute = 0;
    if (outSecond) *outSecond = 0;

    if (!timeValid) return false;

    updateTimeBaseIfNeeded();

    const uint32_t nowMs = millis();
    const uint32_t elapsedMs = (baseMillis == 0) ? 0U : (uint32_t)(nowMs - baseMillis);
    const time_t secs = (time_t)(baseEpoch + (time_t)(elapsedMs / 1000U));
    const float frac = (float)(elapsedMs % 1000U) / 1000.0f;

    struct tm t;
    localtime_r(&secs, &t);

    const float sec = (float)t.tm_sec + frac;
    const float min = (float)t.tm_min + (sec / 60.0f);
    const float hour = (float)(t.tm_hour % 12) + (min / 60.0f);

    if (outSecond) *outSecond = sec;
    if (outMinute) *outMinute = min;
    if (outHour) *outHour = hour;

    return true;
}

void AnalogClockScreen::update() {
    if (!screen || !dial) return;

    const uint32_t now = millis();
    if (lastTickMs != 0 && (uint32_t)(now - lastTickMs) < kAnalogUiTickMs) {
        return;
    }
    lastTickMs = now;

    const bool ok = isTimeValid();
    if (ok != timeValid) {
        timeValid = ok;
        // Reset time base when time becomes valid.
        baseEpoch = 0;
        baseMillis = 0;
    }

    updateAntiBurnInDrift();

    // Force redraw for smooth second-hand motion.
    lv_obj_invalidate(dial);
}

void AnalogClockScreen::onDialDraw(lv_event_t* e) {
    AnalogClockScreen* self = (AnalogClockScreen*)lv_event_get_user_data(e);
    if (!self) return;

    lv_obj_t* obj = lv_event_get_target(e);
    lv_draw_ctx_t* ctx = lv_event_get_draw_ctx(e);
    if (!obj || !ctx) return;

    const lv_coord_t w = lv_obj_get_width(obj);
    const lv_coord_t h = lv_obj_get_height(obj);

    const lv_coord_t cx = (lv_coord_t)(w / 2 + self->driftX);
    const lv_coord_t cy = (lv_coord_t)(h / 2 + self->driftY);

    // Keep a small margin so drift doesn't reveal the dial edge too aggressively.
    const lv_coord_t r = (lv_coord_t)((w < h ? w : h) / 2 - (kAnalogMaxDriftPx + 3));

    // Dial face (white circle).
    drawFilledCircle(ctx, cx, cy, r, c_analog_dial());

    // Ticks.
    for (int i = 0; i < 60; i++) {
        const bool isHour = (i % 5) == 0;
        const float angle = (float)i * 6.0f - 90.0f;

        const float outer = (float)r - 6.0f;
        const float inner = outer - (isHour ? 22.0f : 10.0f);

        const lv_point_t p1 = polarPoint(cx, cy, angle, outer);
        const lv_point_t p2 = polarPoint(cx, cy, angle, inner);

        drawLine(ctx, p1.x, p1.y, p2.x, p2.y, c_analog_tick(), (uint8_t)(isHour ? 10 : 4));
    }

    // Hands.
    float hour = 0, minute = 0, second = 0;
    const bool ok = self->computeHmsFraction(&hour, &minute, &second);

    const lv_color_t handColor = ok ? c_analog_hand() : c_analog_dim();
    const lv_color_t secondColor = ok ? c_analog_second() : c_analog_dim();

    const float secAngle = second * 6.0f - 90.0f;
    const float minAngle = minute * 6.0f - 90.0f;
    const float hourAngle = hour * 30.0f - 90.0f;

    // Hour hand.
    {
        const lv_point_t p = polarPoint(cx, cy, hourAngle, (float)r * 0.55f);
        drawLine(ctx, cx, cy, p.x, p.y, handColor, 14);
    }

    // Minute hand.
    {
        const lv_point_t p = polarPoint(cx, cy, minAngle, (float)r * 0.82f);
        drawLine(ctx, cx, cy, p.x, p.y, handColor, 10);
    }

    // Second hand (smooth) + lollipop.
    {
        const lv_point_t tip = polarPoint(cx, cy, secAngle, (float)r * 0.92f);
        const lv_point_t tail = polarPoint(cx, cy, secAngle + 180.0f, (float)r * 0.18f);
        drawLine(ctx, tail.x, tail.y, tip.x, tip.y, secondColor, 4);

        // Lollipop circle near the tip.
        const lv_point_t lolli = polarPoint(cx, cy, secAngle, (float)r * 0.78f);
        drawFilledCircle(ctx, lolli.x, lolli.y, (lv_coord_t)11, secondColor);
        drawFilledCircle(ctx, lolli.x, lolli.y, (lv_coord_t)6, c_analog_dial());
    }

    // Hub.
    drawFilledCircle(ctx, cx, cy, 10, c_analog_hand());
}

void AnalogClockScreen::onScreenClicked(lv_event_t* e) {
    AnalogClockScreen* self = (AnalogClockScreen*)lv_event_get_user_data(e);
    if (!self || !self->displayMgr) return;
    self->displayMgr->goBackOrDefault();
}
