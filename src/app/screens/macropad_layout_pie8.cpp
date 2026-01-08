#include "macropad_layout.h"

#include "../display_manager.h"
#include "../macro_templates.h"

#include <math.h>

namespace macropad_layout {

namespace {

static float clampf(float v, float lo, float hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

class MacroPadLayoutPie8 final : public IMacroPadLayout {
public:
    const char* id() const override { return macro_templates::kTemplateRoundPie8; }

    bool isPie() const override { return true; }

    void apply(MacroPadLayoutContext& ctx) const override {
        if (!ctx.screen || !ctx.displayMgr) return;
        lv_obj_t** buttons = ctx.buttons;
        lv_obj_t** labels = ctx.labels;
        lv_obj_t** pieSegments = ctx.pieSegments;

        const int w = ctx.displayMgr->getActiveWidth();
        const int h = ctx.displayMgr->getActiveHeight();
        const int cx = w / 2;
        const int cy = h / 2;
        const int minDim = (w < h) ? w : h;

        const int ringSize = minDim;
        const int ringX = cx - (ringSize / 2);
        const int ringY = cy - (ringSize / 2);

        const float half = (float)minDim * 0.5f;

        const float baseSeparatorPx = clampf((float)minDim * 0.015f, 6.0f, 12.0f);
        const float separatorPx = baseSeparatorPx + 3.0f;

        const int arcWidth = (int)clampf((float)minDim * 0.22f, 44.0f, half * 0.60f);
        const float ringOuter = half;
        const float ringInnerEdge = clampf(ringOuter - (float)arcWidth, 0.0f, ringOuter);
        const float rStrokeMid = ringOuter - ((float)arcWidth * 0.5f);
        const float gapDeg = (rStrokeMid > 1.0f)
            ? (separatorPx / rStrokeMid) * (180.0f / (float)M_PI)
            : 0.0f;
        const float sweepDeg = 45.0f - gapDeg;

        if (ctx.pieHitLayer) {
            lv_obj_set_pos(ctx.pieHitLayer, 0, 0);
            lv_obj_set_size(ctx.pieHitLayer, w, h);
            lv_obj_clear_flag(ctx.pieHitLayer, LV_OBJ_FLAG_HIDDEN);
            lv_obj_move_foreground(ctx.pieHitLayer);
        }

        for (int i = 0; i < 8; i++) {
            lv_obj_t* seg = pieSegments ? pieSegments[i] : nullptr;
            if (!seg) continue;

            lv_obj_set_pos(seg, ringX, ringY);
            lv_obj_set_size(seg, ringSize, ringSize);

            lv_obj_set_style_arc_width(seg, arcWidth, LV_PART_INDICATOR);

            const float centerDeg = 270.0f + (float)i * 45.0f;
            const float startF = centerDeg - (sweepDeg * 0.5f);
            const float endF = centerDeg + (sweepDeg * 0.5f);
            int start = (int)lroundf(startF);
            int end = (int)lroundf(endF);
            start %= 360;
            end %= 360;
            if (start < 0) start += 360;
            if (end < 0) end += 360;

            lv_arc_set_rotation(seg, 0);
            lv_arc_set_bg_angles(seg, 0, 0);
            lv_arc_set_angles(seg, start, end);
            lv_obj_move_background(seg);
        }

        const float rMid = rStrokeMid + (separatorPx * 0.5f);
        int outerBox = (int)clampf((float)arcWidth * 1.10f, 64.0f, 128.0f);
        const int outerRadius = outerBox / 2;
        const int labelWidth = (outerBox > 24) ? (outerBox - 18) : outerBox;

        for (int i = 0; i < 8; i++) {
            if (!buttons[i]) continue;

            lv_obj_set_style_bg_opa(buttons[i], LV_OPA_TRANSP, 0);
            lv_obj_set_style_border_width(buttons[i], 0, 0);
            lv_obj_set_style_radius(buttons[i], 0, 0);

            lv_obj_clear_flag(buttons[i], LV_OBJ_FLAG_HIDDEN);

            lv_obj_set_size(buttons[i], outerBox, outerBox);

            const float deg = -90.0f + (float)i * 45.0f;
            const float rad = deg * (float)M_PI / 180.0f;
            const int bx = (int)lroundf((float)cx + rMid * cosf(rad));
            const int by = (int)lroundf((float)cy + rMid * sinf(rad));
            lv_obj_set_pos(buttons[i], bx - outerRadius, by - outerRadius);

            if (labels && labels[i]) {
                lv_obj_set_width(labels[i], labelWidth);
                lv_obj_center(labels[i]);
            }
        }

        int centerBox = (int)clampf((ringInnerEdge - separatorPx) * 2.0f, 72.0f, (float)minDim);
        const int centerRadius = centerBox / 2;
        const int centerLabelWidth = (centerBox > 24) ? (centerBox - 18) : centerBox;

        if (buttons[8]) {
            lv_obj_set_style_radius(buttons[8], LV_RADIUS_CIRCLE, 0);
            lv_obj_set_style_border_width(buttons[8], 0, 0);
            lv_obj_clear_flag(buttons[8], LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_size(buttons[8], centerBox, centerBox);
            lv_obj_set_pos(buttons[8], cx - centerRadius, cy - centerRadius);
            if (labels && labels[8]) {
                lv_obj_set_width(labels[8], centerLabelWidth);
                lv_obj_center(labels[8]);
            }
        }

        for (int i = 9; i < MACROS_BUTTONS_PER_SCREEN; i++) {
            if (buttons[i]) lv_obj_add_flag(buttons[i], LV_OBJ_FLAG_HIDDEN);
        }
    }

    bool isSlotUsed(uint8_t slot) const override { return slot < 9; }

    int slotFromPoint(int x, int y, const MacroPadLayoutContext& ctx) const override {
        if (!ctx.displayMgr) return -1;

        const int w = ctx.displayMgr->getActiveWidth();
        const int h = ctx.displayMgr->getActiveHeight();
        const int cx = w / 2;
        const int cy = h / 2;
        const int dx = x - cx;
        const int dy = y - cy;

        const int minDim = (w < h) ? w : h;
        const float half = (float)minDim * 0.5f;

        // Keep hit-testing geometry consistent with apply() ring geometry.
        const float arcWidth = clampf((float)minDim * 0.22f, 44.0f, half * 0.60f);
        const float baseSeparatorPx = clampf((float)minDim * 0.015f, 6.0f, 12.0f);
        const float separatorPx = baseSeparatorPx + 3.0f;

        const float ringOuter = half;
        const float ringInnerEdge = clampf(ringOuter - arcWidth, 0.0f, ringOuter);
        const float centerR = clampf(ringInnerEdge - separatorPx, 0.0f, ringOuter);
        const float ringInner = ringInnerEdge;

        const float r2 = (float)dx * (float)dx + (float)dy * (float)dy;
        const float centerR2 = centerR * centerR;
        const float ringInner2 = ringInner * ringInner;
        const float ringOuter2 = ringOuter * ringOuter;

        // Center slot
        if (r2 <= centerR2) return 8;
        // Ring area only
        if (r2 < ringInner2 || r2 > ringOuter2) return -1;

        // Angle where 0° is at the top and increases clockwise.
        // (x,y) screen coords => +y down.
        float ang = atan2f((float)dx, (float)-dy) * 180.0f / (float)M_PI;
        if (ang < 0.0f) ang += 360.0f;

        const float slice = 45.0f;
        int slot = (int)floorf((ang + (slice * 0.5f)) / slice);
        slot %= 8;
        if (slot < 0) slot += 8;

        // Avoid clicks on the separator gaps between wedges.
        const float rStrokeMid = ringOuter - (arcWidth * 0.5f);
        const float gapDeg = (rStrokeMid > 1.0f)
            ? (separatorPx / rStrokeMid) * (180.0f / (float)M_PI)
            : 0.0f;
        const float sweepDeg = slice - gapDeg;
        const float slotCenter = (float)slot * slice;
        float delta = ang - slotCenter;
        while (delta > 180.0f) delta -= 360.0f;
        while (delta <= -180.0f) delta += 360.0f;
        if (fabsf(delta) > (sweepDeg * 0.5f)) return -1;

        return slot;
    }
};

} // namespace

const IMacroPadLayout& layout_pie8() {
    static MacroPadLayoutPie8 s;
    return s;
}

} // namespace macropad_layout
