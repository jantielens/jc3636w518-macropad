#include "macropad_layout.h"

#include "../display_manager.h"
#include "../macro_templates.h"

#include <math.h>

namespace macropad_layout {

namespace {

class MacroPadLayoutRound9 final : public IMacroPadLayout {
public:
    const char* id() const override { return macro_templates::kTemplateRoundRing9; }

    void apply(MacroPadLayoutContext& ctx) const override {
        if (!ctx.screen || !ctx.displayMgr) return;
        lv_obj_t** buttons = ctx.buttons;
        lv_obj_t** labels = ctx.labels;

        const int w = ctx.displayMgr->getActiveWidth();
        const int h = ctx.displayMgr->getActiveHeight();
        const int cx = w / 2;
        const int cy = h / 2;

        const int minDim = (w < h) ? w : h;
        const float half = (float)minDim * 0.5f;

        const int desiredGapPx = 1;
        const float s = sinf((float)M_PI / 8.0f); // sin(22.5°)
        float r = (s * (float)minDim - (float)desiredGapPx) / (2.0f * (1.0f + s));
        if (r < 18.0f) r = 18.0f;

        int btnSize = (int)floorf(2.0f * r);
        if (btnSize < 36) btnSize = 36;
        if (btnSize > minDim) btnSize = minDim;
        const int btnRadius = btnSize / 2;

        const float outerRadius = half - (float)btnRadius;

        const int labelWidth = (btnSize > 24) ? (btnSize - 18) : btnSize;

        for (int i = 0; i < 9; i++) {
            if (!buttons[i]) continue;
            lv_obj_set_style_radius(buttons[i], LV_RADIUS_CIRCLE, 0);
            lv_obj_set_style_border_width(buttons[i], 0, 0);
            lv_obj_clear_flag(buttons[i], LV_OBJ_FLAG_HIDDEN);
        }
        for (int i = 9; i < MACROS_BUTTONS_PER_SCREEN; i++) {
            if (buttons[i]) lv_obj_add_flag(buttons[i], LV_OBJ_FLAG_HIDDEN);
        }

        if (buttons[8]) {
            lv_obj_set_size(buttons[8], btnSize, btnSize);
            lv_obj_set_pos(buttons[8], cx - btnRadius, cy - btnRadius);
            if (labels && labels[8]) {
                lv_obj_set_width(labels[8], labelWidth);
                lv_obj_center(labels[8]);
            }
        }

        for (int i = 0; i < 8; i++) {
            if (!buttons[i]) continue;

            lv_obj_set_size(buttons[i], btnSize, btnSize);

            const float deg = -90.0f + (float)i * 45.0f;
            const float rad = deg * (float)M_PI / 180.0f;

            const int bx = (int)lroundf((float)cx + outerRadius * cosf(rad));
            const int by = (int)lroundf((float)cy + outerRadius * sinf(rad));

            lv_obj_set_pos(buttons[i], bx - btnRadius, by - btnRadius);

            if (labels && labels[i]) {
                lv_obj_set_width(labels[i], labelWidth);
                lv_obj_center(labels[i]);
            }
        }
    }

    bool isSlotUsed(uint8_t slot) const override { return slot < 9; }
};

} // namespace

const IMacroPadLayout& layout_round9() {
    static MacroPadLayoutRound9 s;
    return s;
}

} // namespace macropad_layout
