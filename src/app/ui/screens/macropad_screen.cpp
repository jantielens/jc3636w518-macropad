/*
 * MacroPad Screen Implementation
 * 
 * Renders configurable macropad buttons with RADIAL or GRID layouts.
 */

#include "macropad_screen.h"
#include "../../log_manager.h"
#include "../../BleKeyboard.h"
#include "../../keystrokes_handler.h"
#include "../screen_manager.h"
#include <lvgl.h>
#include <cmath>

// External BLE keyboard instance
extern BleKeyboard* bleKeyboard;

// ============================================================================
// Layout Position Tables
// ============================================================================

// RADIAL: Center button + 8 outer positions in circle
// 90px buttons at 135px radius - outer buttons touch 360px screen edge
// Position 0: Center
// Positions 1-8: Outer ring at 135px radius, starting from top (12 o'clock) clockwise
static const int RADIAL_POSITIONS[9][2] = {
  {0, 0},         // 0: Center
  {0, -135},      // 1: Top (12 o'clock)
  {95, -95},      // 2: Upper-right (1:30)
  {135, 0},       // 3: Right (3 o'clock)
  {95, 95},       // 4: Lower-right (4:30)
  {0, 135},       // 5: Bottom (6 o'clock)
  {-95, 95},      // 6: Lower-left (7:30)
  {-135, 0},      // 7: Left (9 o'clock)
  {-95, -95}      // 8: Upper-left (10:30)
};

// GRID: Diamond pattern (1-2-3-2-1 rows)
// Optimized for 360×360 round screen
// Button size: 120×60px (width×height)
static const int GRID_POSITIONS[9][2] = {
  {0, -140},      // 0: Top (row 1)
  {-65, -70},     // 1: Upper-left (row 2)
  {65, -70},      // 2: Upper-right (row 2)
  {-125, 0},      // 3: Middle-left (row 3) - with horizontal spacing
  {0, 0},         // 4: Center (row 3)
  {125, 0},       // 5: Middle-right (row 3) - with horizontal spacing
  {-65, 70},      // 6: Lower-left (row 4)
  {65, 70},       // 7: Lower-right (row 4)
  {0, 140}        // 8: Bottom (row 5)
};

// ============================================================================
// Constructor
// ============================================================================

MacroPadScreen::MacroPadScreen(uint8_t pad_id) 
  : pad_id_(pad_id) {
  // Initialize button arrays
  for (int i = 0; i < MAX_BUTTONS_PER_PAD; i++) {
    buttons_[i] = nullptr;
    button_to_config_[i] = -1;
  }
}

// ============================================================================
// BaseScreen Interface
// ============================================================================

lv_obj_t* MacroPadScreen::root() {
  if (!root_) build();
  return root_;
}

void MacroPadScreen::cleanup() {
  // Reset pointers - LVGL will delete the objects
  root_ = nullptr;
  title_label_ = nullptr;
  for (int i = 0; i < MAX_BUTTONS_PER_PAD; i++) {
    buttons_[i] = nullptr;
    button_to_config_[i] = -1;
  }
}

void MacroPadScreen::handle(const UiEvent &evt) {
  // No event handling needed for now
}

// ============================================================================
// Screen Building
// ============================================================================

void MacroPadScreen::build() {
  // Load macro pad configuration
  if (!macro_pad_manager_get_by_id(pad_id_, &pad_)) {
    Logger.logMessagef("MacroPad", "Failed to load pad #%d", pad_id_);
    
    // Create error screen
    root_ = lv_obj_create(nullptr);
    lv_obj_set_style_bg_color(root_, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(root_, LV_OPA_COVER, 0);
    
    lv_obj_t* label = lv_label_create(root_);
    lv_label_set_text(label, "Macro Pad\nNot Found");
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(0xFF4444), 0);
    lv_obj_center(label);
    return;
  }
  
  pad_loaded_ = true;
  Logger.logMessagef("MacroPad", "Loaded: %s (%d buttons, template=%d)", 
                     pad_.name, pad_.button_count, pad_.template_type);
  
  // Create root screen
  root_ = lv_obj_create(nullptr);
  lv_obj_set_style_bg_color(root_, lv_color_black(), 0);
  lv_obj_set_style_bg_opa(root_, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(root_, 0, 0);
  
  // Disable scrolling and scrollbars
  lv_obj_clear_flag(root_, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_scrollbar_mode(root_, LV_SCROLLBAR_MODE_OFF);
  
  // Store pointer to this screen instance on root for event callbacks
  lv_obj_set_user_data(root_, this);
  
  // Select position table based on template
  const int (*positions)[2] = (pad_.template_type == TEMPLATE_RADIAL) 
                               ? RADIAL_POSITIONS 
                               : GRID_POSITIONS;
  
  // Create buttons based on configuration
  int button_count = 0;
  for (int i = 0; i < pad_.button_count; i++) {
    const MacroButton& btn_config = pad_.buttons[i];
    
    // Skip buttons with no label AND no icon AND no keystrokes
    bool has_label = (btn_config.label_type == LABEL_TEXT && strlen(btn_config.label) > 0);
    bool has_icon = (btn_config.label_type == LABEL_ICON && strlen(btn_config.icon) > 0);
    bool has_keystrokes = (btn_config.action_type == ACTION_KEYSTROKES && strlen(btn_config.keystrokes) > 0);
    
    if (!has_label && !has_icon && !has_keystrokes) {
      Logger.logMessagef("MacroPad", "  Skipping button %d (empty)", i);
      continue;
    }
    
    // Validate position
    if (btn_config.position >= MAX_BUTTONS_PER_PAD) {
      Logger.logMessagef("MacroPad", "  Invalid position %d for button %d", btn_config.position, i);
      continue;
    }
    
    // Get layout position
    int pos_x = positions[btn_config.position][0];
    int pos_y = positions[btn_config.position][1];
    
    // Button size and shape based on layout
    int btn_width, btn_height, btn_radius;
    if (pad_.template_type == TEMPLATE_RADIAL) {
      // RADIAL: Circular buttons - all same size (90×90px)
      btn_width = 90;
      btn_height = 90;
      btn_radius = 45;  // Fully circular (radius = size/2)
    } else {
      // GRID: Rectangular buttons maximized for 360×360 screen
      // Width: 120px (3 columns with spacing)
      // Height: 60px (5 rows with spacing)
      btn_width = 120;
      btn_height = 60;
      btn_radius = 8;
    }
    
    // Create button
    lv_obj_t* btn = lv_btn_create(root_);
    lv_obj_set_size(btn, btn_width, btn_height);
    lv_obj_align(btn, LV_ALIGN_CENTER, pos_x, pos_y);
    
    // Button styling - clean gray background, no visual effects
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x2C2C2C), 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);  // Solid color, no transparency
    lv_obj_set_style_border_width(btn, 0, 0);  // No border/outline
    lv_obj_set_style_shadow_width(btn, 0, 0);  // No shadow
    lv_obj_set_style_radius(btn, btn_radius, 0);
    
    // Pressed state styling - lighter color, no visual effects
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x667EEA), LV_STATE_PRESSED);
    lv_obj_set_style_border_width(btn, 0, LV_STATE_PRESSED);
    lv_obj_set_style_shadow_width(btn, 0, LV_STATE_PRESSED);
    
    // Add label (text only for now, icons in Phase 6)
    lv_obj_t* label = lv_label_create(btn);
    if (btn_config.label_type == LABEL_TEXT && strlen(btn_config.label) > 0) {
      lv_label_set_text(label, btn_config.label);
    } else {
      // Placeholder for empty labels
      lv_label_set_text(label, "•");
    }
    lv_obj_set_style_text_color(label, lv_color_white(), 0);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(label);
    
    // Store button reference and mapping
    buttons_[button_count] = btn;
    button_to_config_[button_count] = i;
    
    // Add event callbacks with touch tracking
    addButtonEventCallbacks(btn, button_event_cb, this);
    
    button_count++;
    Logger.logMessagef("MacroPad", "  Button %d: pos=%d label='%s' action=%d", 
                       i, btn_config.position, btn_config.label, btn_config.action_type);
  }
  
  Logger.logMessagef("MacroPad", "Created %d buttons for pad '%s'", button_count, pad_.name);
}

// ============================================================================
// Button Event Handling
// ============================================================================

void MacroPadScreen::button_event_cb(lv_event_t* e) {
  MacroPadScreen* screen = static_cast<MacroPadScreen*>(lv_event_get_user_data(e));
  
  // Use BaseScreen's touch tracking to filter swipes
  if (processTouchEvent(e)) {
    lv_obj_t* btn = lv_event_get_target(e);
    screen->handleButtonPress(btn);
  }
}

void MacroPadScreen::handleButtonPress(lv_obj_t* btn) {
  if (!pad_loaded_) return;
  
  // Find which button was pressed
  int button_index = -1;
  for (int i = 0; i < MAX_BUTTONS_PER_PAD; i++) {
    if (buttons_[i] == btn) {
      button_index = i;
      break;
    }
  }
  
  if (button_index < 0 || button_index >= MAX_BUTTONS_PER_PAD) {
    Logger.logMessage("MacroPad", "Unknown button pressed");
    return;
  }
  
  int config_index = button_to_config_[button_index];
  if (config_index < 0 || config_index >= pad_.button_count) {
    Logger.logMessage("MacroPad", "Invalid button config index");
    return;
  }
  
  const MacroButton& btn_config = pad_.buttons[config_index];
  
  Logger.logMessagef("MacroPad", "Button pressed: %d (config %d) action=%d", 
                     button_index, config_index, btn_config.action_type);
  
  // Handle action based on type
  switch (btn_config.action_type) {
    case ACTION_KEYSTROKES:
      if (strlen(btn_config.keystrokes) > 0) {
        Logger.logMessagef("MacroPad", "  -> Executing keystrokes: '%s'", btn_config.keystrokes);
        if (keystrokes_handler_execute(btn_config.keystrokes)) {
          Logger.logMessage("MacroPad", "  -> Keystrokes sent successfully");
        } else {
          Logger.logMessage("MacroPad", "  -> Failed to send keystrokes (BLE not connected or parse error)");
        }
      } else {
        Logger.logMessage("MacroPad", "  -> No keystrokes configured for this button");
      }
      break;
      
    case ACTION_NAV_HOME:
      Logger.logMessage("MacroPad", "  -> Navigate to Home");
      UI.navigate(ScreenId::Home, LV_SCR_LOAD_ANIM_FADE_ON, 200, 0);
      break;
      
    case ACTION_NAV_NEXT:
      Logger.logMessage("MacroPad", "  -> Navigate to Next");
      UI.navigateToNext();
      break;
      
    case ACTION_NAV_PREV:
      Logger.logMessage("MacroPad", "  -> Navigate to Previous");
      UI.navigateToPrevious();
      break;
      
    case ACTION_NAV_GOTO:
      Logger.logMessagef("MacroPad", "  -> Navigate to MacroPad ID %d", btn_config.nav_target_id);
      // TODO: Implement goto by ID
      break;
      
    default:
      Logger.logMessagef("MacroPad", "  -> Unknown action type: %d", btn_config.action_type);
      break;
  }
}
