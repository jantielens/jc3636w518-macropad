#ifndef SCREEN_MANAGER_H
#define SCREEN_MANAGER_H

#include <lvgl.h>
#include "base_screen.h"

// Simple screen IDs for navigation
enum class ScreenId : uint8_t {
  Splash = 0,
  Home,
  MacroPad0,
  MacroPad1,
  MacroPad2,
  MacroPad3,
  MacroPad4,
  MacroPad5,
  MacroPad6,
  MacroPad7,
  MacroPad8,
  MacroPad9,
  // Add more screens as needed
};

// Minimal screen manager (no swipe navigation)
class ScreenManager {
 public:
  void begin(ScreenId initial = ScreenId::Splash);
  void loop(); // Polls UI events and calls handle() on active screen
  void navigate(ScreenId id,
                lv_scr_load_anim_t anim = LV_SCR_LOAD_ANIM_NONE,
                uint32_t time = 300,
                uint32_t delay = 0);
  
  void navigateToNext();      // Navigate to next macropad in sequence
  void navigateToPrevious();  // Navigate to previous macropad in sequence
  void rebuildNavigation();   // Rebuild navigation sequence after config changes
  
  ScreenId currentId() const { return current_id_; }

 private:
  BaseScreen* current_ = nullptr;
  ScreenId current_id_ = ScreenId::Splash;
  lv_obj_t* current_root_ = nullptr;
};

extern ScreenManager UI;

#endif // SCREEN_MANAGER_H
