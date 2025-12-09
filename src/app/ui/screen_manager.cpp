#include "screen_manager.h"
#include "ui_events.h"
#include "screens/splash_screen.h"
#include "screens/home_screen.h"
#include "screens/macropad_screen.h"
#include "../macro_pad_manager.h"
#include "../log_manager.h"

// Global instance
ScreenManager UI;

// Screen instances (allocated once)
static SplashScreen splash_screen;
static HomeScreen home_screen;

// Lazy-created MacroPad screens (up to 10)
static MacroPadScreen* macropad_screens_[MAX_MACRO_PADS] = {nullptr};

// Navigation sequence (IDs of enabled screens) - static array instead of vector
static ScreenId nav_sequence_[MAX_MACRO_PADS + 1]; // +1 for Home
static int nav_sequence_count_ = 0;

// Helper: Get screen instance (with lazy creation for MacroPad screens)
static BaseScreen* get_screen(ScreenId id) {
  switch (id) {
    case ScreenId::Splash:
      return &splash_screen;
    case ScreenId::Home:
      return &home_screen;
    case ScreenId::MacroPad0:
    case ScreenId::MacroPad1:
    case ScreenId::MacroPad2:
    case ScreenId::MacroPad3:
    case ScreenId::MacroPad4:
    case ScreenId::MacroPad5:
    case ScreenId::MacroPad6:
    case ScreenId::MacroPad7:
    case ScreenId::MacroPad8:
    case ScreenId::MacroPad9: {
      int idx = (int)id - (int)ScreenId::MacroPad0;
      if (idx >= 0 && idx < MAX_MACRO_PADS) {
        if (!macropad_screens_[idx]) {
          macropad_screens_[idx] = new MacroPadScreen(idx);
          Logger.logMessagef("ScreenMgr", "Created MacroPadScreen for pad #%d", idx);
        }
        return macropad_screens_[idx];
      }
      return &splash_screen;
    }
    default:
      return &splash_screen;
  }
}

void ScreenManager::begin(ScreenId initial) {
  current_id_ = initial;
  current_ = get_screen(initial);
  
  // Load the screen
  if (current_) {
    current_root_ = current_->root();
    if (current_root_) {
      lv_scr_load(current_root_);
      current_->onEnter();
    }
  }
  
  // NOTE: Don't call rebuildNavigation() here - macro pad manager not initialized yet
  // It will be called from app.ino after macro_pad_manager_load()
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
  BaseScreen* next = get_screen(id);
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

void ScreenManager::navigateToNext() {
  if (nav_sequence_count_ == 0) {
    Logger.logMessage("ScreenMgr", "No navigation sequence available");
    return;
  }
  
  // Find current screen in sequence
  int current_pos = -1;
  for (int i = 0; i < nav_sequence_count_; i++) {
    if (nav_sequence_[i] == current_id_) {
      current_pos = i;
      break;
    }
  }
  
  // Navigate to next (wrap to start)
  int next_pos = (current_pos + 1) % nav_sequence_count_;
  ScreenId next_id = nav_sequence_[next_pos];
  
  Logger.logMessagef("ScreenMgr", "Navigate Next: pos %d -> %d (ID %d)", 
                     current_pos, next_pos, (int)next_id);
  navigate(next_id, LV_SCR_LOAD_ANIM_MOVE_LEFT, 200, 0);
}

void ScreenManager::navigateToPrevious() {
  if (nav_sequence_count_ == 0) {
    Logger.logMessage("ScreenMgr", "No navigation sequence available");
    return;
  }
  
  // Find current screen in sequence
  int current_pos = -1;
  for (int i = 0; i < nav_sequence_count_; i++) {
    if (nav_sequence_[i] == current_id_) {
      current_pos = i;
      break;
    }
  }
  
  // Navigate to previous (wrap to end)
  int prev_pos = (current_pos - 1 + nav_sequence_count_) % nav_sequence_count_;
  ScreenId prev_id = nav_sequence_[prev_pos];
  
  Logger.logMessagef("ScreenMgr", "Navigate Prev: pos %d -> %d (ID %d)", 
                     current_pos, prev_pos, (int)prev_id);
  navigate(prev_id, LV_SCR_LOAD_ANIM_MOVE_RIGHT, 200, 0);
}

void ScreenManager::rebuildNavigation() {
  nav_sequence_count_ = 0;
  
  // Always start with Home
  nav_sequence_[nav_sequence_count_++] = ScreenId::Home;
  
  // Add enabled MacroPad screens with buttons
  for (int i = 0; i < macro_pad_manager_count(); i++) {
    MacroPad pad;
    if (macro_pad_manager_get(i, &pad)) {
      // Check if pad has at least one non-empty button
      bool has_buttons = false;
      for (int j = 0; j < pad.button_count; j++) {
        const MacroButton& btn = pad.buttons[j];
        bool has_label = (btn.label_type == LABEL_TEXT && strlen(btn.label) > 0);
        bool has_icon = (btn.label_type == LABEL_ICON && strlen(btn.icon) > 0);
        bool has_keystrokes = (btn.action_type == ACTION_KEYSTROKES && strlen(btn.keystrokes) > 0);
        if (has_label || has_icon || has_keystrokes) {
          has_buttons = true;
          break;
        }
      }
      
      if (has_buttons && pad.enabled && nav_sequence_count_ < (MAX_MACRO_PADS + 1)) {
        ScreenId pad_screen = (ScreenId)((int)ScreenId::MacroPad0 + pad.id);
        nav_sequence_[nav_sequence_count_++] = pad_screen;
        Logger.logMessagef("ScreenMgr", "  Added MacroPad %d ('%s') to nav sequence", 
                           pad.id, pad.name);
      }
    }
  }
  
  Logger.logMessagef("ScreenMgr", "Navigation sequence rebuilt: %d screens", 
                     nav_sequence_count_);
}
