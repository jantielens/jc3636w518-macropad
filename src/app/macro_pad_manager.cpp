/*
 * MacroPad Configuration Manager Implementation
 * 
 * Handles persistent storage of macropad configurations using LittleFS + JSON.
 * Configs stored in /littlefs/macropads.json as a JSON file.
 */

#include "macro_pad_manager.h"
#include "log_manager.h"
#include <LittleFS.h>
#include <ArduinoJson.h>

// ============================================================================
// Internal State
// ============================================================================

static MacroPadConfig g_config;
static bool g_initialized = false;

// ============================================================================
// Initialization and Filesystem
// ============================================================================

void macro_pad_manager_init() {
    if (g_initialized) {
        Logger.logMessage("MacroPad", "Already initialized");
        return;
    }
    
    // Initialize config to defaults
    memset(&g_config, 0, sizeof(MacroPadConfig));
    g_config.magic = MACRO_PAD_MAGIC;
    g_config.count = 0;
    
    // Mount LittleFS (format if mount fails)
    if (!LittleFS.begin(true)) {
        Logger.logMessage("MacroPad", "ERROR: Failed to mount LittleFS");
        return;
    }
    
    Logger.logMessage("MacroPad", "LittleFS mounted successfully");
    g_initialized = true;
}

MacroPadConfig* macro_pad_manager_get_config() {
    return &g_config;
}

// ============================================================================
// JSON Serialization
// ============================================================================

static String serialize_button(const MacroButton *btn) {
    JsonDocument doc;
    
    doc["position"] = btn->position;
    doc["label_type"] = btn->label_type == LABEL_TEXT ? "text" : "icon";
    doc["label"] = btn->label;
    doc["icon"] = btn->icon;
    
    // Action type as string
    switch (btn->action_type) {
        case ACTION_KEYSTROKES: doc["action_type"] = "keystrokes"; break;
        case ACTION_NAV_HOME:   doc["action_type"] = "nav_home"; break;
        case ACTION_NAV_NEXT:   doc["action_type"] = "nav_next"; break;
        case ACTION_NAV_PREV:   doc["action_type"] = "nav_prev"; break;
        case ACTION_NAV_GOTO:   doc["action_type"] = "nav_goto"; break;
        default:                doc["action_type"] = "keystrokes"; break;
    }
    
    doc["keystrokes"] = btn->keystrokes;
    doc["nav_target_id"] = btn->nav_target_id;
    doc["enabled"] = btn->enabled;
    
    String output;
    serializeJson(doc, output);
    return output;
}

static bool deserialize_button(const JsonObject &obj, MacroButton *btn) {
    if (!obj.containsKey("position")) return false;
    
    btn->position = obj["position"].as<uint8_t>();
    
    // Label type
    String label_type_str = obj["label_type"].as<String>();
    btn->label_type = (label_type_str == "icon") ? LABEL_ICON : LABEL_TEXT;
    
    strlcpy(btn->label, obj["label"].as<const char*>(), MACRO_LABEL_MAX_LEN);
    strlcpy(btn->icon, obj["icon"].as<const char*>(), MACRO_ICON_MAX_LEN);
    
    // Action type
    String action_type_str = obj["action_type"].as<String>();
    if (action_type_str == "nav_home")        btn->action_type = ACTION_NAV_HOME;
    else if (action_type_str == "nav_next")   btn->action_type = ACTION_NAV_NEXT;
    else if (action_type_str == "nav_prev")   btn->action_type = ACTION_NAV_PREV;
    else if (action_type_str == "nav_goto")   btn->action_type = ACTION_NAV_GOTO;
    else                                       btn->action_type = ACTION_KEYSTROKES;
    
    strlcpy(btn->keystrokes, obj["keystrokes"].as<const char*>(), MACRO_KEYSTROKES_MAX_LEN);
    btn->nav_target_id = obj["nav_target_id"].as<uint8_t>();
    btn->enabled = obj["enabled"].as<bool>();
    
    return true;
}

static String serialize_macropad(const MacroPad *pad) {
    JsonDocument doc;
    
    doc["id"] = pad->id;
    doc["name"] = pad->name;
    doc["template"] = (pad->template_type == TEMPLATE_RADIAL) ? "radial" : "grid";
    doc["button_count"] = pad->button_count;
    doc["enabled"] = pad->enabled;
    
    JsonArray buttons_array = doc["buttons"].to<JsonArray>();
    for (int i = 0; i < MAX_BUTTONS_PER_PAD; i++) {
        if (pad->buttons[i].enabled) {
            JsonDocument btn_doc;
            deserializeJson(btn_doc, serialize_button(&pad->buttons[i]));
            buttons_array.add(btn_doc.as<JsonObject>());
        }
    }
    
    String output;
    serializeJson(doc, output);
    return output;
}

static bool deserialize_macropad(const JsonObject &obj, MacroPad *pad) {
    if (!obj.containsKey("id")) return false;
    
    pad->id = obj["id"].as<uint8_t>();
    strlcpy(pad->name, obj["name"].as<const char*>(), MACRO_NAME_MAX_LEN);
    
    // Template type
    String template_str = obj["template"].as<String>();
    pad->template_type = (template_str == "grid") ? TEMPLATE_GRID : TEMPLATE_RADIAL;
    
    pad->button_count = obj["button_count"].as<uint8_t>();
    pad->enabled = obj["enabled"].as<bool>();
    
    // Initialize all buttons as disabled
    for (int i = 0; i < MAX_BUTTONS_PER_PAD; i++) {
        memset(&pad->buttons[i], 0, sizeof(MacroButton));
        pad->buttons[i].enabled = false;
    }
    
    // Parse buttons array
    JsonArray buttons_array = obj["buttons"].as<JsonArray>();
    for (JsonObject btn_obj : buttons_array) {
        uint8_t pos = btn_obj["position"].as<uint8_t>();
        if (pos < MAX_BUTTONS_PER_PAD) {
            deserialize_button(btn_obj, &pad->buttons[pos]);
        }
    }
    
    return true;
}

// ============================================================================
// Load/Save Operations
// ============================================================================

bool macro_pad_manager_load() {
    if (!g_initialized) {
        Logger.logMessage("MacroPad", "ERROR: Not initialized");
        return false;
    }
    
    File file = LittleFS.open("/macropads.json", "r");
    if (!file) {
        Logger.logMessage("MacroPad", "No config file found (first boot)");
        return false;
    }
    
    size_t file_size = file.size();
    if (file_size == 0 || file_size > 65536) {
        Logger.logMessagef("MacroPad", "Invalid file size: %d bytes", file_size);
        file.close();
        return false;
    }
    
    // Read file content
    String json_content = file.readString();
    file.close();
    
    // Parse JSON
    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, json_content);
    
    if (error) {
        Logger.logMessagef("MacroPad", "JSON parse error: %s", error.c_str());
        return false;
    }
    
    // Validate structure
    if (!doc.containsKey("version") || !doc.containsKey("pads")) {
        Logger.logMessage("MacroPad", "Invalid JSON structure");
        return false;
    }
    
    // Clear current config
    memset(&g_config, 0, sizeof(MacroPadConfig));
    g_config.magic = MACRO_PAD_MAGIC;
    
    // Parse macropads array
    JsonArray pads_array = doc["pads"].as<JsonArray>();
    g_config.count = 0;
    
    for (JsonObject pad_obj : pads_array) {
        if (g_config.count >= MAX_MACRO_PADS) break;
        
        MacroPad pad;
        if (deserialize_macropad(pad_obj, &pad)) {
            if (pad.id < MAX_MACRO_PADS) {
                g_config.pads[pad.id] = pad;
                g_config.count++;
            }
        }
    }
    
    Logger.logMessagef("MacroPad", "Loaded %d macropad configs", g_config.count);
    return true;
}

bool macro_pad_manager_save() {
    if (!g_initialized) {
        Logger.logMessage("MacroPad", "ERROR: Not initialized");
        return false;
    }
    
    // Build JSON document
    JsonDocument doc;
    doc["version"] = 1;
    doc["count"] = g_config.count;
    
    JsonArray pads_array = doc["pads"].to<JsonArray>();
    
    for (int i = 0; i < MAX_MACRO_PADS; i++) {
        if (g_config.pads[i].enabled) {
            JsonDocument pad_doc;
            deserializeJson(pad_doc, serialize_macropad(&g_config.pads[i]));
            pads_array.add(pad_doc.as<JsonObject>());
        }
    }
    
    // Write to file
    File file = LittleFS.open("/macropads.json", "w");
    if (!file) {
        Logger.logMessage("MacroPad", "ERROR: Failed to open file for writing");
        return false;
    }
    
    size_t bytes_written = serializeJson(doc, file);
    file.close();
    
    Logger.logMessagef("MacroPad", "Saved config (%d bytes)", bytes_written);
    return bytes_written > 0;
}

bool macro_pad_manager_reset() {
    if (!g_initialized) {
        Logger.logMessage("MacroPad", "ERROR: Not initialized");
        return false;
    }
    
    // Delete config file
    if (LittleFS.exists("/macropads.json")) {
        LittleFS.remove("/macropads.json");
        Logger.logMessage("MacroPad", "Config file deleted");
    }
    
    // Reset in-memory config
    memset(&g_config, 0, sizeof(MacroPadConfig));
    g_config.magic = MACRO_PAD_MAGIC;
    g_config.count = 0;
    
    Logger.logMessage("MacroPad", "Reset complete");
    return true;
}

// ============================================================================
// Query Operations
// ============================================================================

int macro_pad_manager_count() {
    return g_config.count;
}

bool macro_pad_manager_get(int index, MacroPad *pad) {
    if (!pad) return false;
    if (index < 0 || index >= MAX_MACRO_PADS) return false;
    
    // Find the Nth enabled macropad
    int found = 0;
    for (int i = 0; i < MAX_MACRO_PADS; i++) {
        if (g_config.pads[i].enabled) {
            if (found == index) {
                memcpy(pad, &g_config.pads[i], sizeof(MacroPad));
                return true;
            }
            found++;
        }
    }
    
    return false;
}

bool macro_pad_manager_get_by_id(uint8_t id, MacroPad *pad) {
    if (!pad) return false;
    if (id >= MAX_MACRO_PADS) return false;
    
    if (g_config.pads[id].enabled) {
        memcpy(pad, &g_config.pads[id], sizeof(MacroPad));
        return true;
    }
    
    return false;
}

// ============================================================================
// Modification Operations
// ============================================================================

bool macro_pad_manager_add(const MacroPad *pad) {
    if (!pad) return false;
    if (pad->id >= MAX_MACRO_PADS) return false;
    
    // Validate
    if (!macro_pad_manager_validate(pad)) {
        Logger.logMessage("MacroPad", "ERROR: Validation failed");
        return false;
    }
    
    // Check if slot already used
    if (g_config.pads[pad->id].enabled) {
        Logger.logMessagef("MacroPad", "ERROR: ID %d already in use", pad->id);
        return false;
    }
    
    // Add to config
    memcpy(&g_config.pads[pad->id], pad, sizeof(MacroPad));
    g_config.pads[pad->id].enabled = true;
    g_config.count++;
    
    Logger.logMessagef("MacroPad", "Added macropad ID=%d '%s'", pad->id, pad->name);
    return true;
}

bool macro_pad_manager_update(uint8_t id, const MacroPad *pad) {
    if (!pad) return false;
    if (id >= MAX_MACRO_PADS) return false;
    
    // Validate
    if (!macro_pad_manager_validate(pad)) {
        Logger.logMessage("MacroPad", "ERROR: Validation failed");
        return false;
    }
    
    // Check if exists
    if (!g_config.pads[id].enabled) {
        Logger.logMessagef("MacroPad", "ERROR: ID %d not found", id);
        return false;
    }
    
    // Update config
    memcpy(&g_config.pads[id], pad, sizeof(MacroPad));
    g_config.pads[id].id = id;  // Ensure ID matches
    g_config.pads[id].enabled = true;
    
    Logger.logMessagef("MacroPad", "Updated macropad ID=%d '%s'", id, pad->name);
    return true;
}

bool macro_pad_manager_remove(uint8_t id) {
    if (id >= MAX_MACRO_PADS) return false;
    
    if (!g_config.pads[id].enabled) {
        Logger.logMessagef("MacroPad", "ERROR: ID %d not found", id);
        return false;
    }
    
    // Remove from config
    memset(&g_config.pads[id], 0, sizeof(MacroPad));
    g_config.pads[id].enabled = false;
    g_config.count--;
    
    Logger.logMessagef("MacroPad", "Removed macropad ID=%d", id);
    return true;
}

// ============================================================================
// Helper Functions
// ============================================================================

const char* macro_pad_manager_template_name(MacroTemplate type) {
    switch (type) {
        case TEMPLATE_RADIAL: return "radial";
        case TEMPLATE_GRID:   return "grid";
        default:              return "unknown";
    }
}

int macro_pad_manager_template_max_buttons(MacroTemplate type) {
    // Both templates support 9 buttons
    return MAX_BUTTONS_PER_PAD;
}

const char* macro_pad_manager_action_type_name(MacroActionType type) {
    switch (type) {
        case ACTION_KEYSTROKES: return "keystrokes";
        case ACTION_NAV_HOME:   return "nav_home";
        case ACTION_NAV_NEXT:   return "nav_next";
        case ACTION_NAV_PREV:   return "nav_prev";
        case ACTION_NAV_GOTO:   return "nav_goto";
        default:                return "unknown";
    }
}

const char* macro_pad_manager_label_type_name(MacroLabelType type) {
    switch (type) {
        case LABEL_TEXT: return "text";
        case LABEL_ICON: return "icon";
        default:         return "unknown";
    }
}

// ============================================================================
// Validation
// ============================================================================

bool macro_pad_manager_validate(const MacroPad *pad) {
    if (!pad) return false;
    
    // Check ID range
    if (pad->id >= MAX_MACRO_PADS) {
        Logger.logMessagef("MacroPad", "ERROR: Invalid ID %d (max %d)", pad->id, MAX_MACRO_PADS - 1);
        return false;
    }
    
    // Check name not empty
    if (strlen(pad->name) == 0) {
        Logger.logMessage("MacroPad", "ERROR: Name cannot be empty");
        return false;
    }
    
    // Check button positions are unique
    bool positions_used[MAX_BUTTONS_PER_PAD] = {false};
    int enabled_count = 0;
    
    for (int i = 0; i < MAX_BUTTONS_PER_PAD; i++) {
        if (pad->buttons[i].enabled) {
            uint8_t pos = pad->buttons[i].position;
            
            if (pos >= MAX_BUTTONS_PER_PAD) {
                Logger.logMessagef("MacroPad", "ERROR: Invalid button position %d", pos);
                return false;
            }
            
            if (positions_used[pos]) {
                Logger.logMessagef("MacroPad", "ERROR: Duplicate button position %d", pos);
                return false;
            }
            
            positions_used[pos] = true;
            enabled_count++;
        }
    }
    
    // Check button count matches
    if (enabled_count != pad->button_count) {
        Logger.logMessagef("MacroPad", "WARNING: button_count mismatch (%d vs %d)", 
                          pad->button_count, enabled_count);
    }
    
    return true;
}

// ============================================================================
// Debug
// ============================================================================

void macro_pad_manager_print() {
    Logger.logMessagef("MacroPad", "=== MacroPad Config (count=%d) ===", g_config.count);
    
    for (int i = 0; i < MAX_MACRO_PADS; i++) {
        if (g_config.pads[i].enabled) {
            const MacroPad *pad = &g_config.pads[i];
            Logger.logMessagef("MacroPad", "  [%d] '%s' - %s template, %d buttons",
                              pad->id, pad->name,
                              macro_pad_manager_template_name(pad->template_type),
                              pad->button_count);
            
            for (int j = 0; j < MAX_BUTTONS_PER_PAD; j++) {
                if (pad->buttons[j].enabled) {
                    const MacroButton *btn = &pad->buttons[j];
                    Logger.logMessagef("MacroPad", "    Btn[%d]: %s='%s' action=%s",
                                      btn->position,
                                      macro_pad_manager_label_type_name(btn->label_type),
                                      btn->label_type == LABEL_TEXT ? btn->label : btn->icon,
                                      macro_pad_manager_action_type_name(btn->action_type));
                }
            }
        }
    }
}
