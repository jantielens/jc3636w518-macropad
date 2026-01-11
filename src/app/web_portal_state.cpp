#include "web_portal_state.h"

static WebPortalState g_state;

WebPortalState& web_portal_state() {
    return g_state;
}
