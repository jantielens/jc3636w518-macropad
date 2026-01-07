// ============================================================================
// Screen Compilation Unit
// ============================================================================
// Single compilation unit for all screen implementations.
// This ensures screen .cpp files are compiled with correct build flags
// while keeping them organized in the screens/ subdirectory.
//
// Note: Display driver .cpp files are included in display_manager.cpp,
// not here, since the display manager is responsible for driver lifecycle.

#include "board_config.h"

#if HAS_DISPLAY

// Include all screen implementations
#include "screens/splash_screen.cpp"
#include "screens/info_screen.cpp"
#include "screens/test_screen.cpp"

// Macro pad screens (8 fixed screens)
#include "screens/macropad_screen.cpp"

// MacroPad layout implementations (one-per-template)
#include "screens/macropad_layout_factory.cpp"
#include "screens/macropad_layout_round9.cpp"
#include "screens/macropad_layout_pie8.cpp"
#include "screens/macropad_layout_five_stack.cpp"
#include "screens/macropad_layout_wide_center.cpp"
#include "screens/macropad_layout_four_split.cpp"

#if HAS_IMAGE_API
#include "screens/direct_image_screen.cpp"
#include "screens/lvgl_image_screen.cpp"
#endif

#endif // HAS_DISPLAY

// ============================================================================
// Touch Driver Implementations
// ============================================================================
// Touch driver implementations are compiled via src/app/touch_drivers.cpp.
