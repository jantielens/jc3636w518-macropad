#include "macropad_layout.h"

#include "../display_manager.h"
#include "../macro_templates.h"

namespace macropad_layout {

namespace {

class MacroPadLayoutFourSplit final : public IMacroPadLayout {
public:
    const char* id() const override { return macro_templates::kTemplateSplitSides4; }

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

        for (int i = 0; i < MACROS_BUTTONS_PER_SCREEN; i++) {
            if (!buttons[i]) continue;
            const bool used = (i == 0 || i == 2 || i == 3 || i == 4);
            if (used) {
                lv_obj_set_style_radius(buttons[i], 10, 0);
                lv_obj_set_style_border_width(buttons[i], 0, 0);
                lv_obj_clear_flag(buttons[i], LV_OBJ_FLAG_HIDDEN);
            } else {
                lv_obj_add_flag(buttons[i], LV_OBJ_FLAG_HIDDEN);
            }
        }

        int topH = (fullH - spacing) / 2;
        int bottomH = fullH - topH - spacing;
        if (topH < minTouchPx || bottomH < minTouchPx) {
            topH = (topH < minTouchPx) ? minTouchPx : topH;
            bottomH = fullH - topH - spacing;
            if (bottomH < minTouchPx) {
                bottomH = minTouchPx;
                topH = fullH - bottomH - spacing;
                if (topH < minTouchPx) topH = minTouchPx;
            }
        }

        if (buttons[0]) {
            lv_obj_set_pos(buttons[0], xCenter, yTop);
            lv_obj_set_size(buttons[0], centerW, topH);
            if (labels && labels[0]) {
                lv_obj_set_width(labels[0], (centerW > 12) ? (centerW - 12) : centerW);
                lv_obj_center(labels[0]);
            }
        }

        if (buttons[2]) {
            lv_obj_set_pos(buttons[2], xCenter, yTop + topH + spacing);
            lv_obj_set_size(buttons[2], centerW, bottomH);
            if (labels && labels[2]) {
                lv_obj_set_width(labels[2], (centerW > 12) ? (centerW - 12) : centerW);
                lv_obj_center(labels[2]);
            }
        }

        if (buttons[3]) {
            lv_obj_set_pos(buttons[3], xLeft, yTop);
            lv_obj_set_size(buttons[3], sideW, fullH);
            if (labels && labels[3]) {
                lv_obj_set_width(labels[3], (sideW > 8) ? (sideW - 8) : sideW);
                lv_obj_center(labels[3]);
            }
        }

        if (buttons[4]) {
            lv_obj_set_pos(buttons[4], xRight, yTop);
            lv_obj_set_size(buttons[4], sideW, fullH);
            if (labels && labels[4]) {
                lv_obj_set_width(labels[4], (sideW > 8) ? (sideW - 8) : sideW);
                lv_obj_center(labels[4]);
            }
        }
    }

    bool isSlotUsed(uint8_t slot) const override {
        return slot == 0 || slot == 2 || slot == 3 || slot == 4;
    }
};

} // namespace

const IMacroPadLayout& layout_four_split() {
    static MacroPadLayoutFourSplit s;
    return s;
}

} // namespace macropad_layout
