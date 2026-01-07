#include "macropad_layout.h"

#include "../macro_templates.h"

#include <string.h>

namespace macropad_layout {

// Forward declarations for layout singletons implemented in other translation units.
const IMacroPadLayout& layout_round9();
const IMacroPadLayout& layout_pie8();
const IMacroPadLayout& layout_five_stack();
const IMacroPadLayout& layout_wide_center();
const IMacroPadLayout& layout_four_split();

const IMacroPadLayout& layoutForId(const char* templateId) {
    const char* id = (templateId && templateId[0] != '\0') ? templateId : macro_templates::default_id();
    if (!macro_templates::is_valid(id)) {
        id = macro_templates::default_id();
    }

    if (strcmp(id, macro_templates::kTemplateStackSides5) == 0) return layout_five_stack();
    if (strcmp(id, macro_templates::kTemplateRoundPie8) == 0) return layout_pie8();
    if (strcmp(id, macro_templates::kTemplateWideSides3) == 0) return layout_wide_center();
    if (strcmp(id, macro_templates::kTemplateSplitSides4) == 0) return layout_four_split();

    // round_ring_9 default
    return layout_round9();
}

} // namespace macropad_layout
