#pragma once

#include <stddef.h>

struct DeviceConfig;

struct WebPortalState {
    bool ap_mode_active = false;
    DeviceConfig* config = nullptr;

    // OTA (web upload or background update) progress snapshot.
    bool ota_in_progress = false;
    size_t ota_progress = 0;
    size_t ota_total = 0;
};

WebPortalState& web_portal_state();
