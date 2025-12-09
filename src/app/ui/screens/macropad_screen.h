/*
 * MacroPad Screen
 * 
 * Renders a configurable macropad screen with 9 buttons in either:
 * - RADIAL layout: 1 center + 8 outer buttons in circle
 * - GRID layout: 1-2-3-2-1 diamond pattern
 * 
 * Supports both action buttons (keystrokes) and navigation buttons.
 */

#ifndef MACROPAD_SCREEN_H
#define MACROPAD_SCREEN_H

#include "../base_screen.h"
#include "../../macro_pad_manager.h"

class MacroPadScreen : public BaseScreen {
 public:
  MacroPadScreen(uint8_t pad_id);
  
  lv_obj_t* root() override;
  void handle(const UiEvent &evt) override;
  void cleanup() override;

 private:
  void build();
  static void button_event_cb(lv_event_t* e);
  void handleButtonPress(lv_obj_t* btn);
  
  uint8_t pad_id_;                          // Which macro pad to display
  MacroPad pad_;                            // Loaded configuration
  bool pad_loaded_ = false;
  
  lv_obj_t* root_ = nullptr;
  lv_obj_t* title_label_ = nullptr;
  
  // Button references (up to 9)
  lv_obj_t* buttons_[MAX_BUTTONS_PER_PAD];
  
  // Map button objects to their configuration indices
  int button_to_config_[MAX_BUTTONS_PER_PAD];
};

#endif // MACROPAD_SCREEN_H
