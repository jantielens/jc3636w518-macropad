#include "web_portal_auth.h"

#include <ESPAsyncWebServer.h>

#include "board_config.h"
#include "config_manager.h"
#include "log_manager.h"
#include "web_portal_state.h"

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

static bool portal_auth_required() {
    if (web_portal_state().ap_mode_active) return false;
    if (!web_portal_state().config) return false;
    return web_portal_state().config->basic_auth_enabled;
}

bool portal_auth_gate(AsyncWebServerRequest* request) {
    // AsyncWebServer handlers execute on the AsyncTCP task.
    // Log stack margin once so we can safely tune CONFIG_ASYNC_TCP_STACK_SIZE.
    static bool logged_async_stack = false;
    if (!logged_async_stack) {
        const UBaseType_t remaining_words = uxTaskGetStackHighWaterMark(nullptr);
        const uint32_t unit_bytes = (uint32_t)sizeof(StackType_t);
        const uint32_t remaining_bytes = (uint32_t)remaining_words * unit_bytes;

        const char* taskName = pcTaskGetName(nullptr);
        Logger.logMessagef(
            "Portal",
            "AsyncTCP stack watermark: task=%s rem=%u units (%u B), unit=%u B, CONFIG_ASYNC_TCP_STACK_SIZE(raw)=%u",
            taskName ? taskName : "(null)",
            (unsigned)remaining_words,
            (unsigned)remaining_bytes,
            (unsigned)unit_bytes,
            (unsigned)CONFIG_ASYNC_TCP_STACK_SIZE);
        logged_async_stack = true;
    }

    if (!portal_auth_required()) return true;

    const char* user = web_portal_state().config->basic_auth_username;
    const char* pass = web_portal_state().config->basic_auth_password;

    if (request->authenticate(user, pass)) {
        return true;
    }

    request->requestAuthentication(PROJECT_DISPLAY_NAME);
    return false;
}
