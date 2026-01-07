#include "macro_templates.h"

#include <string.h>

namespace macro_templates {

bool is_valid(const char* id) {
    if (!id || !*id) return false;
    return strcmp(id, kTemplateRoundRing9) == 0
        || strcmp(id, kTemplateRoundPie8) == 0
        || strcmp(id, kTemplateStackSides5) == 0
        || strcmp(id, kTemplateWideSides3) == 0
        || strcmp(id, kTemplateSplitSides4) == 0;
}

const char* default_id() {
    return kTemplateRoundRing9;
}

const char* display_name(const char* id) {
    if (!id || !*id) return "(unknown)";
    if (strcmp(id, kTemplateRoundRing9) == 0) return "Round Ring (9)";
    if (strcmp(id, kTemplateRoundPie8) == 0) return "Round Pie (8 + Center)";
    if (strcmp(id, kTemplateStackSides5) == 0) return "Stack + Sides (5)";
    if (strcmp(id, kTemplateSplitSides4) == 0) return "Split Center + Sides (4)";
    if (strcmp(id, kTemplateWideSides3) == 0) return "Wide Center + Sides (3)";
    return "(unknown)";
}

const char* selector_layout_json(const char* id) {
    if (!id || !*id) return nullptr;

    // NOTE: This string is embedded verbatim into /api/macros.
    // Keep it small; the portal JS treats null/undefined cells as spacers.

    // Round 9: preserve the old 3x3 "ring" selector visualization.
    static constexpr const char* kRound9 =
        "{\"columns\":3,\"cells\":[7,0,1,6,8,2,5,4,3]}";

    // Five-stack: show a cross-like selector (top/center/bottom + left/right).
    // Layout (3x3):
    //   _  0  _
    //   3  1  4
    //   _  2  _
    static constexpr const char* kFiveStack =
        "{\"columns\":3,\"cells\":[null,0,null,3,1,4,null,2,null]}";

    // Wide-center: show a single-row 3-column selector.
    // Slots: 2 (left), 0 (center), 1 (right)
    static constexpr const char* kWideCenter =
        "{\"columns\":3,\"cells\":[2,0,1]}";

    // Four-split: based on five_stack, but the center middle becomes unused and
    // we keep only top + bottom in the center stack.
    // Layout (3x3):
    //   _  0  _
    //   3  _  4
    //   _  2  _
    static constexpr const char* kFourSplit =
        "{\"columns\":3,\"cells\":[null,0,null,3,null,4,null,2,null]}";

    if (strcmp(id, kTemplateRoundRing9) == 0) return kRound9;
    if (strcmp(id, kTemplateRoundPie8) == 0) return kRound9;
    if (strcmp(id, kTemplateStackSides5) == 0) return kFiveStack;
    if (strcmp(id, kTemplateWideSides3) == 0) return kWideCenter;
    if (strcmp(id, kTemplateSplitSides4) == 0) return kFourSplit;
    return nullptr;
}

} // namespace macro_templates
