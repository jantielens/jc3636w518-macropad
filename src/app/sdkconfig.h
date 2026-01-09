#pragma once

// Shim header to override ESP-IDF sdkconfig defaults for Arduino library builds.
//
// NimBLE-Arduino's allocator selection in exp_nimble_mem.c uses `#ifdef` checks.
// The ESP32 core's sdkconfig typically defines:
//   CONFIG_BT_NIMBLE_MEM_ALLOC_MODE_INTERNAL=y
// which forces internal heap allocation even if we pass
//   -DCONFIG_BT_NIMBLE_MEM_ALLOC_MODE_EXTERNAL=1
//
// Because the Arduino build puts `src/app` early in the include path, this file
// is included first. We then include the *real* sdkconfig.h and adjust defines.

#if defined(__has_include_next)
  #if __has_include_next("sdkconfig.h")
    #include_next "sdkconfig.h"
  #else
    #include "sdkconfig.h"
  #endif
#else
  #include "sdkconfig.h"
#endif

// If external (PSRAM) allocation mode is enabled, ensure INTERNAL mode doesn't
// remain defined, otherwise NimBLE-Arduino's `#ifdef CONFIG_..._INTERNAL` wins.
#if defined(CONFIG_BT_NIMBLE_MEM_ALLOC_MODE_EXTERNAL) && (CONFIG_BT_NIMBLE_MEM_ALLOC_MODE_EXTERNAL)
  #ifdef CONFIG_BT_NIMBLE_MEM_ALLOC_MODE_INTERNAL
    #undef CONFIG_BT_NIMBLE_MEM_ALLOC_MODE_INTERNAL
  #endif
#endif
