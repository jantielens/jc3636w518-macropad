#include "macropad_screen.h"

#include "../display_manager.h"
#include "../macros_config.h"
#include "../macro_templates.h"
#include "../ble_keyboard_manager.h"
#include "../ducky_script.h"
#include "../log_manager.h"
#include "../config_manager.h"

#if HAS_DISPLAY
#include "../png_assets.h"
#endif

#if HAS_DISPLAY && HAS_ICONS
#include "../icon_registry.h"
#endif

#if HAS_DISPLAY
#include "../screen_saver_manager.h"
#endif

#include <math.h>
#include <esp_system.h>
#include <string.h>

#include <WiFi.h>

namespace {

constexpr uint32_t kUiRefreshIntervalMs = 500;

static void setButtonVisible(lv_obj_t* btn, bool visible) {
    if (!btn) return;
    if (visible) {
        lv_obj_clear_flag(btn, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(btn, LV_OBJ_FLAG_HIDDEN);
    }
}

static const char* actionToShortLabel(MacroButtonAction a) {
    switch (a) {
        case MacroButtonAction::None: return "—";
        case MacroButtonAction::SendKeys: return "Send";
        case MacroButtonAction::NavPrevScreen: return "Prev";
        case MacroButtonAction::NavNextScreen: return "Next";
        default: return "—";
    }
}

static void defaultLabel(char* out, size_t outLen, uint8_t screenIndex, uint8_t buttonIndex) {
    // screenIndex/buttonIndex are 0-based
    snprintf(out, outLen, "S%u-B%u", (unsigned)(screenIndex + 1), (unsigned)(buttonIndex + 1));
}

#if HAS_DISPLAY && HAS_ICONS
static int clampInt(int v, int lo, int hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

struct Mask2xCacheEntry {
    const lv_img_dsc_t* src64;
    lv_img_dsc_t dsc128;
    uint8_t* data128;
    uint32_t lastUseTick;
};

static Mask2xCacheEntry s_mask2xCache[4] = {};

static const lv_img_dsc_t* findCachedMask2x(const lv_img_dsc_t* src64) {
    if (!src64) return nullptr;
    for (size_t i = 0; i < (sizeof(s_mask2xCache) / sizeof(s_mask2xCache[0])); i++) {
        if (s_mask2xCache[i].src64 == src64 && s_mask2xCache[i].data128) {
            return &s_mask2xCache[i].dsc128;
        }
    }
    return nullptr;
}

static const lv_img_dsc_t* findOriginalFromMaybeMask2x(const lv_img_dsc_t* srcMaybe2x) {
    if (!srcMaybe2x) return nullptr;
    for (size_t i = 0; i < (sizeof(s_mask2xCache) / sizeof(s_mask2xCache[0])); i++) {
        if (&s_mask2xCache[i].dsc128 == srcMaybe2x && s_mask2xCache[i].src64) {
            return s_mask2xCache[i].src64;
        }
    }
    return nullptr;
}

static const lv_img_dsc_t* getOrCreateMask2x(const lv_img_dsc_t* src64) {
    if (!src64) return nullptr;
    if (src64->header.cf != LV_IMG_CF_ALPHA_8BIT) return nullptr;
    if (src64->header.w == 0 || src64->header.h == 0) return nullptr;

    if (const lv_img_dsc_t* cached = findCachedMask2x(src64)) {
        // Touch LRU.
        for (size_t i = 0; i < (sizeof(s_mask2xCache) / sizeof(s_mask2xCache[0])); i++) {
            if (s_mask2xCache[i].src64 == src64) {
                s_mask2xCache[i].lastUseTick = lv_tick_get();
                break;
            }
        }
        return cached;
    }

    // Pick an empty slot, otherwise evict the least recently used.
    size_t slot = 0;
    bool foundEmpty = false;
    uint32_t oldest = 0xFFFFFFFFu;
    for (size_t i = 0; i < (sizeof(s_mask2xCache) / sizeof(s_mask2xCache[0])); i++) {
        if (!s_mask2xCache[i].data128) {
            slot = i;
            foundEmpty = true;
            break;
        }
        if (s_mask2xCache[i].lastUseTick < oldest) {
            oldest = s_mask2xCache[i].lastUseTick;
            slot = i;
        }
    }

    // (We intentionally don't free evicted buffers to avoid heap fragmentation.
    //  Cache size is tiny and bounded.)
    if (!foundEmpty) {
        s_mask2xCache[slot].src64 = nullptr;
        s_mask2xCache[slot].data128 = nullptr;
        s_mask2xCache[slot].lastUseTick = 0;
        memset(&s_mask2xCache[slot].dsc128, 0, sizeof(s_mask2xCache[slot].dsc128));
    }

    const uint16_t srcW = src64->header.w;
    const uint16_t srcH = src64->header.h;
    const uint16_t dstW = (uint16_t)(srcW * 2);
    const uint16_t dstH = (uint16_t)(srcH * 2);
    const size_t dstSize = (size_t)dstW * (size_t)dstH;
    const size_t srcStride = (size_t)srcW;
    const size_t dstStride = (size_t)dstW;

    uint8_t* dst = (uint8_t*)lv_mem_alloc(dstSize);
    if (!dst) return nullptr;

    const uint8_t* src = (const uint8_t*)src64->data;
    for (uint16_t y = 0; y < srcH; y++) {
        for (uint16_t x = 0; x < srcW; x++) {
            const uint8_t a = src[(size_t)y * srcStride + x];
            const size_t dy0 = (size_t)(y * 2) * dstStride;
            const size_t dy1 = (size_t)(y * 2 + 1) * dstStride;
            const size_t dx = (size_t)(x * 2);
            dst[dy0 + dx] = a;
            dst[dy0 + dx + 1] = a;
            dst[dy1 + dx] = a;
            dst[dy1 + dx + 1] = a;
        }
    }

    Mask2xCacheEntry* e = &s_mask2xCache[slot];
    e->src64 = src64;
    e->data128 = dst;
    e->lastUseTick = lv_tick_get();

    e->dsc128.header.cf = LV_IMG_CF_ALPHA_8BIT;
    e->dsc128.header.always_zero = 0;
    e->dsc128.header.reserved = 0;
    e->dsc128.header.w = dstW;
    e->dsc128.header.h = dstH;
    e->dsc128.data_size = (uint32_t)dstSize;
    e->dsc128.data = dst;

    return &e->dsc128;
}

static bool normalizeIconId(const char* in, char* out, size_t outLen) {
    if (!out || outLen == 0) return false;
    out[0] = '\0';
    if (!in) return false;

    // Trim leading whitespace.
    while (*in == ' ' || *in == '\t' || *in == '\r' || *in == '\n') {
        in++;
    }

    // Copy, normalize, and track the last non-space.
    size_t w = 0;
    size_t lastNonSpace = 0;
    for (; *in && w + 1 < outLen; in++) {
        char c = *in;
        if (c == '-') c = '_';
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');

        // Keep other characters; registry lookup will just fail if invalid.
        out[w++] = c;
        if (!(c == ' ' || c == '\t' || c == '\r' || c == '\n')) {
            lastNonSpace = w;
        }
    }
    out[lastNonSpace] = '\0';

    return out[0] != '\0';
}
#endif

} // namespace

MacroPadScreen::MacroPadScreen(DisplayManager* manager, uint8_t idx)
    : displayMgr(manager), screenIndex(idx), screen(nullptr), lastUpdateMs(0) {
    configure(manager, idx);
}

void MacroPadScreen::configure(DisplayManager* manager, uint8_t idx) {
    displayMgr = manager;
    screenIndex = idx;

    lastTemplateId[0] = '\0';

    for (int i = 0; i < MACROS_BUTTONS_PER_SCREEN; i++) {
        buttons[i] = nullptr;
        labels[i] = nullptr;
        icons[i] = nullptr;
        buttonCtx[i] = {this, (uint8_t)i};
    }

    emptyStateLabel = nullptr;

    lastUpdateMs = 0;
}

MacroPadScreen::~MacroPadScreen() {
    destroy();
}

const MacroConfig* MacroPadScreen::getMacroConfig() const {
    if (!displayMgr) return nullptr;
    return displayMgr->getMacroConfig();
}

BleKeyboardManager* MacroPadScreen::getBleKeyboard() const {
    if (!displayMgr) return nullptr;
    return displayMgr->getBleKeyboard();
}

void MacroPadScreen::create() {
    if (screen) return;

    screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(screen, lv_color_black(), 0);

    // Create clickable objects with centered labels.
    // (Avoid lv_btn_create so this works even when LV_USE_BTN=0.)
    for (int i = 0; i < MACROS_BUTTONS_PER_SCREEN; i++) {
        lv_obj_t* btn = lv_obj_create(screen);
        // These are static touch targets, not scrollable containers.
        lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_style_radius(btn, 10, 0);
        lv_obj_set_style_bg_color(btn, lv_color_make(30, 30, 30), 0);
        lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
        // No outline/border by default.
        lv_obj_set_style_border_width(btn, 0, 0);
        lv_obj_set_style_outline_width(btn, 0, 0);
        lv_obj_set_style_outline_pad(btn, 0, 0);
        lv_obj_set_style_shadow_width(btn, 0, 0);
        lv_obj_set_style_pad_all(btn, 0, 0);
        lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);

        lv_obj_add_event_cb(btn, buttonEventCallback, LV_EVENT_CLICKED, &buttonCtx[i]);

        // Optional icon child (hidden by default; enabled when HAS_ICONS).
        // Keep it present so icon work is additive.
        lv_obj_t* icon = nullptr;
        #if HAS_DISPLAY && HAS_ICONS
        icon = lv_img_create(btn);
        lv_obj_add_flag(icon, LV_OBJ_FLAG_HIDDEN);
        // Image objects should never be scrollable containers.
        lv_obj_clear_flag(icon, LV_OBJ_FLAG_SCROLLABLE);
        // Some themes/style cascades can apply LV_STYLE_OPA to children.
        // Force the object itself to be opaque so the image can render.
        lv_obj_set_style_opa(icon, LV_OPA_COVER, 0);
        // Alpha-only images (LV_IMG_CF_ALPHA_*) are tinted via style.img_recolor, but
        // they still respect style.img_opa. Force to opaque so theme defaults can't
        // accidentally hide icons.
        lv_obj_set_style_img_opa(icon, LV_OPA_COVER, 0);
        // Default to no recolor unless this is a mask icon.
        lv_obj_set_style_img_recolor_opa(icon, LV_OPA_TRANSP, 0);
        lv_obj_clear_flag(icon, LV_OBJ_FLAG_CLICKABLE);
        #endif

        lv_obj_t* lbl = lv_label_create(btn);
        lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_color(lbl, lv_color_white(), 0);
        lv_label_set_long_mode(lbl, LV_LABEL_LONG_WRAP);
        // Width is updated in layoutButtons() once button size is known.
        lv_obj_center(lbl);

        buttons[i] = btn;
        labels[i] = lbl;
        icons[i] = icon;
    }

    // Empty-state helper (shown only on Macro Screen 1 when no macros are configured).
    emptyStateLabel = lv_label_create(screen);
    lv_obj_set_style_text_align(emptyStateLabel, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(emptyStateLabel, lv_color_make(180, 180, 180), 0);
    lv_label_set_long_mode(emptyStateLabel, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(emptyStateLabel, lv_pct(92));
    lv_obj_align(emptyStateLabel, LV_ALIGN_CENTER, 0, 0);
    lv_obj_clear_flag(emptyStateLabel, LV_OBJ_FLAG_CLICKABLE);
    lv_label_set_text(emptyStateLabel, "");
    lv_obj_add_flag(emptyStateLabel, LV_OBJ_FLAG_HIDDEN);

    layoutButtons();
    refreshButtons(true);

}

void MacroPadScreen::destroy() {
    if (screen) {
        lv_obj_del(screen);
        screen = nullptr;
    }

    for (int i = 0; i < MACROS_BUTTONS_PER_SCREEN; i++) {
        buttons[i] = nullptr;
        labels[i] = nullptr;
        icons[i] = nullptr;
    }

    emptyStateLabel = nullptr;

    lastUpdateMs = 0;
}

void MacroPadScreen::show() {
    if (!screen) {
        create();
    }
    if (screen) {
        // If the template was changed in the web UI while we're running,
        // re-apply layout before showing this screen.
        layoutButtons();
        refreshButtons(true);
        lv_scr_load(screen);
    }
}

void MacroPadScreen::hide() {
    // Nothing to do.
}

void MacroPadScreen::layoutButtons() {
    if (!screen || !displayMgr) return;

    const MacroConfig* cfg = getMacroConfig();
    const char* tpl = (cfg && cfg->template_id[screenIndex][0]) ? cfg->template_id[screenIndex] : macro_templates::default_id();
    if (!macro_templates::is_valid(tpl)) {
        tpl = macro_templates::default_id();
    }

    // Cache the applied template so update() can detect changes.
    strlcpy(lastTemplateId, tpl, sizeof(lastTemplateId));

    if (strcmp(tpl, macro_templates::kTemplateStackSides5) == 0) {
        layoutButtonsFiveStack();
    } else if (strcmp(tpl, macro_templates::kTemplateWideSides3) == 0) {
        layoutButtonsWideCenter();
    } else if (strcmp(tpl, macro_templates::kTemplateSplitSides4) == 0) {
        layoutButtonsFourSplit();
    } else {
        layoutButtonsRound9();
    }
}

bool MacroPadScreen::isSlotUsedByTemplate(uint8_t slot) const {
    const MacroConfig* cfg = getMacroConfig();
    const char* tpl = (cfg && cfg->template_id[screenIndex][0]) ? cfg->template_id[screenIndex] : macro_templates::default_id();
    if (!macro_templates::is_valid(tpl)) {
        tpl = macro_templates::default_id();
    }

    if (strcmp(tpl, macro_templates::kTemplateStackSides5) == 0) {
        return slot < 5;
    }

    if (strcmp(tpl, macro_templates::kTemplateWideSides3) == 0) {
        return slot < 3;
    }

    if (strcmp(tpl, macro_templates::kTemplateSplitSides4) == 0) {
        return slot == 0 || slot == 2 || slot == 3 || slot == 4;
    }

    // round9
    return slot < 9;
}

void MacroPadScreen::layoutButtonsWideCenter() {
    if (!screen || !displayMgr) return;

    const int w = displayMgr->getActiveWidth();
    const int h = displayMgr->getActiveHeight();

    // Match five_stack margins/spacing logic.
    const int padX = (w + h) / 2 / 24; // ~15px at 360
    const int spacing = (padX >= 9) ? (padX / 3) : 3; // ~5px at 360

    // Touch targets. Side buttons must touch the screen edges.
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

    // Used slots: 0=center, 1=right, 2=left. Hide everything else.
    for (int i = 0; i < 3; i++) {
        if (!buttons[i]) continue;
        lv_obj_set_style_radius(buttons[i], 10, 0);
        lv_obj_set_style_border_width(buttons[i], 0, 0);
        lv_obj_clear_flag(buttons[i], LV_OBJ_FLAG_HIDDEN);
    }
    for (int i = 3; i < MACROS_BUTTONS_PER_SCREEN; i++) {
        if (buttons[i]) lv_obj_add_flag(buttons[i], LV_OBJ_FLAG_HIDDEN);
    }

    // Center (slot 0)
    if (buttons[0]) {
        lv_obj_set_pos(buttons[0], xCenter, yTop);
        lv_obj_set_size(buttons[0], centerW, fullH);
        if (labels[0]) {
            lv_obj_set_width(labels[0], (centerW > 12) ? (centerW - 12) : centerW);
            lv_obj_center(labels[0]);
        }
    }

    // Right (slot 1)
    if (buttons[1]) {
        lv_obj_set_pos(buttons[1], xRight, yTop);
        lv_obj_set_size(buttons[1], sideW, fullH);
        if (labels[1]) {
            lv_obj_set_width(labels[1], (sideW > 8) ? (sideW - 8) : sideW);
            lv_obj_center(labels[1]);
        }
    }

    // Left (slot 2)
    if (buttons[2]) {
        lv_obj_set_pos(buttons[2], xLeft, yTop);
        lv_obj_set_size(buttons[2], sideW, fullH);
        if (labels[2]) {
            lv_obj_set_width(labels[2], (sideW > 8) ? (sideW - 8) : sideW);
            lv_obj_center(labels[2]);
        }
    }
}

void MacroPadScreen::updateButtonLayout(uint8_t index, bool hasIcon, bool hasLabel) {
    if (index >= MACROS_BUTTONS_PER_SCREEN) return;
    lv_obj_t* btn = buttons[index];
    lv_obj_t* lbl = labels[index];
    lv_obj_t* icon = icons[index];
    if (!btn || !lbl) return;

    // Default: centered label.
    if (!hasIcon || !icon) {
        lv_obj_center(lbl);
        return;
    }

    // With icon: place label directly under the icon so they read as a unit.
    // Use small padding so it looks good across different templates.
    const int w = (int)lv_obj_get_width(btn);
    const int h = (int)lv_obj_get_height(btn);

    const int pad = clampInt((w + h) / 2 / 20, 4, 10);

    // Icon target box. Master icon size is 64px.
    // For tall/narrow buttons, basing on minDim makes icons too small; prefer width.
    const bool tallNarrow = (h > (w * 2));
    const int baseDim = tallNarrow ? w : ((w < h) ? w : h);

    int iconBox = 0;
    if (!hasLabel) {
        // Icon-only: make it prominent.
        iconBox = (int)lroundf((float)baseDim * 0.75f);
        iconBox = clampInt(iconBox, 32, 128);
    } else {
        // Icon + label: still keep it reasonably large.
        iconBox = (int)lroundf((float)baseDim * 0.85f);
        iconBox = clampInt(iconBox, 28, 128);
    }

    // Special-case very tall/narrow buttons (like the side rails in wide_sides_3):
    // when the button width is 64px, scaling a 64px source down can make thin
    // icons nearly disappear. Prefer 1:1 scale *when it fits*.
    if (tallNarrow && w >= 64) {
        iconBox = 64;
    }

    // Scale icon via zoom so a single 64x64 source works on different button sizes.
    // LVGL zoom: 256 = 1.0x
    #if HAS_DISPLAY && HAS_ICONS
    bool canTransform = true;
    const void* src = lv_img_get_src(icon);
    if (src && lv_img_src_get_type(src) == LV_IMG_SRC_VARIABLE) {
        const lv_img_dsc_t* dsc = (const lv_img_dsc_t*)src;
        switch (dsc->header.cf) {
            case LV_IMG_CF_ALPHA_1BIT:
            case LV_IMG_CF_ALPHA_2BIT:
            case LV_IMG_CF_ALPHA_4BIT:
            case LV_IMG_CF_ALPHA_8BIT:
                // LVGL v8 image widget transformations (zoom/rotate) require true-color images.
                // Alpha-only images can't be transformed, and will render blank when zoom != 256.
                canTransform = false;
                break;
            default:
                break;
        }
    }

    // Fixed-step enlargement rule:
    // - Allow arbitrary DOWNscaling (<1x) to make icons fit.
    // - For enlargement (>1x), snap to exactly 2x (512) and only when the layout has
    //   already decided iconBox can hit its max (128).
    const bool wants2x = (iconBox >= 128);

    if (canTransform) {
        uint16_t zoom = (uint16_t)clampInt((int)lroundf(256.0f * ((float)iconBox / 64.0f)), 64, 256);
        if (wants2x) zoom = 512;
        lv_img_set_zoom(icon, zoom);
        // Ensure the scaled image has enough object area to render without clipping.
        if (wants2x) {
            lv_obj_set_size(icon, 128, 128);
        } else {
            lv_obj_set_size(icon, iconBox, iconBox);
        }
    } else {
        // Alpha-only masks can't be zoomed by lv_img_set_zoom.
        // Instead, when we want a 2x icon, generate a cached 2x alpha mask in RAM.
        if (src && lv_img_src_get_type(src) == LV_IMG_SRC_VARIABLE) {
            const lv_img_dsc_t* dsc = (const lv_img_dsc_t*)src;
            if (wants2x) {
                // If currently on the original 64x64 mask, swap to 128x128 cached mask.
                if (dsc->header.cf == LV_IMG_CF_ALPHA_8BIT && dsc->header.w <= 64 && dsc->header.h <= 64) {
                    const lv_img_dsc_t* dsc2x = getOrCreateMask2x(dsc);
                    if (dsc2x) {
                        lv_img_set_src(icon, dsc2x);
                    }
                }
            } else {
                // If currently using a cached 2x mask, revert to the original 64x64 source.
                if (const lv_img_dsc_t* orig = findOriginalFromMaybeMask2x(dsc)) {
                    lv_img_set_src(icon, orig);
                }
            }
        }

        lv_img_set_zoom(icon, 256);
        lv_obj_set_size(icon, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    }
    #endif

    if (!hasLabel) {
        // Icon-only: center.
        lv_obj_center(icon);
        lv_label_set_text(lbl, "");
        lv_obj_center(lbl);
        return;
    }

    // Center the icon+label group.
    const int gap = clampInt(pad / 2, 2, 8);
    lv_obj_update_layout(lbl);
    const int lblH = (int)lv_obj_get_height(lbl);
    const int iconYOffset = -((lblH / 2) + (gap / 2));

    lv_obj_align(icon, LV_ALIGN_CENTER, 0, iconYOffset);
    lv_obj_align_to(lbl, icon, LV_ALIGN_OUT_BOTTOM_MID, 0, gap);
}

void MacroPadScreen::layoutButtonsFourSplit() {
    if (!screen || !displayMgr) return;

    const int w = displayMgr->getActiveWidth();
    const int h = displayMgr->getActiveHeight();

    // Match five_stack margins/spacing logic.
    const int padX = (w + h) / 2 / 24; // ~15px at 360
    const int spacing = (padX >= 9) ? (padX / 3) : 3; // ~5px at 360

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

    // Used slots: 0 (center-top), 2 (center-bottom), 3 (left), 4 (right).
    // Hide everything else.
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

    // Center is split horizontally into top/bottom.
    int topH = (fullH - spacing) / 2;
    int bottomH = fullH - topH - spacing;
    if (topH < minTouchPx || bottomH < minTouchPx) {
        // Clamp if needed.
        topH = (topH < minTouchPx) ? minTouchPx : topH;
        bottomH = fullH - topH - spacing;
        if (bottomH < minTouchPx) {
            bottomH = minTouchPx;
            topH = fullH - bottomH - spacing;
            if (topH < minTouchPx) topH = minTouchPx;
        }
    }

    // Slot 0 = center top.
    if (buttons[0]) {
        lv_obj_set_pos(buttons[0], xCenter, yTop);
        lv_obj_set_size(buttons[0], centerW, topH);
        if (labels[0]) {
            lv_obj_set_width(labels[0], (centerW > 12) ? (centerW - 12) : centerW);
            lv_obj_center(labels[0]);
        }
    }

    // Slot 2 = center bottom.
    if (buttons[2]) {
        lv_obj_set_pos(buttons[2], xCenter, yTop + topH + spacing);
        lv_obj_set_size(buttons[2], centerW, bottomH);
        if (labels[2]) {
            lv_obj_set_width(labels[2], (centerW > 12) ? (centerW - 12) : centerW);
            lv_obj_center(labels[2]);
        }
    }

    // Side buttons full-height, touching outer edges.
    if (buttons[3]) {
        lv_obj_set_pos(buttons[3], xLeft, yTop);
        lv_obj_set_size(buttons[3], sideW, fullH);
        if (labels[3]) {
            lv_obj_set_width(labels[3], (sideW > 8) ? (sideW - 8) : sideW);
            lv_obj_center(labels[3]);
        }
    }
    if (buttons[4]) {
        lv_obj_set_pos(buttons[4], xRight, yTop);
        lv_obj_set_size(buttons[4], sideW, fullH);
        if (labels[4]) {
            lv_obj_set_width(labels[4], (sideW > 8) ? (sideW - 8) : sideW);
            lv_obj_center(labels[4]);
        }
    }
}

void MacroPadScreen::layoutButtonsRound9() {
    if (!screen || !displayMgr) return;

    const int w = displayMgr->getActiveWidth();
    const int h = displayMgr->getActiveHeight();
    const int cx = w / 2;
    const int cy = h / 2;

    // Button sizing tuned for 360x360 round.
    // Goal: use the full circle (touch the outer edge) and make outer buttons
    // barely separated from each other.
    const int minDim = (w < h) ? w : h;
    const float half = (float)minDim * 0.5f;

    // Adjacent outer buttons are 45° apart.
    // Pick the largest radius that still leaves a small gap between them.
    const int desiredGapPx = 1;
    const float s = sinf((float)M_PI / 8.0f); // sin(22.5°)
    float r = (s * (float)minDim - (float)desiredGapPx) / (2.0f * (1.0f + s));
    if (r < 18.0f) r = 18.0f;

    int btnSize = (int)floorf(2.0f * r);
    if (btnSize < 36) btnSize = 36;
    if (btnSize > minDim) btnSize = minDim;
    const int btnRadius = btnSize / 2;

    // Outer ring radius: touch the outer edge of the active area.
    const float outerRadius = half - (float)btnRadius;

    // Label width scales with button size.
    const int labelWidth = (btnSize > 24) ? (btnSize - 18) : btnSize;

    // Round discs.
    for (int i = 0; i < 9; i++) {
        if (!buttons[i]) continue;
        lv_obj_set_style_radius(buttons[i], LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_border_width(buttons[i], 0, 0);
        lv_obj_clear_flag(buttons[i], LV_OBJ_FLAG_HIDDEN);
    }
    // Hide unused.
    for (int i = 9; i < MACROS_BUTTONS_PER_SCREEN; i++) {
        if (buttons[i]) lv_obj_add_flag(buttons[i], LV_OBJ_FLAG_HIDDEN);
    }

    // Center button (#9) is index 8.
    if (buttons[8]) {
        lv_obj_set_size(buttons[8], btnSize, btnSize);
        lv_obj_set_pos(buttons[8], cx - btnRadius, cy - btnRadius);
        if (labels[8]) {
            lv_obj_set_width(labels[8], labelWidth);
            lv_obj_center(labels[8]);
        }
    }

    // B1..B8 are indices 0..7.
    for (int i = 0; i < 8; i++) {
        if (!buttons[i]) continue;

        lv_obj_set_size(buttons[i], btnSize, btnSize);

        const float deg = -90.0f + (float)i * 45.0f; // B1 at top, clockwise
        const float rad = deg * (float)M_PI / 180.0f;

        const int bx = (int)lroundf((float)cx + outerRadius * cosf(rad));
        const int by = (int)lroundf((float)cy + outerRadius * sinf(rad));

        lv_obj_set_pos(buttons[i], bx - btnRadius, by - btnRadius);

        if (labels[i]) {
            lv_obj_set_width(labels[i], labelWidth);
            lv_obj_center(labels[i]);
        }
    }
}

void MacroPadScreen::layoutButtonsFiveStack() {
    if (!screen || !displayMgr) return;

    const int w = displayMgr->getActiveWidth();
    const int h = displayMgr->getActiveHeight();

    // Layout intentionally uses the full rectangular screen bounds (0..w, 0..h).
    // It's OK if elements fall outside the visible round area.
    const int padX = (w + h) / 2 / 24; // ~15px at 360
    // Use one spacing value for both axes so vertical gaps match horizontal gaps.
    const int spacing = (padX >= 9) ? (padX / 3) : 3; // ~5px at 360

    // Touch targets. Side buttons must touch the screen edges.
    const int minTouchPx = 52;

    // Side width: try to keep them smaller than center, but wide enough to tap,
    // and wide enough to reach the screen edges.
    const int minCenterW = minTouchPx * 2;
    int sideW = (int)((float)w * 0.18f);
    if (sideW < minTouchPx) sideW = minTouchPx;
    const int maxSideW = (w - minCenterW - (2 * spacing)) / 2;
    if (sideW > maxSideW) sideW = maxSideW;

    // Center stack is separated from the side buttons by `spacing`.
    const int xLeft = 0;
    const int xCenter = sideW + spacing;
    int centerW = w - (2 * sideW) - (2 * spacing);
    if (centerW < minCenterW) centerW = minCenterW;
    const int xRight = w - sideW;
    const int yTop = 0;
    const int usableH = h;

    // Stack fills full height.
    // Buttons 1 and 3 (slots 0 and 2) touch the top/bottom screen edges.
    // Button 2 (slot 1) is taller and uses the remaining space.
    // Make top/bottom taller and the middle less tall.
    int topH = (int)((float)usableH * 0.30f);
    int bottomH = topH;
    if (topH < minTouchPx) topH = minTouchPx;
    if (bottomH < minTouchPx) bottomH = minTouchPx;
    int middleH = usableH - topH - bottomH - (2 * spacing);
    if (middleH < minTouchPx) {
        // Steal from top/bottom evenly if the screen is too small.
        const int need = minTouchPx - middleH;
        const int stealEach = (need + 1) / 2;
        topH = (topH - stealEach > minTouchPx) ? (topH - stealEach) : minTouchPx;
        bottomH = (bottomH - stealEach > minTouchPx) ? (bottomH - stealEach) : minTouchPx;
        middleH = usableH - topH - bottomH - (2 * spacing);
        if (middleH < minTouchPx) middleH = minTouchPx;
    }

    // Show slots 0..4, hide the rest.
    for (int i = 0; i < 5; i++) {
        if (!buttons[i]) continue;
        lv_obj_set_style_radius(buttons[i], 10, 0);
        lv_obj_set_style_border_width(buttons[i], 0, 0);
        lv_obj_clear_flag(buttons[i], LV_OBJ_FLAG_HIDDEN);
    }
    for (int i = 5; i < MACROS_BUTTONS_PER_SCREEN; i++) {
        if (buttons[i]) lv_obj_add_flag(buttons[i], LV_OBJ_FLAG_HIDDEN);
    }

    // Slots 0,1,2 = stacked center.
    if (buttons[0]) {
        lv_obj_set_pos(buttons[0], xCenter, yTop);
        lv_obj_set_size(buttons[0], centerW, topH);
        if (labels[0]) {
            lv_obj_set_width(labels[0], (centerW > 12) ? (centerW - 12) : centerW);
            lv_obj_center(labels[0]);
        }
    }
    if (buttons[1]) {
        lv_obj_set_pos(buttons[1], xCenter, yTop + topH + spacing);
        lv_obj_set_size(buttons[1], centerW, middleH);
        if (labels[1]) {
            lv_obj_set_width(labels[1], (centerW > 12) ? (centerW - 12) : centerW);
            lv_obj_center(labels[1]);
        }
    }
    if (buttons[2]) {
        lv_obj_set_pos(buttons[2], xCenter, yTop + usableH - bottomH);
        lv_obj_set_size(buttons[2], centerW, bottomH);
        if (labels[2]) {
            lv_obj_set_width(labels[2], (centerW > 12) ? (centerW - 12) : centerW);
            lv_obj_center(labels[2]);
        }
    }

    // Slot 3 = left rail, Slot 4 = right rail.
    // Align with the top of button 1 and bottom of button 3.
    const int ySide = yTop;
    const int sideH = usableH;
    if (buttons[3]) {
        lv_obj_set_pos(buttons[3], xLeft, ySide);
        lv_obj_set_size(buttons[3], sideW, sideH);
        if (labels[3]) {
            lv_obj_set_width(labels[3], (sideW > 8) ? (sideW - 8) : sideW);
            lv_obj_center(labels[3]);
        }
    }
    if (buttons[4]) {
        lv_obj_set_pos(buttons[4], xRight, ySide);
        lv_obj_set_size(buttons[4], sideW, sideH);
        if (labels[4]) {
            lv_obj_set_width(labels[4], (sideW > 8) ? (sideW - 8) : sideW);
            lv_obj_center(labels[4]);
        }
    }
}

void MacroPadScreen::refreshButtons(bool force) {
    if (!screen) return;

    const uint32_t now = millis();
    if (!force && lastUpdateMs != 0 && (uint32_t)(now - lastUpdateMs) < kUiRefreshIntervalMs) {
        return;
    }
    lastUpdateMs = now;

    const MacroConfig* cfg = getMacroConfig();
    if (!cfg) return;

    // Apply macro screen background (optional per-screen override, else global default).
    const uint32_t screenBg = (cfg->screen_bg[screenIndex] != MACROS_COLOR_UNSET)
        ? cfg->screen_bg[screenIndex]
        : cfg->default_screen_bg;
    lv_obj_set_style_bg_color(screen, lv_color_hex(screenBg), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);

    bool anyButtonConfigured = false;

    for (int i = 0; i < MACROS_BUTTONS_PER_SCREEN; i++) {
        const MacroButtonConfig* btnCfg = &cfg->buttons[screenIndex][i];

        if (!isSlotUsedByTemplate((uint8_t)i)) {
            setButtonVisible(buttons[i], false);
            continue;
        }

        if (btnCfg->action != MacroButtonAction::None) {
            anyButtonConfigured = true;
        }

        const bool visible = btnCfg->action != MacroButtonAction::None;
        setButtonVisible(buttons[i], visible);

        // For navigation buttons, provide sensible default icons if none configured.
        // This avoids “blank” side buttons on templates like round_wide_sides_3.
        const char* effectiveIconId = btnCfg->icon_id;
        if (effectiveIconId[0] == '\0') {
            if (btnCfg->action == MacroButtonAction::NavPrevScreen) effectiveIconId = "chevron_left";
            else if (btnCfg->action == MacroButtonAction::NavNextScreen) effectiveIconId = "chevron_right";
        }

        if (!visible) continue;

        // Per-button appearance (optional overrides, else global defaults).
        const uint32_t buttonBg = (btnCfg->button_bg != MACROS_COLOR_UNSET)
            ? btnCfg->button_bg
            : cfg->default_button_bg;
        const uint32_t labelColor = (btnCfg->label_color != MACROS_COLOR_UNSET)
            ? btnCfg->label_color
            : cfg->default_label_color;
        const uint32_t iconColor = (btnCfg->icon_color != MACROS_COLOR_UNSET)
            ? btnCfg->icon_color
            : cfg->default_icon_color;

        if (buttons[i]) {
            lv_obj_set_style_bg_color(buttons[i], lv_color_hex(buttonBg), 0);
            lv_obj_set_style_bg_opa(buttons[i], LV_OPA_COVER, 0);
        }
        if (labels[i]) {
            lv_obj_set_style_text_color(labels[i], lv_color_hex(labelColor), 0);
        }

        // If an icon is set and the user intentionally left the label blank,
        // respect that (icon-only button) instead of forcing a default label.
        const bool userLabelEmpty = (btnCfg->label[0] == '\0');
        const bool wantsIcon = (effectiveIconId && effectiveIconId[0] != '\0');

        // If this is a navigation button and the label is a simple arrow, prefer icon-only
        // ONLY when the user explicitly configured an icon.
        // If the icon comes from a fallback (icon_id empty), keep the label so the button
        // doesn't become blank on targets where icons are hard to see.
        const bool labelIsSimpleArrow = (btnCfg->icon_id[0] != '\0') && (
            (btnCfg->label[0] == '<' && btnCfg->label[1] == '\0')
            || (btnCfg->label[0] == '>' && btnCfg->label[1] == '\0'));

        char labelBuf[32];
        const char* labelText = btnCfg->label;
        if ((userLabelEmpty || labelIsSimpleArrow) && wantsIcon) {
            labelText = "";
        } else if (!labelText || !*labelText) {
            defaultLabel(labelBuf, sizeof(labelBuf), screenIndex, (uint8_t)i);
            labelText = labelBuf;
        }

        // If action isn't SendKeys, show a small hint so blank labels are still meaningful.
        // For icon-only buttons, do not inject a hint.
        if (!wantsIcon && (btnCfg->label == nullptr || btnCfg->label[0] == '\0') && btnCfg->action != MacroButtonAction::SendKeys) {
            char combined[40];
            snprintf(combined, sizeof(combined), "%s\n(%s)", labelText, actionToShortLabel(btnCfg->action));
            lv_label_set_text(labels[i], combined);
        } else {
            lv_label_set_text(labels[i], labelText);
        }

        // Icon rendering (optional).
        bool hasIcon = false;
        #if HAS_DISPLAY && HAS_ICONS
        if (icons[i]) {
            IconRef ref;
            char normalizedId[MACROS_ICON_ID_MAX_LEN];
            const char* lookupId = effectiveIconId;
            if (lookupId && lookupId[0] != '\0' && normalizeIconId(lookupId, normalizedId, sizeof(normalizedId))) {
                lookupId = normalizedId;
            }

            if (lookupId[0] != '\0' && icon_registry_lookup(lookupId, &ref) && ref.dsc) {
                lv_img_set_src(icons[i], ref.dsc);
                lv_obj_set_style_opa(icons[i], LV_OPA_COVER, 0);
                if (ref.kind == IconKind::Mask) {
                    // Monochrome: recolor/tint via style.
                    lv_obj_set_style_img_recolor(icons[i], lv_color_hex(iconColor), 0);
                    lv_obj_set_style_img_recolor_opa(icons[i], LV_OPA_COVER, 0);
                    lv_obj_set_style_img_opa(icons[i], LV_OPA_COVER, 0);
                } else {
                    // Color: do not recolor.
                    lv_obj_set_style_img_recolor_opa(icons[i], LV_OPA_TRANSP, 0);
                    lv_obj_set_style_img_opa(icons[i], LV_OPA_COVER, 0);
                }

                lv_obj_clear_flag(icons[i], LV_OBJ_FLAG_HIDDEN);
                // Ensure the icon isn't occluded by the label in narrow layouts.
                lv_obj_move_foreground(icons[i]);
                hasIcon = true;
            } else {
                lv_obj_add_flag(icons[i], LV_OBJ_FLAG_HIDDEN);
                hasIcon = false;
            }
        }
        #endif

        const bool hasLabel = (labelText && *labelText);
        updateButtonLayout((uint8_t)i, hasIcon, hasLabel);
    }

    updateEmptyState(anyButtonConfigured);
}

void MacroPadScreen::updateEmptyState(bool anyButtonConfigured) {
    // Show on any Macro Screen when it's empty.
    if (!emptyStateLabel) return;

    if (anyButtonConfigured) {
        lv_obj_add_flag(emptyStateLabel, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    // Determine which IP to show (STA or AP mode).
    String ipStr;
    if (WiFi.status() == WL_CONNECTED) {
        ipStr = WiFi.localIP().toString();
    } else if (WiFi.getMode() == WIFI_AP || WiFi.getMode() == WIFI_AP_STA) {
        ipStr = WiFi.softAPIP().toString();
    } else {
        ipStr = "0.0.0.0";
    }

    // Determine mDNS hostname. Prefer sanitized device_name; fall back to WiFi hostname if present.
    char mdnsHost[CONFIG_DEVICE_NAME_MAX_LEN] = {0};
    if (displayMgr && displayMgr->getConfig()) {
        config_manager_sanitize_device_name(displayMgr->getConfig()->device_name, mdnsHost, sizeof(mdnsHost));
    }
    const char* wifiHost = WiFi.getHostname();
    if (mdnsHost[0] == '\0' && wifiHost && wifiHost[0] != '\0') {
        strlcpy(mdnsHost, wifiHost, sizeof(mdnsHost));
    }

    const unsigned screenNumber = (unsigned)screenIndex + 1;

    char text[256];
    if (mdnsHost[0] != '\0') {
        snprintf(
            text,
            sizeof(text),
            "No macros configured.\n\nOpen the config portal:\nhttp://%s\nhttp://%s.local\n\nConfigure Macro Screen %u.",
            ipStr.c_str(),
            mdnsHost,
            screenNumber);
    } else {
        snprintf(
            text,
            sizeof(text),
            "No macros configured.\n\nOpen the config portal:\nhttp://%s\n\nConfigure Macro Screen %u.",
            ipStr.c_str(),
            screenNumber);
    }

    lv_label_set_text(emptyStateLabel, text);
    lv_obj_clear_flag(emptyStateLabel, LV_OBJ_FLAG_HIDDEN);
}

void MacroPadScreen::update() {
    const MacroConfig* cfg = getMacroConfig();
    if (cfg) {
        const char* tpl = (cfg->template_id[screenIndex][0] != '\0') ? cfg->template_id[screenIndex] : macro_templates::default_id();
        if (!macro_templates::is_valid(tpl)) {
            tpl = macro_templates::default_id();
        }

        if (strcmp(tpl, lastTemplateId) != 0) {
            // Template changed: re-layout immediately.
            layoutButtons();
            refreshButtons(true);
            return;
        }
    }

    refreshButtons(false);
}

void MacroPadScreen::buttonEventCallback(lv_event_t* e) {
    ButtonCtx* ctx = (ButtonCtx*)lv_event_get_user_data(e);
    if (!ctx || !ctx->self) return;

    MacroPadScreen* self = ctx->self;
    const uint8_t b = ctx->buttonIndex;

    #if HAS_DISPLAY
    screen_saver_manager_notify_activity(true);
    #endif

    const MacroConfig* cfg = self->getMacroConfig();
    if (!cfg) return;

    const MacroButtonConfig* btnCfg = &cfg->buttons[self->screenIndex][b];
    if (!btnCfg) return;

    if (btnCfg->action == MacroButtonAction::None) return;

    if (btnCfg->action == MacroButtonAction::NavNextScreen || btnCfg->action == MacroButtonAction::NavPrevScreen) {
        const uint8_t next = (btnCfg->action == MacroButtonAction::NavNextScreen)
            ? (uint8_t)((self->screenIndex + 1) % MACROS_SCREEN_COUNT)
            : (uint8_t)((self->screenIndex + MACROS_SCREEN_COUNT - 1) % MACROS_SCREEN_COUNT);

        char id[16];
        snprintf(id, sizeof(id), "macro%u", (unsigned)(next + 1));
        self->displayMgr->showScreen(id);
        return;
    }

    if (btnCfg->action == MacroButtonAction::SendKeys) {
        if (btnCfg->script[0] == '\0') {
            Logger.logMessage("Macro", "Empty script; skipping");
            return;
        }

        BleKeyboardManager* kb = self->getBleKeyboard();
        ducky_execute(btnCfg->script, kb);
        return;
    }
}
