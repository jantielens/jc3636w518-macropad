#ifndef MACRO_TEMPLATES_H
#define MACRO_TEMPLATES_H

#include <Arduino.h>
#include <stdint.h>

// Firmware + web portal share these ids via /api/macros.
namespace macro_templates {

static constexpr const char* kTemplateRoundRing9 = "round_ring_9";
static constexpr const char* kTemplateRoundPie8 = "round_pie_8";
static constexpr const char* kTemplateStackSides5 = "round_stack_sides_5";
static constexpr const char* kTemplateWideSides3 = "round_wide_sides_3";
static constexpr const char* kTemplateSplitSides4 = "round_split_sides_4";

// Returns true if template id is recognized.
bool is_valid(const char* id);

// Returns a stable default template id.
const char* default_id();

// Returns the editor selector layout JSON fragment for a template id.
// Output is a JSON object string (no surrounding commas), e.g. {"columns":4,"cells":[...]}
// Returns nullptr if unknown.
const char* selector_layout_json(const char* id);

// Returns a user-facing name for the template.
const char* display_name(const char* id);

}

#endif // MACRO_TEMPLATES_H
