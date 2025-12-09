#ifndef BASE_SCREEN_H
#define BASE_SCREEN_H

#include <lvgl.h>
#include "ui_events.h"

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

 protected:
  // Simple touch tracker to ensure only one CLICKED event per press cycle
  struct TouchTracker {
    bool click_handled = false;
    
    void reset() {
      click_handled = false;
    }
  };
  
  // Global touch tracker shared by all screens
  static TouchTracker touch_tracker_;
  
  // Helper to add button with multi-event handling
  static void addButtonEventCallbacks(lv_obj_t* btn, lv_event_cb_t callback, void* user_data) {
    lv_obj_add_event_cb(btn, callback, LV_EVENT_PRESSED, user_data);
    lv_obj_add_event_cb(btn, callback, LV_EVENT_RELEASED, user_data);
    lv_obj_add_event_cb(btn, callback, LV_EVENT_CLICKED, user_data);
  }
  
  // Process touch events - ensures CLICKED fires only once per press
  static bool processTouchEvent(lv_event_t* e) {
    TouchTracker& tracker = touch_tracker_;
    lv_event_code_t code = lv_event_get_code(e);
    
    if (code == LV_EVENT_PRESSED) {
      tracker.click_handled = false;
      return false;
    } else if (code == LV_EVENT_RELEASED) {
      return false;
    } else if (code == LV_EVENT_CLICKED) {
      // Only process if we haven't handled this click yet
      if (!tracker.click_handled) {
        tracker.click_handled = true;
        return true;
      }
      return false;
    }
    return false;
  }
};

#endif // BASE_SCREEN_H
