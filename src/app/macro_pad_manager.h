/*
 * MacroPad Configuration Manager
 * 
 * Manages persistent storage of macropad screen configurations using LittleFS + JSON.
 * Each macropad screen has 9 configurable buttons with icons/labels and keystrokes.
 * 
 * USAGE:
 *   macro_pad_manager_init();        // Initialize LittleFS
 *   if (macro_pad_manager_load()) {  // Try to load saved configs
 *       // Configs loaded
 *   }
 *   macro_pad_manager_save();        // Save after modifications
 *   macro_pad_manager_reset();       // Erase all configs
 */

#ifndef MACRO_PAD_MANAGER_H
#define MACRO_PAD_MANAGER_H

#include <Arduino.h>

// ============================================================================
// Constants
// ============================================================================

#define MAX_MACRO_PADS 10
#define MAX_BUTTONS_PER_PAD 9
#define MACRO_NAME_MAX_LEN 32
#define MACRO_LABEL_MAX_LEN 24
#define MACRO_ICON_MAX_LEN 32
#define MACRO_KEYSTROKES_MAX_LEN 256

#define MACRO_PAD_MAGIC 0xCAFEFEED

// ============================================================================
// Enumerations
// ============================================================================

// Action types: what happens when button is tapped
enum MacroActionType {
    ACTION_KEYSTROKES = 0,  // Send keystrokes string via BLE keyboard
    ACTION_NAV_HOME = 1,    // Navigate to home screen
    ACTION_NAV_NEXT = 2,    // Navigate to next macropad screen
    ACTION_NAV_PREV = 3,    // Navigate to previous macropad screen
    ACTION_NAV_GOTO = 4     // Navigate to specific screen (by ID)
};

// Label types: icon or text
enum MacroLabelType {
    LABEL_TEXT = 0,         // Plain text label
    LABEL_ICON = 1          // Icon name (from material_icons font)
};

// Template types: layout patterns
enum MacroTemplate {
    TEMPLATE_RADIAL = 0,    // 1 center + 8 outer buttons in circle
    TEMPLATE_GRID = 1       // 1-2-3-2-1 diamond pattern
};

// ============================================================================
// Data Structures
// ============================================================================

// Button configuration
struct MacroButton {
    uint8_t position;                              // Button position (0-8)
    MacroLabelType label_type;                     // Text or icon
    char label[MACRO_LABEL_MAX_LEN];              // Text label (if label_type == LABEL_TEXT)
    char icon[MACRO_ICON_MAX_LEN];                // Icon name (if label_type == LABEL_ICON)
    MacroActionType action_type;                   // Keystrokes or navigation
    char keystrokes[MACRO_KEYSTROKES_MAX_LEN];    // Keystrokes string (if action_type == ACTION_KEYSTROKES)
    uint8_t nav_target_id;                         // Target screen ID (if action_type == ACTION_NAV_GOTO)
    bool enabled;                                  // Whether this button is configured
};

// Macro pad screen configuration
struct MacroPad {
    uint8_t id;                                    // Unique ID (0-9)
    char name[MACRO_NAME_MAX_LEN];                // User-friendly name (e.g., "Media Controls")
    MacroTemplate template_type;                   // Radial or grid layout
    uint8_t button_count;                         // Number of configured buttons (0-9)
    MacroButton buttons[MAX_BUTTONS_PER_PAD];     // Button configurations
    bool enabled;                                  // Whether this screen is active
};

// Top-level config (stored in JSON)
struct MacroPadConfig {
    uint8_t count;                                 // Number of configured macro pads (0-10)
    MacroPad pads[MAX_MACRO_PADS];                // All macropad configurations
    uint32_t magic;                                // Validation: 0xCAFEFEED
};

// ============================================================================
// API Functions
// ============================================================================

// Initialization and persistence
void macro_pad_manager_init();                     // Initialize LittleFS filesystem
bool macro_pad_manager_load();                     // Load configs from /littlefs/macropads.json
bool macro_pad_manager_save();                     // Save configs to /littlefs/macropads.json
bool macro_pad_manager_reset();                    // Erase all configs (delete JSON file)

// Query operations
int macro_pad_manager_count();                     // Get number of active macropads
bool macro_pad_manager_get(int index, MacroPad *pad);          // Get macropad by index (0-based)
bool macro_pad_manager_get_by_id(uint8_t id, MacroPad *pad);   // Get macropad by ID
MacroPadConfig* macro_pad_manager_get_config();    // Get pointer to entire config (internal use)

// Modification operations
bool macro_pad_manager_add(const MacroPad *pad);   // Add new macropad
bool macro_pad_manager_update(uint8_t id, const MacroPad *pad); // Update existing macropad
bool macro_pad_manager_remove(uint8_t id);         // Remove macropad by ID

// Helper functions
const char* macro_pad_manager_template_name(MacroTemplate type);       // Get template name as string
int macro_pad_manager_template_max_buttons(MacroTemplate type);        // Get max buttons for template
const char* macro_pad_manager_action_type_name(MacroActionType type); // Get action type name as string
const char* macro_pad_manager_label_type_name(MacroLabelType type);   // Get label type name as string

// Debug and validation
void macro_pad_manager_print();                    // Print all configs to serial (debug)
bool macro_pad_manager_validate(const MacroPad *pad); // Validate macropad configuration

#endif // MACRO_PAD_MANAGER_H
