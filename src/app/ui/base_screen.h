#ifndef BASE_SCREEN_H
#define BASE_SCREEN_H

#include <lvgl.h>
#include "ui_events.h"

// Simplified base class for all screens (no touch tracking - we don't need swipe detection)
class BaseScreen {
 public:
  virtual ~BaseScreen() = default;
  
  // Build (if needed) and return the root object for this screen
  virtual lv_obj_t* root() = 0;
  
  // Lifecycle hooks
  virtual void onEnter() {}
  virtual void onExit() {}
  
  // Handle UI events
  virtual void handle(const UiEvent &evt) {(void)evt;}
  
  // Clean up LVGL root object to free memory
  virtual void cleanup() {}
};

#endif // BASE_SCREEN_H
