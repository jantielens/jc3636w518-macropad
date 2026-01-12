#include "clock_screen.h"

#include "../display_manager.h"

#include <Arduino.h>
#include <math.h>
#include <time.h>

namespace {

constexpr uint32_t kUiTickMs = 50;

constexpr uint32_t kDriftTickMs = 15000; // 15s
constexpr int16_t kMaxDriftPx = 3;

constexpr uint32_t kFlipPhaseMs = 160;
constexpr uint32_t kFlipPauseMs = 30;

static lv_color_t c_bg() { return lv_color_black(); }
static lv_color_t c_card() { return lv_color_make(36, 36, 36); }
static lv_color_t c_card2() { return lv_color_make(28, 28, 28); }
static lv_color_t c_hinge() { return lv_color_black(); }
static lv_color_t c_text() { return lv_color_make(235, 235, 240); }
static lv_color_t c_dimText() { return lv_color_make(140, 140, 150); }
static lv_color_t c_flap() { return lv_color_make(20, 20, 20); }
static lv_color_t c_rivet() { return lv_color_make(14, 14, 16); }

static void setLabelChar(lv_obj_t* label, char c) {
    if (!label) return;
    char s[2] = {c, '\0'};
    lv_label_set_text(label, s);
}

static void setLabelText(lv_obj_t* label, const char* s) {
    if (!label) return;
    lv_label_set_text(label, s ? s : "");
}

} // namespace

ClockScreen::ClockScreen(DisplayManager* manager)
    : displayMgr(manager),
      screen(nullptr),
      container(nullptr),
      lastTickMs(0),
    lastDriftMs(0),
    driftX(0),
    driftY(0),
      timeValid(false) {}

ClockScreen::~ClockScreen() {
    destroy();
}

void ClockScreen::create() {
    if (screen) return;

    screen = lv_obj_create(nullptr);
    lv_obj_set_style_bg_color(screen, c_bg(), 0);
    lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(screen, LV_SCROLLBAR_MODE_OFF);

    lv_obj_add_flag(screen, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(screen, ClockScreen::onScreenClicked, LV_EVENT_CLICKED, this);

    container = lv_obj_create(screen);
    lv_obj_set_style_bg_opa(container, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(container, 0, 0);
    lv_obj_set_style_pad_all(container, 0, 0);
    lv_obj_clear_flag(container, LV_OBJ_FLAG_SCROLLABLE);

    // Create digits with placeholder sizes; layout() will size/position.
    for (int i = 0; i < 6; i++) {
        initDigit(digits[i], container, 40, 76);
        setDigitImmediate(digits[i], '-');
    }

    layout();
}

void ClockScreen::destroy() {
    if (screen) {
        lv_obj_del(screen);
        screen = nullptr;
        container = nullptr;
        for (auto& d : digits) {
            d = DigitWidget{};
        }
    }
}

void ClockScreen::show() {
    if (!screen) create();
    if (screen) {
        layout();
        lv_scr_load(screen);
        lastTickMs = 0;
        lastDriftMs = 0;
        driftX = 0;
        driftY = 0;
        timeValid = false;
    }
}

void ClockScreen::updateAntiBurnInDrift() {
    if (!container) return;

    const uint32_t nowMs = millis();
    if (lastDriftMs != 0 && (uint32_t)(nowMs - lastDriftMs) < kDriftTickMs) return;
    lastDriftMs = nowMs;

    // If we don't have a valid time yet, keep it centered.
    if (!timeValid) {
        if (driftX != 0 || driftY != 0) {
            driftX = 0;
            driftY = 0;
            lv_obj_align(container, LV_ALIGN_CENTER, driftX, driftY);
        }
        return;
    }

    // Deterministic small drift pattern based on wall-clock time.
    // This avoids a perfectly static image without being distracting.
    time_t now = time(nullptr);
    struct tm t;
    localtime_r(&now, &t);

    const int step = (t.tm_min * 60 + t.tm_sec) / (kDriftTickMs / 1000);
    const int phase = step % 16; // 4x4 grid

    const int gridX = (phase % 4) - 1; // -1..2
    const int gridY = (phase / 4) - 1; // -1..2

    int16_t newX = (int16_t)(gridX * 2);
    int16_t newY = (int16_t)(gridY * 2);

    if (newX > kMaxDriftPx) newX = kMaxDriftPx;
    if (newX < -kMaxDriftPx) newX = -kMaxDriftPx;
    if (newY > kMaxDriftPx) newY = kMaxDriftPx;
    if (newY < -kMaxDriftPx) newY = -kMaxDriftPx;

    if (newX == driftX && newY == driftY) return;
    driftX = newX;
    driftY = newY;
    lv_obj_align(container, LV_ALIGN_CENTER, driftX, driftY);
}

void ClockScreen::hide() {
    // no-op
}

void ClockScreen::layout() {
    if (!container || !displayMgr) return;

    const int w = displayMgr->getActiveWidth();
    const int h = displayMgr->getActiveHeight();

    const int margin = (w >= 320) ? 18 : 12;
    const int gap = (w >= 320) ? 6 : 4;
    const int pairGap = (w >= 320) ? 14 : 10;

    constexpr int kHingeThickness = 4;

    const int avail = w - margin * 2;
    // total = 6*d + 3*gap + 2*pairGap
    const int totalSpacing = 3 * gap + 2 * pairGap;
    int dW = (int)lroundf((float)(avail - totalSpacing) / 6.0f);
    if (dW < 28) dW = 28;

    int dH = (int)lroundf((float)dW * 1.9f);
    if (dH < 56) dH = 56;

    const int groupW = 6 * dW + totalSpacing;
    const int groupH = dH;

    lv_obj_set_size(container, groupW, groupH);
    lv_obj_align(container, LV_ALIGN_CENTER, 0, 0);

    const int y = 0;
    int x = 0;

    auto placeDigit = [&](int idx) {
        DigitWidget& d = digits[idx];
        d.w = (int16_t)dW;
        d.h = (int16_t)dH;
        d.halfH = (int16_t)(dH / 2);

        lv_obj_set_size(d.root, dW, dH);
        lv_obj_set_pos(d.root, x, y);

        lv_obj_set_size(d.topHalf, dW, d.halfH);
        lv_obj_set_pos(d.topHalf, 0, 0);
        lv_obj_set_size(d.bottomHalf, dW, dH - d.halfH);
        lv_obj_set_pos(d.bottomHalf, 0, d.halfH);

        if (d.hinge) {
            lv_obj_set_size(d.hinge, dW, kHingeThickness);
            lv_obj_set_pos(d.hinge, 0, (lv_coord_t)(d.halfH - (kHingeThickness / 2)));
        }

        positionDigitLabels(d);

        // flaps
        lv_obj_set_width(d.topFlap, dW);
        lv_obj_set_pos(d.topFlap, 0, 0);

        lv_obj_set_width(d.bottomFlap, dW);
        lv_obj_set_pos(d.bottomFlap, 0, d.halfH);

        positionDigitLabels(d);

        x += dW + gap;
    };

    placeDigit(0);
    placeDigit(1);

    x += (pairGap - gap);

    placeDigit(2);
    placeDigit(3);

    x += (pairGap - gap);

    placeDigit(4);
    placeDigit(5);

    // Ensure container is centered even if rounding created a slightly different width.
    lv_obj_set_size(container, x - gap, groupH);
    lv_obj_align(container, LV_ALIGN_CENTER, 0, 0);
}

void ClockScreen::positionDigitLabels(DigitWidget& d) {
    if (!d.topLabel || !d.bottomLabel || !d.topFlapLabel || !d.bottomFlapLabel) return;

    // Center the glyph within the full card height, then clip each half container.
    const lv_coord_t glyphH = lv_font_get_line_height(&lv_font_montserrat_48);
    const lv_coord_t centeredY = (lv_coord_t)((d.h - glyphH) / 2);

    auto prepLabel = [&](lv_obj_t* label) {
        if (!label) return;
        lv_obj_set_size(label, d.w, glyphH);
    };

    prepLabel(d.topLabel);
    lv_obj_set_pos(d.topLabel, 0, centeredY);

    prepLabel(d.bottomLabel);
    lv_obj_set_pos(d.bottomLabel, 0, (lv_coord_t)(centeredY - d.halfH));

    prepLabel(d.topFlapLabel);
    lv_obj_set_pos(d.topFlapLabel, 0, centeredY);

    prepLabel(d.bottomFlapLabel);
    lv_obj_set_pos(d.bottomFlapLabel, 0, (lv_coord_t)(centeredY - d.halfH));
}

void ClockScreen::initDigit(DigitWidget& d, lv_obj_t* parent, int16_t w, int16_t h) {
    d.owner = this;
    d.root = lv_obj_create(parent);
    lv_obj_set_size(d.root, w, h);
    lv_obj_set_style_bg_color(d.root, c_card(), 0);
    lv_obj_set_style_bg_grad_color(d.root, c_card2(), 0);
    lv_obj_set_style_bg_grad_dir(d.root, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_opa(d.root, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(d.root, 10, 0);
    lv_obj_set_style_border_width(d.root, 0, 0);
    lv_obj_set_style_pad_all(d.root, 0, 0);
    lv_obj_clear_flag(d.root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(d.root, LV_OBJ_FLAG_CLICKABLE);

    // Soft shadow helps sell "physical" cards without a crisp outline.
    lv_obj_set_style_shadow_width(d.root, 10, 0);
    lv_obj_set_style_shadow_spread(d.root, 1, 0);
    lv_obj_set_style_shadow_opa(d.root, LV_OPA_20, 0);
    lv_obj_set_style_shadow_color(d.root, lv_color_black(), 0);
    lv_obj_set_style_shadow_ofs_y(d.root, 2, 0);

    d.halfH = (int16_t)(h / 2);

    d.topHalf = lv_obj_create(d.root);
    lv_obj_set_size(d.topHalf, w, d.halfH);
    // Transparent clipping container (card material is drawn by d.root).
    lv_obj_set_style_bg_opa(d.topHalf, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(d.topHalf, 0, 0);
    lv_obj_set_style_radius(d.topHalf, 0, 0);
    lv_obj_set_style_pad_all(d.topHalf, 0, 0);
    lv_obj_clear_flag(d.topHalf, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    lv_obj_clear_flag(d.topHalf, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(d.topHalf, LV_OBJ_FLAG_CLICKABLE);

    d.bottomHalf = lv_obj_create(d.root);
    lv_obj_set_size(d.bottomHalf, w, (int16_t)(h - d.halfH));
    lv_obj_set_pos(d.bottomHalf, 0, d.halfH);
    // Transparent clipping container (card material is drawn by d.root).
    lv_obj_set_style_bg_opa(d.bottomHalf, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(d.bottomHalf, 0, 0);
    lv_obj_set_style_radius(d.bottomHalf, 0, 0);
    lv_obj_set_style_pad_all(d.bottomHalf, 0, 0);
    lv_obj_clear_flag(d.bottomHalf, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    lv_obj_clear_flag(d.bottomHalf, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(d.bottomHalf, LV_OBJ_FLAG_CLICKABLE);

    // Hinge line across the middle
    d.hinge = lv_obj_create(d.root);
    lv_obj_set_size(d.hinge, w, 4);
    lv_obj_set_pos(d.hinge, 0, d.halfH - 2);
    lv_obj_set_style_bg_color(d.hinge, c_hinge(), 0);
    lv_obj_set_style_bg_opa(d.hinge, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(d.hinge, 0, 0);
    lv_obj_set_style_pad_all(d.hinge, 0, 0);
    lv_obj_clear_flag(d.hinge, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(d.hinge, LV_OBJ_FLAG_CLICKABLE);

    // Small hinge "rivets" near left/right edges.
    for (int i = 0; i < 2; i++) {
        lv_obj_t* rivet = lv_obj_create(d.root);
        lv_obj_set_size(rivet, 4, 4);
        lv_obj_set_style_radius(rivet, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(rivet, c_rivet(), 0);
        lv_obj_set_style_bg_opa(rivet, LV_OPA_70, 0);
        lv_obj_set_style_border_width(rivet, 0, 0);
        lv_obj_set_style_pad_all(rivet, 0, 0);
        lv_obj_clear_flag(rivet, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_clear_flag(rivet, LV_OBJ_FLAG_CLICKABLE);
        const lv_coord_t x = (i == 0) ? 8 : (lv_coord_t)(w - 12);
        lv_obj_set_pos(rivet, x, (lv_coord_t)(d.halfH - 2));
    }

    d.topLabel = lv_label_create(d.topHalf);
    lv_obj_set_style_text_color(d.topLabel, c_text(), 0);
    lv_obj_set_style_text_opa(d.topLabel, LV_OPA_COVER, 0);
    lv_obj_set_style_text_font(d.topLabel, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_align(d.topLabel, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_clear_flag(d.topLabel, LV_OBJ_FLAG_CLICKABLE);

    d.bottomLabel = lv_label_create(d.bottomHalf);
    lv_obj_set_style_text_color(d.bottomLabel, c_text(), 0);
    lv_obj_set_style_text_opa(d.bottomLabel, LV_OPA_COVER, 0);
    lv_obj_set_style_text_font(d.bottomLabel, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_align(d.bottomLabel, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_clear_flag(d.bottomLabel, LV_OBJ_FLAG_CLICKABLE);

    // Flaps (overlays) - hidden until animating.
    d.topFlap = lv_obj_create(d.root);
    lv_obj_set_size(d.topFlap, w, d.halfH);
    lv_obj_set_style_bg_color(d.topFlap, c_flap(), 0);
    lv_obj_set_style_bg_opa(d.topFlap, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(d.topFlap, 10, 0);
    lv_obj_set_style_border_width(d.topFlap, 0, 0);
    lv_obj_set_style_pad_all(d.topFlap, 0, 0);
    lv_obj_clear_flag(d.topFlap, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    lv_obj_add_flag(d.topFlap, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(d.topFlap, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(d.topFlap, LV_OBJ_FLAG_CLICKABLE);

    d.topFlapLabel = lv_label_create(d.topFlap);
    lv_obj_set_style_text_color(d.topFlapLabel, c_text(), 0);
    lv_obj_set_style_text_opa(d.topFlapLabel, LV_OPA_COVER, 0);
    lv_obj_set_style_text_font(d.topFlapLabel, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_align(d.topFlapLabel, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_clear_flag(d.topFlapLabel, LV_OBJ_FLAG_CLICKABLE);

    d.bottomFlap = lv_obj_create(d.root);
    lv_obj_set_size(d.bottomFlap, w, d.halfH);
    lv_obj_set_pos(d.bottomFlap, 0, d.halfH);
    lv_obj_set_style_bg_color(d.bottomFlap, c_flap(), 0);
    lv_obj_set_style_bg_opa(d.bottomFlap, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(d.bottomFlap, 10, 0);
    lv_obj_set_style_border_width(d.bottomFlap, 0, 0);
    lv_obj_set_style_pad_all(d.bottomFlap, 0, 0);
    lv_obj_clear_flag(d.bottomFlap, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    lv_obj_add_flag(d.bottomFlap, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(d.bottomFlap, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(d.bottomFlap, LV_OBJ_FLAG_CLICKABLE);

    d.bottomFlapLabel = lv_label_create(d.bottomFlap);
    lv_obj_set_style_text_color(d.bottomFlapLabel, c_text(), 0);
    lv_obj_set_style_text_opa(d.bottomFlapLabel, LV_OPA_COVER, 0);
    lv_obj_set_style_text_font(d.bottomFlapLabel, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_align(d.bottomFlapLabel, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_clear_flag(d.bottomFlapLabel, LV_OBJ_FLAG_CLICKABLE);

    // Ensure initial label positions are correct for clipping.
    d.w = w;
    d.h = h;
    positionDigitLabels(d);
}

void ClockScreen::setDigitImmediate(DigitWidget& d, char c) {
    d.current = c;
    d.pending = c;
    d.animating = false;

    setLabelChar(d.topLabel, c);
    setLabelChar(d.bottomLabel, c);

    if (d.topFlap) lv_obj_add_flag(d.topFlap, LV_OBJ_FLAG_HIDDEN);
    if (d.bottomFlap) lv_obj_add_flag(d.bottomFlap, LV_OBJ_FLAG_HIDDEN);
}

void ClockScreen::requestDigit(DigitWidget& d, char c) {
    if (d.current == c && !d.animating) return;
    d.pending = c;
    if (!d.animating) {
        startFlip(d, d.current, d.pending);
    }
}

void ClockScreen::startFlip(DigitWidget& d, char from, char to) {
    if (!d.root || !d.topFlap || !d.bottomFlap) {
        setDigitImmediate(d, to);
        return;
    }

    d.animating = true;
    d.flipFrom = from;
    d.flipTo = to;

    setLabelChar(d.topFlapLabel, from);
    setLabelChar(d.bottomFlapLabel, to);

    lv_obj_clear_flag(d.topFlap, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(d.bottomFlap, LV_OBJ_FLAG_HIDDEN);

    // Top flap starts fully visible.
    lv_obj_set_pos(d.topFlap, 0, 0);
    lv_obj_set_size(d.topFlap, d.w, d.halfH);

    // Reset flap styles for a clean start.
    lv_obj_set_style_bg_color(d.topFlap, c_flap(), 0);
    lv_obj_set_style_text_opa(d.topFlapLabel, LV_OPA_COVER, 0);

    // Bottom flap starts collapsed.
    lv_obj_set_pos(d.bottomFlap, 0, d.halfH);
    lv_obj_set_size(d.bottomFlap, d.w, 0);

    lv_obj_set_style_bg_color(d.bottomFlap, c_flap(), 0);
    lv_obj_set_style_text_opa(d.bottomFlapLabel, LV_OPA_TRANSP, 0);

    // Ensure label baseline positions are correct before we start applying offsets.
    if (d.owner) {
        d.owner->positionDigitLabels(d);
    }

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, &d);
    lv_anim_set_exec_cb(&a, ClockScreen::animSetTopFlapHeight);
    lv_anim_set_values(&a, d.halfH, 0);
    lv_anim_set_time(&a, kFlipPhaseMs);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in);
    lv_anim_set_ready_cb(&a, ClockScreen::animTopFlapReady);
    lv_anim_start(&a);
}

void ClockScreen::animSetTopFlapHeight(void* var, int32_t v) {
    DigitWidget* dptr = (DigitWidget*)var;
    if (!dptr) return;
    DigitWidget& d = *dptr;
    if (!d.topFlap) return;

    if (v < 0) v = 0;
    if (v > d.halfH) v = d.halfH;

    const int32_t halfH = d.halfH > 0 ? d.halfH : 1;
    const int32_t inv = halfH - v;

    // Fade the digit as the flap closes (quadratic feels more physical than linear).
    const uint32_t vv = (uint32_t)v * (uint32_t)v;
    const uint32_t hh = (uint32_t)halfH * (uint32_t)halfH;
    const lv_opa_t textOpa = (lv_opa_t)((vv * (uint32_t)LV_OPA_COVER) / (hh ? hh : 1));
    if (d.topFlapLabel) {
        lv_obj_set_style_text_opa(d.topFlapLabel, textOpa, 0);
    }

    // Darken the flap slightly as it closes.
    int shade = 32 - (int)((inv * 18) / halfH);
    if (shade < 10) shade = 10;
    if (shade > 40) shade = 40;
    lv_obj_set_style_bg_color(d.topFlap, lv_color_make(shade, shade, shade + 2), 0);

    // Nudge the label toward the hinge for a small perspective hint.
    if (d.topFlapLabel) {
        const lv_coord_t glyphH = lv_font_get_line_height(&lv_font_montserrat_48);
        const lv_coord_t baseY = (lv_coord_t)((d.h - glyphH) / 2);
        const lv_coord_t dy = (lv_coord_t)((inv * 6) / halfH);
        lv_obj_set_pos(d.topFlapLabel, 0, (lv_coord_t)(baseY + dy));
    }

    lv_obj_set_size(d.topFlap, d.w, (lv_coord_t)v);
    // Keep it pinned to top.
    lv_obj_set_pos(d.topFlap, 0, 0);
}

void ClockScreen::animTopFlapReady(lv_anim_t* a) {
    DigitWidget* dptr = (DigitWidget*)a->var;
    if (!dptr) return;
    DigitWidget& d = *dptr;
    ClockScreen* self = d.owner;
    if (!self) return;

    // Swap the top half to the new digit.
    setLabelChar(d.topLabel, d.flipTo);
    if (d.topFlap) {
        lv_obj_add_flag(d.topFlap, LV_OBJ_FLAG_HIDDEN);
    }

    // Small pause before opening bottom flap for a nicer cadence.
    lv_anim_t b;
    lv_anim_init(&b);
    lv_anim_set_var(&b, &d);
    lv_anim_set_exec_cb(&b, ClockScreen::animSetBottomFlapHeight);
    lv_anim_set_values(&b, 0, d.halfH);
    lv_anim_set_time(&b, kFlipPhaseMs);
    lv_anim_set_delay(&b, kFlipPauseMs);
    lv_anim_set_path_cb(&b, lv_anim_path_ease_out);
    lv_anim_set_ready_cb(&b, ClockScreen::animBottomFlapReady);
    lv_anim_start(&b);

    (void)self;
}

void ClockScreen::animSetBottomFlapHeight(void* var, int32_t v) {
    DigitWidget* dptr = (DigitWidget*)var;
    if (!dptr) return;
    DigitWidget& d = *dptr;
    if (!d.bottomFlap) return;

    if (v < 0) v = 0;
    if (v > d.halfH) v = d.halfH;

    const int32_t halfH = d.halfH > 0 ? d.halfH : 1;
    const int32_t inv = halfH - v;

    // Fade the digit in as the flap opens.
    const uint32_t vv = (uint32_t)v * (uint32_t)v;
    const uint32_t hh = (uint32_t)halfH * (uint32_t)halfH;
    const lv_opa_t textOpa = (lv_opa_t)((vv * (uint32_t)LV_OPA_COVER) / (hh ? hh : 1));
    if (d.bottomFlapLabel) {
        lv_obj_set_style_text_opa(d.bottomFlapLabel, textOpa, 0);
    }

    // Brighten slightly as it opens.
    int shade = 12 + (int)((v * 18) / halfH);
    if (shade < 8) shade = 8;
    if (shade > 40) shade = 40;
    lv_obj_set_style_bg_color(d.bottomFlap, lv_color_make(shade, shade, shade + 2), 0);

    // Nudge the label toward the hinge while opening.
    if (d.bottomFlapLabel) {
        const lv_coord_t glyphH = lv_font_get_line_height(&lv_font_montserrat_48);
        const lv_coord_t baseY = (lv_coord_t)((d.h - glyphH) / 2);
        const lv_coord_t dy = (lv_coord_t)((inv * 6) / halfH);
        lv_obj_set_pos(d.bottomFlapLabel, 0, (lv_coord_t)((baseY - d.halfH) - dy));
    }

    // Grow downward from the hinge line.
    lv_obj_set_pos(d.bottomFlap, 0, d.halfH);
    lv_obj_set_size(d.bottomFlap, d.w, (lv_coord_t)v);
}

void ClockScreen::animBottomFlapReady(lv_anim_t* a) {
    DigitWidget* dptr = (DigitWidget*)a->var;
    if (!dptr) return;
    DigitWidget& d = *dptr;
    ClockScreen* self = d.owner;
    if (!self) return;

    // Finalize bottom half and hide flap.
    setLabelChar(d.bottomLabel, d.flipTo);
    if (d.bottomFlap) {
        if (d.bottomFlapLabel) {
            lv_obj_set_style_text_opa(d.bottomFlapLabel, LV_OPA_COVER, 0);
        }
        lv_obj_add_flag(d.bottomFlap, LV_OBJ_FLAG_HIDDEN);
    }

    d.current = d.flipTo;
    d.animating = false;

    // If the digit changed again mid-animation, run the next flip.
    if (d.pending != d.current) {
        self->startFlip(d, d.current, d.pending);
        return;
    }
}

void ClockScreen::update() {
    if (!screen) return;

    const uint32_t now = millis();
    if (lastTickMs != 0 && (uint32_t)(now - lastTickMs) < kUiTickMs) {
        return;
    }
    lastTickMs = now;

    bool ok = false;
    char hms[7] = {'-', '-', '-', '-', '-', '-', '\0'};
    getHms(hms, &ok);

    if (ok != timeValid) {
        timeValid = ok;
    }

    updateAntiBurnInDrift();

    if (!timeValid) {
        // Keep showing placeholder. Avoid triggering animations.
        for (int i = 0; i < 6; i++) {
            if (!digits[i].animating && digits[i].current != '-') {
                setDigitImmediate(digits[i], '-');
            }
        }
        return;
    }

    // Update digits (HHMMSS)
    for (int i = 0; i < 6; i++) {
        requestDigit(digits[i], hms[i]);
    }
}

void ClockScreen::onScreenClicked(lv_event_t* e) {
    ClockScreen* self = (ClockScreen*)lv_event_get_user_data(e);
    if (!self || !self->displayMgr) return;

    // Convenience: tap anywhere to go back to previous screen (or macro1).
    self->displayMgr->goBackOrDefault();
}

bool ClockScreen::isTimeValid() {
    time_t now = time(nullptr);
    // Consider time valid if it's after 2020-01-01.
    return now > 1577836800;
}

void ClockScreen::getHms(char out[7], bool* ok) {
    if (ok) *ok = false;
    if (!out) return;

    if (!isTimeValid()) {
        out[0] = '-'; out[1] = '-'; out[2] = '-'; out[3] = '-'; out[4] = '-'; out[5] = '-'; out[6] = '\0';
        return;
    }

    time_t now = time(nullptr);
    struct tm t;
    localtime_r(&now, &t);

    out[0] = (char)('0' + ((t.tm_hour / 10) % 10));
    out[1] = (char)('0' + (t.tm_hour % 10));
    out[2] = (char)('0' + ((t.tm_min / 10) % 10));
    out[3] = (char)('0' + (t.tm_min % 10));
    out[4] = (char)('0' + ((t.tm_sec / 10) % 10));
    out[5] = (char)('0' + (t.tm_sec % 10));
    out[6] = '\0';

    if (ok) *ok = true;
}
