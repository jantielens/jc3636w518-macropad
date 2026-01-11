#include "macropad_screen.h"

#include "macropad_layout.h"

#include "../display_manager.h"
#include "../macros_config.h"
#include "../macro_templates.h"
#include "../ble_keyboard_manager.h"
#include "../ducky_script.h"
#include "../log_manager.h"
#include "../config_manager.h"

#if HAS_MQTT
#include "../mqtt_manager.h"
#endif

#if HAS_DISPLAY
#include "../png_assets.h"
#endif

#if HAS_DISPLAY && HAS_ICONS
#include "../icon_registry.h"
#include "../icon_store.h"
#endif

#if HAS_DISPLAY
#include "../screen_saver_manager.h"
#endif

#include <esp_system.h>
#include <string.h>

#include <WiFi.h>

namespace {

constexpr uint32_t kUiRefreshIntervalMs = 500;
constexpr lv_state_t kPressCueState = LV_STATE_USER_1;

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
        case MacroButtonAction::NavToScreen: return "Go";
        case MacroButtonAction::GoBack: return "Back";
        case MacroButtonAction::MqttSend: return "MQTT";
        default: return "—";
    }
}

static void defaultLabel(char* out, size_t outLen, uint8_t screenIndex, uint8_t buttonIndex) {
    // screenIndex/buttonIndex are 0-based
    snprintf(out, outLen, "S%u-B%u", (unsigned)(screenIndex + 1), (unsigned)(buttonIndex + 1));
}

static inline uint8_t rgb565_luma(lv_color_t c) {
    // LV_COLOR_DEPTH is 16 (RGB565) in this project.
    const uint16_t v = c.full;
    const uint8_t r5 = (v >> 11) & 0x1F;
    const uint8_t g6 = (v >> 5) & 0x3F;
    const uint8_t b5 = (v >> 0) & 0x1F;

    const uint8_t r8 = (uint8_t)((r5 * 255u) / 31u);
    const uint8_t g8 = (uint8_t)((g6 * 255u) / 63u);
    const uint8_t b8 = (uint8_t)((b5 * 255u) / 31u);

    // ITU-R BT.601-ish luma approximation.
    return (uint8_t)((uint16_t)(r8 * 77u + g8 * 150u + b8 * 29u) >> 8);
}

static inline lv_color_t flashColorFor(lv_color_t base) {
    // Luminance-aware: brighten dark colors, darken bright colors.
    const uint8_t luma = rgb565_luma(base);
    const lv_color_t target = (luma >= 160) ? lv_color_black() : lv_color_white();
    // lv_color_mix(c1, c2, mix): mix=0 => c2, mix=255 => c1
    return lv_color_mix(target, base, 80);
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

    // Pick an empty slot. Do NOT evict/reuse a slot that might still be referenced
    // by an LVGL image object (it would invalidate the descriptor and can crash).
    // If the cache is full, just skip 2x generation (caller will keep the 1x mask).
    size_t slot = (size_t)-1;
    for (size_t i = 0; i < (sizeof(s_mask2xCache) / sizeof(s_mask2xCache[0])); i++) {
        if (!s_mask2xCache[i].data128) {
            slot = i;
            break;
        }
    }
    if (slot == (size_t)-1) {
        return nullptr;
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
    : displayMgr(manager), screenIndex(idx), screen(nullptr), pressedPieSlot(-1), pressHoldTimer(nullptr), lastUpdateMs(0) {
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

    pieHitLayer = nullptr;
    for (int i = 0; i < 8; i++) {
        pieSegments[i] = nullptr;
    }

    emptyStateLabel = nullptr;

    pressedPieSlot = -1;

    pressHoldTimer = nullptr;
    for (int i = 0; i < MACROS_BUTTONS_PER_SCREEN; i++) {
        pressDownTick[i] = 0;
        pendingClearTick[i] = 0;
    }

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

const char* MacroPadScreen::resolveTemplateId(const MacroConfig* cfg) const {
    const char* tpl = (cfg && cfg->template_id[screenIndex][0]) ? cfg->template_id[screenIndex] : macro_templates::default_id();
    if (!macro_templates::is_valid(tpl)) {
        tpl = macro_templates::default_id();
    }
    return tpl;
}

void MacroPadScreen::ensurePressStylesInited() {
    if (pressStyles.inited) return;

    static const lv_style_prop_t btnProps[] = {
        (lv_style_prop_t)LV_STYLE_BG_COLOR,
        (lv_style_prop_t)LV_STYLE_BG_OPA,
        (lv_style_prop_t)LV_STYLE_PROP_INV,
    };
    lv_style_transition_dsc_init(&pressStyles.btnTrans, btnProps, lv_anim_path_ease_out, 160, 0, nullptr);

    for (int i = 0; i < MACROS_BUTTONS_PER_SCREEN; i++) {
        lv_style_init(&pressStyles.btnBase[i]);
        lv_style_set_bg_opa(&pressStyles.btnBase[i], LV_OPA_COVER);
        lv_style_set_bg_color(&pressStyles.btnBase[i], lv_color_make(30, 30, 30));
        lv_style_set_transition(&pressStyles.btnBase[i], &pressStyles.btnTrans);

        lv_style_init(&pressStyles.btnPressed[i]);
        lv_style_set_bg_opa(&pressStyles.btnPressed[i], LV_OPA_COVER);
        lv_style_set_bg_color(&pressStyles.btnPressed[i], flashColorFor(lv_color_make(30, 30, 30)));
    }

    for (int i = 0; i < 8; i++) {
        lv_style_init(&pressStyles.segBase[i]);
        lv_style_set_arc_opa(&pressStyles.segBase[i], LV_OPA_COVER);
        lv_style_set_arc_color(&pressStyles.segBase[i], lv_color_make(30, 30, 30));
        // NOTE: We intentionally do NOT animate arc color/opacity transitions.
        // On some targets the arc (indicator) transition can produce visible color
        // artifacts during redraw (reported as “yellow flicker” on red wedges).

        lv_style_init(&pressStyles.segPressed[i]);
        lv_style_set_arc_opa(&pressStyles.segPressed[i], LV_OPA_COVER);
        lv_style_set_arc_color(&pressStyles.segPressed[i], flashColorFor(lv_color_make(30, 30, 30)));
    }

    pressStyles.inited = true;
}

void MacroPadScreen::buildLayoutContext(macropad_layout::MacroPadLayoutContext& out) {
    out = {
        .displayMgr = displayMgr,
        .screen = screen,
        .buttons = buttons,
        .labels = labels,
        .icons = icons,
        .pieHitLayer = pieHitLayer,
        .pieSegments = pieSegments,
    };
}

void MacroPadScreen::create() {
    if (screen) return;

    ensurePressStylesInited();

    screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(screen, lv_color_black(), 0);
    // The screen root is just a container for our custom hit targets.
    // Disable scrolling to avoid LVGL showing scrollbars.
    lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(screen, LV_SCROLLBAR_MODE_OFF);

    // Create clickable objects with centered labels.
    // (Avoid lv_btn_create so this works even when LV_USE_BTN=0.)
    for (int i = 0; i < MACROS_BUTTONS_PER_SCREEN; i++) {
        lv_obj_t* btn = lv_obj_create(screen);
        // These are static touch targets, not scrollable containers.
        lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_style_radius(btn, 10, 0);

        // Base + pressed styles (pressed uses a luminance-aware flash fill).
        lv_obj_add_style(btn, &pressStyles.btnBase[i], LV_PART_MAIN);
        lv_obj_add_style(btn, &pressStyles.btnPressed[i], LV_PART_MAIN | kPressCueState);

        // No outline/border by default.
        lv_obj_set_style_border_width(btn, 0, 0);
        lv_obj_set_style_outline_width(btn, 0, 0);
        lv_obj_set_style_outline_pad(btn, 0, 0);
        lv_obj_set_style_shadow_width(btn, 0, 0);
        lv_obj_set_style_pad_all(btn, 0, 0);
        lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);

        // Drive pressed visual feedback explicitly (kPressCueState).
        lv_obj_add_event_cb(btn, buttonEventCallback, LV_EVENT_PRESSED, &buttonCtx[i]);
        lv_obj_add_event_cb(btn, buttonEventCallback, LV_EVENT_RELEASED, &buttonCtx[i]);
        lv_obj_add_event_cb(btn, buttonEventCallback, LV_EVENT_PRESS_LOST, &buttonCtx[i]);
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

    // Pie template: full-screen hit layer (polar hit testing), and 8 ring segments.
    // These are hidden by default and only enabled for the round_pie_8 template.
    pieHitLayer = lv_obj_create(screen);
    lv_obj_clear_flag(pieHitLayer, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(pieHitLayer, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_opa(pieHitLayer, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(pieHitLayer, 0, 0);
    lv_obj_set_style_outline_width(pieHitLayer, 0, 0);
    lv_obj_set_style_pad_all(pieHitLayer, 0, 0);
    lv_obj_add_flag(pieHitLayer, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(pieHitLayer, pieEventCallback, LV_EVENT_PRESSED, this);
    lv_obj_add_event_cb(pieHitLayer, pieEventCallback, LV_EVENT_PRESSING, this);
    lv_obj_add_event_cb(pieHitLayer, pieEventCallback, LV_EVENT_RELEASED, this);
    lv_obj_add_event_cb(pieHitLayer, pieEventCallback, LV_EVENT_PRESS_LOST, this);
    lv_obj_add_event_cb(pieHitLayer, pieEventCallback, LV_EVENT_CLICKED, this);

    for (int i = 0; i < 8; i++) {
        lv_obj_t* seg = lv_arc_create(screen);
        // Arc widget defaults are interactive; make it purely visual.
        lv_obj_clear_flag(seg, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_clear_flag(seg, LV_OBJ_FLAG_CLICKABLE);
        // Hide until pie template is active.
        lv_obj_add_flag(seg, LV_OBJ_FLAG_HIDDEN);

        // Make the arc look like a ring segment (no knob/indicator).
        lv_obj_set_style_bg_opa(seg, LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_set_style_border_width(seg, 0, LV_PART_MAIN);
        lv_obj_set_style_pad_all(seg, 0, LV_PART_MAIN);

        // Hide knob.
        lv_obj_set_style_bg_opa(seg, LV_OPA_TRANSP, LV_PART_KNOB);
        lv_obj_set_style_border_width(seg, 0, LV_PART_KNOB);

        // Default segment color; per-button colors applied in refreshButtons().
        // Base + pressed styles (pressed uses a luminance-aware flash fill).
        lv_obj_add_style(seg, &pressStyles.segBase[i], LV_PART_INDICATOR);
        lv_obj_add_style(seg, &pressStyles.segPressed[i], LV_PART_INDICATOR | kPressCueState);
        lv_obj_set_style_arc_rounded(seg, false, LV_PART_INDICATOR);

        // Disable the background arc.
        lv_obj_set_style_arc_opa(seg, LV_OPA_TRANSP, LV_PART_MAIN);

        pieSegments[i] = seg;
    }

    // Ensure the pie hit layer stays above any non-clickable visuals (arcs) so it
    // consistently owns pointer events while the pie template is active.
    if (pieHitLayer) {
        lv_obj_move_foreground(pieHitLayer);
    }

    // Timer used to enforce a minimum visible time for pressed feedback.
    // (Quick taps otherwise make the cue nearly invisible.)
    if (!pressHoldTimer) {
        pressHoldTimer = lv_timer_create(pressHoldTimerCallback, 10, this);
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

    if (pressHoldTimer) {
        lv_timer_del(pressHoldTimer);
        pressHoldTimer = nullptr;
    }

    for (int i = 0; i < MACROS_BUTTONS_PER_SCREEN; i++) {
        buttons[i] = nullptr;
        labels[i] = nullptr;
        icons[i] = nullptr;
    }

    pieHitLayer = nullptr;
    for (int i = 0; i < 8; i++) {
        pieSegments[i] = nullptr;
    }

    emptyStateLabel = nullptr;

    pressedPieSlot = -1;

    for (int i = 0; i < MACROS_BUTTONS_PER_SCREEN; i++) {
        pressDownTick[i] = 0;
        pendingClearTick[i] = 0;
    }

    lastUpdateMs = 0;
}

void MacroPadScreen::notePressed(uint8_t slotIndex) {
    if (slotIndex >= MACROS_BUTTONS_PER_SCREEN) return;
    pressDownTick[slotIndex] = lv_tick_get();
    pendingClearTick[slotIndex] = 0;
}

void MacroPadScreen::cancelPendingClear(uint8_t slotIndex) {
    if (slotIndex >= MACROS_BUTTONS_PER_SCREEN) return;
    pendingClearTick[slotIndex] = 0;
}

void MacroPadScreen::clearPressedVisual(uint8_t slotIndex) {
    if (slotIndex >= MACROS_BUTTONS_PER_SCREEN) return;

    if (slotIndex < 8) {
        // Slots 0..7 are special in the pie template (ring segments), but they are
        // also normal buttons in non-pie templates. Clear both to be safe.
        if (pieSegments[slotIndex]) lv_obj_clear_state(pieSegments[slotIndex], kPressCueState);
        if (buttons[slotIndex]) lv_obj_clear_state(buttons[slotIndex], kPressCueState);
        if (pressedPieSlot == (int8_t)slotIndex) {
            pressedPieSlot = -1;
        }
        return;
    }

    // Center (8) and grid buttons (9..)
    if (buttons[slotIndex]) {
        lv_obj_clear_state(buttons[slotIndex], kPressCueState);
    }
}

void MacroPadScreen::scheduleReleaseClear(uint8_t slotIndex) {
    if (slotIndex >= MACROS_BUTTONS_PER_SCREEN) return;

    const uint32_t now = lv_tick_get();
    const uint32_t down = pressDownTick[slotIndex];
    const uint32_t elapsed = (down == 0) ? kMinPressCueMs : lv_tick_elaps(down);

    if (elapsed >= kMinPressCueMs) {
        pendingClearTick[slotIndex] = 0;
        clearPressedVisual(slotIndex);
        return;
    }

    const uint32_t remaining = (kMinPressCueMs - elapsed);
    pendingClearTick[slotIndex] = now + remaining;
}

void MacroPadScreen::pressHoldTimerCallback(lv_timer_t* t) {
    MacroPadScreen* self = (MacroPadScreen*)t->user_data;
    if (!self) return;

    const uint32_t now = lv_tick_get();
    bool anyPending = false;

    for (uint8_t i = 0; i < MACROS_BUTTONS_PER_SCREEN; i++) {
        const uint32_t due = self->pendingClearTick[i];
        if (due == 0) continue;
        anyPending = true;
        if ((int32_t)(now - due) >= 0) {
            self->pendingClearTick[i] = 0;
            self->clearPressedVisual(i);
        }
    }

    // Light optimization: if nothing is pending, tick slower.
    lv_timer_set_period(t, anyPending ? 10 : 50);
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
    const char* tpl = resolveTemplateId(cfg);

    // Cache the applied template so update() can detect changes.
    strlcpy(lastTemplateId, tpl, sizeof(lastTemplateId));

    const macropad_layout::IMacroPadLayout& layout = macropad_layout::layoutForId(tpl);
    macropad_layout::MacroPadLayoutContext ctx;
    buildLayoutContext(ctx);
    layout.apply(ctx);
}

void MacroPadScreen::pieEventCallback(lv_event_t* e) {
    MacroPadScreen* self = (MacroPadScreen*)lv_event_get_user_data(e);
    if (!self) return;

    const lv_event_code_t code = lv_event_get_code(e);

    auto clearPressed = [&]() {
        if (self->pressedPieSlot >= 0 && self->pressedPieSlot < 8) {
            const uint8_t s = (uint8_t)self->pressedPieSlot;
            self->cancelPendingClear(s);
            self->clearPressedVisual(s);
        }
        self->pressedPieSlot = -1;

        // The pie hit layer sits above everything (including the center button), so
        // we need to drive the center pressed state manually for consistent feedback.
        if (self->buttons[8]) {
            self->cancelPendingClear(8);
            self->clearPressedVisual(8);
        }
    };

    // Release: clear highlight (but enforce a minimum visible cue time).
    // Note: we intentionally ignore LV_EVENT_PRESS_LOST here because, in pie mode,
    // LVGL can transiently report press-lost while the finger is still down if the
    // pressed object selection changes. That can cause visible flicker.
    if (code == LV_EVENT_RELEASED) {
        // Schedule clear for whichever target is currently pressed.
        if (self->pressedPieSlot >= 0 && self->pressedPieSlot < 8) {
            self->scheduleReleaseClear((uint8_t)self->pressedPieSlot);
        }
        if (self->buttons[8] && lv_obj_has_state(self->buttons[8], kPressCueState)) {
            self->scheduleReleaseClear(8);
        }
        return;
    }

    if (code == LV_EVENT_PRESS_LOST) {
        return;
    }

    lv_indev_t* indev = lv_indev_get_act();
    if (!indev) return;

    lv_point_t p;
    lv_indev_get_point(indev, &p);

    const MacroConfig* cfg = self->getMacroConfig();
    const char* tpl = self->resolveTemplateId(cfg);

    const macropad_layout::IMacroPadLayout& layout = macropad_layout::layoutForId(tpl);
    macropad_layout::MacroPadLayoutContext ctx;
    self->buildLayoutContext(ctx);

    const int slot = layout.slotFromPoint(p.x, p.y, ctx);

    // Press: latch pressed feedback to the initial target.
    // We intentionally do NOT switch the highlighted wedge during LV_EVENT_PRESSING;
    // that eliminates flicker caused by touch jitter (especially near wedge gaps).
    if (code == LV_EVENT_PRESSED) {
        clearPressed();

        if (slot >= 0 && slot < 8) {
            self->pressedPieSlot = (int8_t)slot;
            lv_obj_t* seg = self->pieSegments[slot];
            if (seg) lv_obj_add_state(seg, kPressCueState);
            self->notePressed((uint8_t)slot);
            return;
        }

        if (slot == 8) {
            if (self->buttons[8]) lv_obj_add_state(self->buttons[8], kPressCueState);
            self->notePressed(8);
            return;
        }

        // slot == -1 (gap) or other invalid: no highlight.
        return;
    }

    if (code == LV_EVENT_PRESSING) {
        // If we didn't manage to latch on initial press (e.g., pressed in a gap),
        // allow one-time acquisition while still pressed.
        const bool hasWedge = (self->pressedPieSlot >= 0 && self->pressedPieSlot < 8);
        const bool hasCenter = (self->buttons[8] && lv_obj_has_state(self->buttons[8], kPressCueState));
        if (hasWedge || hasCenter) return;

        if (slot >= 0 && slot < 8) {
            self->pressedPieSlot = (int8_t)slot;
            lv_obj_t* seg = self->pieSegments[slot];
            if (seg) lv_obj_add_state(seg, kPressCueState);
            self->notePressed((uint8_t)slot);
            return;
        }

        if (slot == 8) {
            if (self->buttons[8]) lv_obj_add_state(self->buttons[8], kPressCueState);
            self->notePressed(8);
            return;
        }

        return;
    }

    // Click: clear highlight (if any), then trigger action.
    if (code == LV_EVENT_CLICKED) {
        if (slot < 0) return;
        self->handleButtonClick((uint8_t)slot);
        return;
    }
}

void MacroPadScreen::handleButtonClick(uint8_t b) {
    #if HAS_DISPLAY
    screen_saver_manager_notify_activity(true);
    #endif

    const MacroConfig* cfg = getMacroConfig();
    if (!cfg) return;

    const MacroButtonConfig* btnCfg = &cfg->buttons[screenIndex][b];
    if (!btnCfg) return;

    if (btnCfg->action == MacroButtonAction::None) return;

    if (btnCfg->action == MacroButtonAction::NavNextScreen || btnCfg->action == MacroButtonAction::NavPrevScreen) {
        const uint8_t next = (btnCfg->action == MacroButtonAction::NavNextScreen)
            ? (uint8_t)((screenIndex + 1) % MACROS_SCREEN_COUNT)
            : (uint8_t)((screenIndex + MACROS_SCREEN_COUNT - 1) % MACROS_SCREEN_COUNT);

        char id[16];
        snprintf(id, sizeof(id), "macro%u", (unsigned)(next + 1));
        displayMgr->showScreen(id);
        return;
    }

    if (btnCfg->action == MacroButtonAction::GoBack) {
        // Best-effort: if no previous screen is tracked, fall back to Macro 1.
        if (!displayMgr->goBackOrDefault()) {
            displayMgr->showScreen("macro1");
        }
        return;
    }

    if (btnCfg->action == MacroButtonAction::NavToScreen) {
        const char* target = btnCfg->payload;
        if (!target || target[0] == '\0') {
            displayMgr->showScreen("macro1");
            return;
        }
        if (!displayMgr->showScreen(target)) {
            displayMgr->showScreen("macro1");
        }
        return;
    }

    if (btnCfg->action == MacroButtonAction::SendKeys) {
        if (btnCfg->payload[0] == '\0') {
            Logger.logMessage("Macro", "Empty payload; skipping");
            return;
        }

        BleKeyboardManager* kb = getBleKeyboard();
        ducky_execute(btnCfg->payload, kb);
        return;
    }

    if (btnCfg->action == MacroButtonAction::MqttSend) {
        const char* topic = btnCfg->mqtt_topic;
        const char* payload = btnCfg->payload;

        if (!topic || topic[0] == '\0') {
            displayMgr->showError("MQTT", "Missing topic");
            return;
        }

        #if HAS_MQTT
        MqttManager* mqtt = displayMgr->getMqttManager();
        if (!mqtt) {
            displayMgr->showError("MQTT", "MQTT manager not available");
            return;
        }

        const bool ok = mqtt->publishImmediate(topic, payload ? payload : "", false);
        if (!ok) {
            Logger.logMessagef("Macro", "MQTT publish failed: topic=%s", topic);
            displayMgr->showError("MQTT publish failed", topic);
        }
        #else
        Logger.logMessagef("Macro", "MQTT not supported in this firmware: topic=%s", topic);
        displayMgr->showError("MQTT", "Not supported in this firmware");
        #endif

        return;
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

        // LVGL's image widget will *tile* (repeat) the image when the object is larger than
        // the image and the size mode is VIRTUAL. We want a single icon, so force REAL sizing
        // and let the object size follow the (zoomed) image size.
        lv_img_set_size_mode(icon, LV_IMG_SIZE_MODE_REAL);
        lv_img_set_offset_x(icon, 0);
        lv_img_set_offset_y(icon, 0);
        lv_img_set_zoom(icon, zoom);
        lv_obj_set_size(icon, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
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

void MacroPadScreen::refreshButtons(bool force) {
    if (!screen) return;

    const uint32_t now = millis();
    if (!force && lastUpdateMs != 0 && (uint32_t)(now - lastUpdateMs) < kUiRefreshIntervalMs) {
        return;
    }
    lastUpdateMs = now;

    const MacroConfig* cfg = getMacroConfig();
    if (!cfg) return;

    const char* tpl = resolveTemplateId(cfg);
    const macropad_layout::IMacroPadLayout& layout = macropad_layout::layoutForId(tpl);
    const bool isPie = layout.isPie();

    if (!isPie && pressedPieSlot >= 0 && pressedPieSlot < 8) {
        lv_obj_t* seg = pieSegments[pressedPieSlot];
        if (seg) lv_obj_clear_state(seg, kPressCueState);
        pressedPieSlot = -1;
    }

    // Enable/disable pie helpers depending on the active template.
    if (pieHitLayer) {
        if (isPie) {
            lv_obj_clear_flag(pieHitLayer, LV_OBJ_FLAG_HIDDEN);
            // Keep the hit layer above the arc visuals.
            lv_obj_move_foreground(pieHitLayer);
        } else {
            lv_obj_add_flag(pieHitLayer, LV_OBJ_FLAG_HIDDEN);
        }
    }

    // Apply macro screen background (optional per-screen override, else global default).
    const uint32_t screenBg = (cfg->screen_bg[screenIndex] != MACROS_COLOR_UNSET)
        ? cfg->screen_bg[screenIndex]
        : cfg->default_screen_bg;
    lv_obj_set_style_bg_color(screen, lv_color_hex(screenBg), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    bool anyButtonConfigured = false;

    for (int i = 0; i < MACROS_BUTTONS_PER_SCREEN; i++) {
        const MacroButtonConfig* btnCfg = &cfg->buttons[screenIndex][i];

        if (!layout.isSlotUsed((uint8_t)i)) {
            setButtonVisible(buttons[i], false);
            continue;
        }

        // In pie mode, input should be owned by the full-screen hit layer.
        // Disable clickability for underlying slot containers to avoid LVGL
        // re-targeting the pressed object mid-press.
        if (buttons[i]) {
            if (isPie) lv_obj_clear_flag(buttons[i], LV_OBJ_FLAG_CLICKABLE);
            else lv_obj_add_flag(buttons[i], LV_OBJ_FLAG_CLICKABLE);
        }

        if (btnCfg->action != MacroButtonAction::None) {
            anyButtonConfigured = true;
        }

        const bool visible = btnCfg->action != MacroButtonAction::None;
        setButtonVisible(buttons[i], visible);

        // For navigation buttons, provide sensible default icons if none configured.
        // This avoids “blank” side buttons on templates like round_wide_sides_3.
        const char* effectiveIconId = btnCfg->icon.id;
        if (!effectiveIconId || effectiveIconId[0] == '\0' || btnCfg->icon.type == MacroIconType::None) {
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

        // Keep button base/pressed styles aligned with configured colors.
        {
            const bool isPieOuter = isPie && (i < 8);
            const lv_color_t base = lv_color_hex(buttonBg);
            const lv_color_t flash = flashColorFor(base);

            if (isPieOuter) {
                lv_style_set_bg_opa(&pressStyles.btnBase[i], LV_OPA_TRANSP);
                lv_style_set_bg_opa(&pressStyles.btnPressed[i], LV_OPA_TRANSP);
            } else {
                lv_style_set_bg_opa(&pressStyles.btnBase[i], LV_OPA_COVER);
                lv_style_set_bg_opa(&pressStyles.btnPressed[i], LV_OPA_COVER);
            }
            lv_style_set_bg_color(&pressStyles.btnBase[i], base);
            lv_style_set_bg_color(&pressStyles.btnPressed[i], flash);
        }
        if (labels[i]) {
            lv_obj_set_style_text_color(labels[i], lv_color_hex(labelColor), 0);
        }

        // Pie ring segment background for slots 0..7.
        if (isPie && i < 8) {
            lv_obj_t* seg = pieSegments[i];
            if (seg) {
                const lv_color_t base = lv_color_hex(buttonBg);
                const lv_color_t flash = flashColorFor(base);
                lv_style_set_arc_color(&pressStyles.segBase[i], base);
                lv_style_set_arc_color(&pressStyles.segPressed[i], flash);
                lv_style_set_arc_opa(&pressStyles.segBase[i], LV_OPA_COVER);
                lv_style_set_arc_opa(&pressStyles.segPressed[i], LV_OPA_COVER);
                lv_obj_clear_flag(seg, LV_OBJ_FLAG_HIDDEN);
            }
        }

        // If an icon is set and the user intentionally left the label blank,
        // respect that (icon-only button) instead of forcing a default label.
        const bool userLabelEmpty = (btnCfg->label[0] == '\0');
        const bool wantsIcon = (effectiveIconId && effectiveIconId[0] != '\0');

        // If this is a navigation button and the label is a simple arrow, prefer icon-only
        // ONLY when the user explicitly configured an icon.
        // If the icon comes from a fallback (no configured icon), keep the label so the button
        // doesn't become blank on targets where icons are hard to see.
        const bool labelIsSimpleArrow = (btnCfg->icon.type != MacroIconType::None) && (btnCfg->icon.id[0] != '\0') && (
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

            if (lookupId[0] != '\0' && icon_store_lookup(lookupId, &ref) && ref.dsc) {
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

    // Hide unused pie segments (unconfigured outer slots).
    if (isPie) {
        for (int i = 0; i < 8; i++) {
            const MacroButtonConfig* btnCfg = &cfg->buttons[screenIndex][i];
            const bool segVisible = (btnCfg && btnCfg->action != MacroButtonAction::None);
            if (pieSegments[i]) {
                if (segVisible) {
                    // kept visible above
                } else {
                    lv_obj_add_flag(pieSegments[i], LV_OBJ_FLAG_HIDDEN);
                }
            }
        }
    } else {
        for (int i = 0; i < 8; i++) {
            if (pieSegments[i]) lv_obj_add_flag(pieSegments[i], LV_OBJ_FLAG_HIDDEN);
        }
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

    lv_obj_t* obj = lv_event_get_target(e);
    const lv_event_code_t code = lv_event_get_code(e);

    // Ensure pressed visual feedback (kPressCueState) is driven for our touch targets.
    if (obj) {
        if (code == LV_EVENT_PRESSED) {
            lv_obj_add_state(obj, kPressCueState);
            ctx->self->notePressed(ctx->buttonIndex);
            return;
        }
        if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
            // Enforce a minimum visible cue time on release.
            ctx->self->scheduleReleaseClear(ctx->buttonIndex);
            return;
        }
    }

    MacroPadScreen* self = ctx->self;
    const uint8_t b = ctx->buttonIndex;

    if (code == LV_EVENT_CLICKED) {
        self->handleButtonClick(b);
    }
}
