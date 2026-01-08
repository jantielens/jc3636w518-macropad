#include "error_screen.h"

#include "log_manager.h"
#include "../display_manager.h"
#include "../icon_assets_mono.h"

ErrorScreen::ErrorScreen(DisplayManager* manager) : displayMgr(manager) {}

ErrorScreen::~ErrorScreen() {
    destroy();
}

void ErrorScreen::create() {
    if (screen) return;

    screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(screen, lv_color_black(), 0);

    // Warning icon (optional)
    #if HAS_ICONS
    iconImg = lv_img_create(screen);
    lv_img_set_src(iconImg, &ic_warning);
    lv_obj_align(iconImg, LV_ALIGN_CENTER, 0, -60);
    lv_obj_set_style_img_recolor(iconImg, lv_color_make(255, 180, 0), 0);
    lv_obj_set_style_img_recolor_opa(iconImg, LV_OPA_COVER, 0);
    lv_obj_clear_flag(iconImg, LV_OBJ_FLAG_CLICKABLE);
    #endif

    titleLabel = lv_label_create(screen);
    lv_obj_set_style_text_color(titleLabel, lv_color_white(), 0);
    lv_obj_set_style_text_font(titleLabel, &lv_font_montserrat_18, 0);
    lv_label_set_long_mode(titleLabel, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(titleLabel, lv_pct(92));
    lv_obj_align(titleLabel, LV_ALIGN_CENTER, 0, -20);
    lv_obj_clear_flag(titleLabel, LV_OBJ_FLAG_CLICKABLE);

    messageLabel = lv_label_create(screen);
    lv_obj_set_style_text_color(messageLabel, lv_color_make(200, 200, 200), 0);
    lv_label_set_long_mode(messageLabel, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(messageLabel, lv_pct(92));
    lv_obj_align(messageLabel, LV_ALIGN_CENTER, 0, 20);
    lv_obj_clear_flag(messageLabel, LV_OBJ_FLAG_CLICKABLE);

    hintLabel = lv_label_create(screen);
    lv_obj_set_style_text_color(hintLabel, lv_color_make(140, 140, 140), 0);
    lv_label_set_text(hintLabel, "Tap to go back");
    lv_obj_align(hintLabel, LV_ALIGN_BOTTOM_MID, 0, -10);
    lv_obj_clear_flag(hintLabel, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_add_event_cb(screen, touchEventCallback, LV_EVENT_CLICKED, this);
    lv_obj_add_flag(screen, LV_OBJ_FLAG_CLICKABLE);
}

void ErrorScreen::destroy() {
    if (!screen) return;
    lv_obj_del(screen);
    screen = nullptr;
    titleLabel = nullptr;
    messageLabel = nullptr;
    hintLabel = nullptr;
    iconImg = nullptr;
}

void ErrorScreen::show() {
    create();
    if (screen) lv_scr_load(screen);
}

void ErrorScreen::hide() {
    // No-op
}

void ErrorScreen::update() {
    // No periodic updates
}

void ErrorScreen::setError(const char* title, const char* message) {
    if (!titleLabel || !messageLabel) {
        // If called before create(), defer until show() builds widgets.
        // DisplayManager sets the text again after switching.
        return;
    }

    lv_label_set_text(titleLabel, title && title[0] ? title : "Error");
    lv_label_set_text(messageLabel, message && message[0] ? message : "");
}

void ErrorScreen::touchEventCallback(lv_event_t* e) {
    ErrorScreen* self = (ErrorScreen*)lv_event_get_user_data(e);
    if (!self || !self->displayMgr) return;

    // Always attempt to go back; DisplayManager falls back to macro1 when needed.
    (void)self->displayMgr->goBackOrDefault();
}
