#include "macropad_layout.h"

#include "../display_manager.h"
#include "../macro_templates.h"

namespace macropad_layout {

namespace {

class MacroPadLayoutWideCenter final : public IMacroPadLayout {
public:
    const char* id() const override { return macro_templates::kTemplateWideSides3; }

    void apply(MacroPadLayoutContext& ctx) const override {
        if (!ctx.screen || !ctx.displayMgr) return;
        lv_obj_t** buttons = ctx.buttons;
        lv_obj_t** labels = ctx.labels;

        const int w = ctx.displayMgr->getActiveWidth();
        const int h = ctx.displayMgr->getActiveHeight();

        const int padX = (w + h) / 2 / 24;
        const int spacing = (padX >= 9) ? (padX / 3) : 3;

        const int minTouchPx = 52;
        const int minCenterW = minTouchPx * 2;

        int sideW = (int)((float)w * 0.18f);
        if (sideW < minTouchPx) sideW = minTouchPx;
        const int maxSideW = (w - minCenterW - (2 * spacing)) / 2;
        if (sideW > maxSideW) sideW = maxSideW;

        const int xLeft = 0;
        const int xCenter = sideW + spacing;
        int centerW = w - (2 * sideW) - (2 * spacing);
        if (centerW < minCenterW) centerW = minCenterW;
        const int xRight = w - sideW;

        const int yTop = 0;
        const int fullH = h;

        for (int i = 0; i < 3; i++) {
            if (!buttons[i]) continue;
            lv_obj_set_style_radius(buttons[i], 10, 0);
            lv_obj_set_style_border_width(buttons[i], 0, 0);
            lv_obj_clear_flag(buttons[i], LV_OBJ_FLAG_HIDDEN);
        }
        for (int i = 3; i < MACROS_BUTTONS_PER_SCREEN; i++) {
            if (buttons[i]) lv_obj_add_flag(buttons[i], LV_OBJ_FLAG_HIDDEN);
        }

        if (buttons[0]) {
            lv_obj_set_pos(buttons[0], xCenter, yTop);
            lv_obj_set_size(buttons[0], centerW, fullH);
            if (labels && labels[0]) {
                lv_obj_set_width(labels[0], (centerW > 12) ? (centerW - 12) : centerW);
                lv_obj_center(labels[0]);
            }
        }

        if (buttons[1]) {
            lv_obj_set_pos(buttons[1], xRight, yTop);
            lv_obj_set_size(buttons[1], sideW, fullH);
            if (labels && labels[1]) {
                lv_obj_set_width(labels[1], (sideW > 8) ? (sideW - 8) : sideW);
                lv_obj_center(labels[1]);
            }
        }

        if (buttons[2]) {
            lv_obj_set_pos(buttons[2], xLeft, yTop);
            lv_obj_set_size(buttons[2], sideW, fullH);
            if (labels && labels[2]) {
                lv_obj_set_width(labels[2], (sideW > 8) ? (sideW - 8) : sideW);
                lv_obj_center(labels[2]);
            }
        }
    }

    bool isSlotUsed(uint8_t slot) const override { return slot < 3; }
};

} // namespace

const IMacroPadLayout& layout_wide_center() {
    static MacroPadLayoutWideCenter s;
    return s;
}

} // namespace macropad_layout
