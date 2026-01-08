#include "macropad_layout.h"

#include "../display_manager.h"
#include "../macro_templates.h"

namespace macropad_layout {

namespace {

class MacroPadLayoutFiveStack final : public IMacroPadLayout {
public:
    const char* id() const override { return macro_templates::kTemplateStackSides5; }

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
        const int usableH = h;

        int topH = (int)((float)usableH * 0.30f);
        int bottomH = topH;
        if (topH < minTouchPx) topH = minTouchPx;
        if (bottomH < minTouchPx) bottomH = minTouchPx;
        int middleH = usableH - topH - bottomH - (2 * spacing);
        if (middleH < minTouchPx) {
            const int need = minTouchPx - middleH;
            const int stealEach = (need + 1) / 2;
            topH = (topH - stealEach > minTouchPx) ? (topH - stealEach) : minTouchPx;
            bottomH = (bottomH - stealEach > minTouchPx) ? (bottomH - stealEach) : minTouchPx;
            middleH = usableH - topH - bottomH - (2 * spacing);
            if (middleH < minTouchPx) middleH = minTouchPx;
        }

        for (int i = 0; i < 5; i++) {
            if (!buttons[i]) continue;
            lv_obj_set_style_radius(buttons[i], 10, 0);
            lv_obj_set_style_border_width(buttons[i], 0, 0);
            lv_obj_clear_flag(buttons[i], LV_OBJ_FLAG_HIDDEN);
        }
        for (int i = 5; i < MACROS_BUTTONS_PER_SCREEN; i++) {
            if (buttons[i]) lv_obj_add_flag(buttons[i], LV_OBJ_FLAG_HIDDEN);
        }

        if (buttons[0]) {
            lv_obj_set_pos(buttons[0], xCenter, yTop);
            lv_obj_set_size(buttons[0], centerW, topH);
            if (labels && labels[0]) {
                lv_obj_set_width(labels[0], (centerW > 12) ? (centerW - 12) : centerW);
                lv_obj_center(labels[0]);
            }
        }
        if (buttons[1]) {
            lv_obj_set_pos(buttons[1], xCenter, yTop + topH + spacing);
            lv_obj_set_size(buttons[1], centerW, middleH);
            if (labels && labels[1]) {
                lv_obj_set_width(labels[1], (centerW > 12) ? (centerW - 12) : centerW);
                lv_obj_center(labels[1]);
            }
        }
        if (buttons[2]) {
            lv_obj_set_pos(buttons[2], xCenter, yTop + usableH - bottomH);
            lv_obj_set_size(buttons[2], centerW, bottomH);
            if (labels && labels[2]) {
                lv_obj_set_width(labels[2], (centerW > 12) ? (centerW - 12) : centerW);
                lv_obj_center(labels[2]);
            }
        }

        const int ySide = yTop;
        const int sideH = usableH;
        if (buttons[3]) {
            lv_obj_set_pos(buttons[3], xLeft, ySide);
            lv_obj_set_size(buttons[3], sideW, sideH);
            if (labels && labels[3]) {
                lv_obj_set_width(labels[3], (sideW > 8) ? (sideW - 8) : sideW);
                lv_obj_center(labels[3]);
            }
        }
        if (buttons[4]) {
            lv_obj_set_pos(buttons[4], xRight, ySide);
            lv_obj_set_size(buttons[4], sideW, sideH);
            if (labels && labels[4]) {
                lv_obj_set_width(labels[4], (sideW > 8) ? (sideW - 8) : sideW);
                lv_obj_center(labels[4]);
            }
        }
    }

    bool isSlotUsed(uint8_t slot) const override { return slot < 5; }
};

} // namespace

const IMacroPadLayout& layout_five_stack() {
    static MacroPadLayoutFiveStack s;
    return s;
}

} // namespace macropad_layout
