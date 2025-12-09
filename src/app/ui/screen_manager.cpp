#include "screen_manager.h"
#include "ui_events.h"
#include "screens/splash_screen.h"
#include "screens/home_screen.h"

// Global instance
ScreenManager UI;

// Screen instances (allocated once)
static SplashScreen splash_screen;
static HomeScreen home_screen;
// static MacroPadScreen macropad_screen; // TODO: Create this screen

void ScreenManager::begin(ScreenId initial) {
  current_id_ = initial;
  
  // Get screen instance
  switch (initial) {
    case ScreenId::Splash:
      current_ = &splash_screen;
      break;
    case ScreenId::Home:
      current_ = &home_screen;
      break;
    case ScreenId::MacroPad:
      // current_ = &macropad_screen; // TODO: Implement
      current_ = &splash_screen; // Fallback to splash for now
      break;
    default:
      current_ = &splash_screen;
      break;
  }
  
  // Load the screen
  if (current_) {
    current_root_ = current_->root();
    if (current_root_) {
      lv_scr_load(current_root_);
      current_->onEnter();
    }
  }
}

void ScreenManager::loop() {
  // Poll UI events and dispatch to current screen
  UiEvent evt;
  while (ui_poll(&evt)) {
    if (current_) {
      current_->handle(evt);
    }
  }
}

void ScreenManager::navigate(ScreenId id, lv_scr_load_anim_t anim, uint32_t time, uint32_t delay) {
  if (id == current_id_) return;
  
  // Exit current screen
  if (current_) {
    current_->onExit();
  }
  
  // Get new screen instance
  BaseScreen* next = nullptr;
  switch (id) {
    case ScreenId::Splash:
      next = &splash_screen;
      break;
    case ScreenId::Home:
      next = &home_screen;
      break;
    case ScreenId::MacroPad:
      // next = &macropad_screen; // TODO: Implement
      next = &splash_screen; // Fallback to splash for now
      break;
    default:
      next = &splash_screen;
      break;
  }
  
  if (!next) return;
  
  // Load new screen
  lv_obj_t* next_root = next->root();
  if (next_root) {
    lv_scr_load_anim(next_root, anim, time, delay, false);
    next->onEnter();
    current_ = next;
    current_id_ = id;
    current_root_ = next_root;
  }
}
