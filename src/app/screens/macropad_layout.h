#ifndef MACROPAD_LAYOUT_H
#define MACROPAD_LAYOUT_H

#include <lvgl.h>
#include <stdint.h>

#include "../macros_config.h"

class DisplayManager;

namespace macropad_layout {

struct MacroPadLayoutContext {
    DisplayManager* displayMgr;
    lv_obj_t* screen;

    lv_obj_t** buttons; // length: MACROS_BUTTONS_PER_SCREEN
    lv_obj_t** labels;  // length: MACROS_BUTTONS_PER_SCREEN
    lv_obj_t** icons;   // length: MACROS_BUTTONS_PER_SCREEN

    lv_obj_t* pieHitLayer;
    lv_obj_t** pieSegments; // length: 8
};

class IMacroPadLayout {
public:
    virtual ~IMacroPadLayout() = default;

    virtual const char* id() const = 0;

    // Apply button geometry/visibility and template-specific helpers.
    virtual void apply(MacroPadLayoutContext& ctx) const = 0;

    // Whether this template maps a macro config slot onto a UI target.
    virtual bool isSlotUsed(uint8_t slot) const = 0;

    // Optional: template uses pie hit-testing / ring segment visuals.
    virtual bool isPie() const { return false; }

    // Optional: hit-testing for templates that have non-rectangular targets.
    // Returns slot index, or -1 if no slot.
    virtual int slotFromPoint(int /*x*/, int /*y*/, const MacroPadLayoutContext& /*ctx*/) const { return -1; }
};

// Returns a layout singleton for a given template id.
// Unknown ids resolve to the default layout.
const IMacroPadLayout& layoutForId(const char* templateId);

} // namespace macropad_layout

#endif // MACROPAD_LAYOUT_H
