#include "macropad_screen.h"

#include "../display_manager.h"
#include "../macros_config.h"
#include "../macro_templates.h"
#include "../ble_keyboard_manager.h"
#include "../ducky_script.h"
#include "../log_manager.h"
#include "../config_manager.h"

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

        lv_obj_t* lbl = lv_label_create(btn);
        lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_color(lbl, lv_color_white(), 0);
        lv_label_set_long_mode(lbl, LV_LABEL_LONG_WRAP);
        // Width is updated in layoutButtons() once button size is known.
        lv_obj_center(lbl);

        buttons[i] = btn;
        labels[i] = lbl;
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
    // Keep it relatively small; we'll refine visually later.
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
        if (!visible) continue;

        char labelBuf[32];
        const char* labelText = btnCfg->label;
        if (!labelText || !*labelText) {
            defaultLabel(labelBuf, sizeof(labelBuf), screenIndex, (uint8_t)i);
            labelText = labelBuf;
        }

        // If action isn't SendKeys, show a small hint so blank labels are still meaningful.
        if ((btnCfg->label == nullptr || btnCfg->label[0] == '\0') && btnCfg->action != MacroButtonAction::SendKeys) {
            char combined[40];
            snprintf(combined, sizeof(combined), "%s\n(%s)", labelText, actionToShortLabel(btnCfg->action));
            lv_label_set_text(labels[i], combined);
        } else {
            lv_label_set_text(labels[i], labelText);
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
        const char* tpl = cfg->template_id[screenIndex];
        if (tpl && tpl[0] != '\0' && strncmp(tpl, lastTemplateId, sizeof(lastTemplateId)) != 0) {
            // Template changed: re-layout immediately.
            strlcpy(lastTemplateId, tpl, sizeof(lastTemplateId));
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
