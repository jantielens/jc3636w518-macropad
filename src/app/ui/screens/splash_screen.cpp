#include "splash_screen.h"
#include <lvgl.h>
#include <string.h>
#include "../../web_assets.h"   // PROJECT_DISPLAY_NAME
#include "../../version.h"      // FIRMWARE_VERSION (adjusted for Arduino include from app.ino)

lv_obj_t* SplashScreen::root() {
  if (!root_) build();
  return root_;
}

void SplashScreen::cleanup() {
  // Just reset pointers - LVGL will delete the objects
  root_ = nullptr;
  status_label_ = nullptr;
}

void SplashScreen::build() {
  root_ = lv_obj_create(nullptr);
  lv_obj_set_style_bg_color(root_, lv_color_black(), 0);
  lv_obj_set_style_bg_opa(root_, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(root_, 0, 0);
  lv_obj_set_style_pad_all(root_, 16, 0);

  // Title (firmware name)
  lv_obj_t* title = lv_label_create(root_);
  lv_label_set_text(title, PROJECT_DISPLAY_NAME);
  lv_obj_set_style_text_color(title, lv_color_white(), 0);
  lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_width(title, lv_pct(90));
  lv_obj_align(title, LV_ALIGN_CENTER, 0, -90);

  // Version label
  lv_obj_t* ver = lv_label_create(root_);
  lv_label_set_text_fmt(ver, "v%s", FIRMWARE_VERSION);
  lv_obj_set_style_text_color(ver, lv_color_white(), 0);
  lv_obj_set_style_text_align(ver, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_width(ver, lv_pct(80));
  lv_obj_align_to(ver, title, LV_ALIGN_OUT_BOTTOM_MID, 0, 12);

  // Status label (multi-line for WiFi details)
  status_label_ = lv_label_create(root_);
  lv_label_set_long_mode(status_label_, LV_LABEL_LONG_WRAP);
  lv_obj_set_width(status_label_, lv_pct(90));
  lv_label_set_text(status_label_, "Booting...");
  lv_obj_set_style_text_color(status_label_, lv_color_white(), 0);
  lv_obj_set_style_text_align(status_label_, LV_TEXT_ALIGN_CENTER, 0);
  // Longest/variable text sits near the middle of the round display
  lv_obj_align(status_label_, LV_ALIGN_CENTER, 0, 20);
}

void SplashScreen::handle(const UiEvent &evt) {
  if (evt.type == UiEventType::BootStatus && status_label_) {
    lv_label_set_text(status_label_, evt.msg);
  }
}
