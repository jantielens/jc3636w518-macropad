/**
 * @file lv_conf.h
 * LVGL configuration for JC3636W518 (ESP32-S3 + ST77916)
 */

/* clang-format off */
#if 1 /*Set to "1" to enable content*/

#ifndef LV_CONF_H
#define LV_CONF_H

#include <stdint.h>

// Enable simple include path so lvgl can find this file
#define LV_CONF_INCLUDE_SIMPLE 1

/*====================
   COLOR SETTINGS
 *====================*/

#define LV_COLOR_DEPTH 16
#define LV_COLOR_16_SWAP 1
#define LV_COLOR_SCREEN_TRANSP 0
#define LV_COLOR_MIX_ROUND_OFS (LV_COLOR_DEPTH == 32 ? 0: 128)
#define LV_COLOR_CHROMA_KEY lv_color_hex(0x00ff00)

/*=========================
   MEMORY SETTINGS
 *=========================*/

#define LV_MEM_CUSTOM 0
#if LV_MEM_CUSTOM == 0
    // LVGL heap (bytes). Keep modest to fit internal RAM; we also allocate draw buffers separately.
    #define LV_MEM_SIZE (48U * 1024U)
    #define LV_MEM_ADR 0
#else
    #define LV_MEM_CUSTOM_INCLUDE <stdlib.h>
    #define LV_MEM_CUSTOM_ALLOC   malloc
    #define LV_MEM_CUSTOM_FREE    free
    #define LV_MEM_CUSTOM_REALLOC realloc
#endif

#define LV_MEM_BUF_MAX_NUM 16
#define LV_MEMCPY_MEMSET_STD 0

/*====================
   HAL SETTINGS
 *====================*/

#define LV_DISP_DEF_REFR_PERIOD 30      /*[ms]*/ /* Reduced from 15ms to ease SPI queue pressure */
#define LV_INDEV_DEF_READ_PERIOD 30     /*[ms]*/

/* Gesture configuration */
#define LV_INDEV_DEF_GESTURE_LIMIT      80      /* Minimum pixels to detect gesture */
#define LV_INDEV_DEF_GESTURE_MIN_VELOCITY 5     /* Minimum velocity for swipe */

#define LV_TICK_CUSTOM 1
#if LV_TICK_CUSTOM
    #define LV_TICK_CUSTOM_INCLUDE "Arduino.h"
    #define LV_TICK_CUSTOM_SYS_TIME_EXPR (millis())
#endif

#define LV_DPI_DEF 130

/*=======================
 * FEATURE CONFIGURATION
 *=======================*/

#define LV_DRAW_COMPLEX 1
#if LV_DRAW_COMPLEX != 0
    #define LV_SHADOW_CACHE_SIZE 0
    #define LV_CIRCLE_CACHE_SIZE 4
#endif

#define LV_IMG_CACHE_DEF_SIZE   0
#define LV_GRADIENT_MAX_STOPS       2
#define LV_GRAD_CACHE_DEF_SIZE      0
#define LV_DITHER_GRADIENT      0
#define LV_DISP_ROT_MAX_BUF (10*1024)

/* GPU */
#define LV_USE_GPU_STM32_DMA2D 0
#define LV_USE_GPU_NXP_PXP 0
#define LV_USE_GPU_NXP_VG_LITE 0
#define LV_USE_GPU_SDL 0

/* Logging */
#define LV_USE_LOG 0

/* Asserts */
#define LV_USE_ASSERT_NULL          1
#define LV_USE_ASSERT_MALLOC        1
#define LV_USE_ASSERT_STYLE         0
#define LV_USE_ASSERT_MEM_INTEGRITY 0
#define LV_USE_ASSERT_OBJ           0
#define LV_ASSERT_HANDLER_INCLUDE <stdint.h>
#define LV_ASSERT_HANDLER while(1);

/* Performance monitor */
#define LV_USE_PERF_MONITOR 1
#if LV_USE_PERF_MONITOR
    #define LV_USE_PERF_MONITOR_POS LV_ALIGN_BOTTOM_RIGHT
#endif

#define LV_USE_MEM_MONITOR 0
#define LV_USE_REFR_DEBUG 0

#define LV_SPRINTF_CUSTOM 0
#define LV_SPRINTF_USE_FLOAT 0
#define LV_USE_USER_DATA 1
#define LV_ENABLE_GC 0

/*=====================
 *  COMPILER SETTINGS
 *====================*/
#define LV_BIG_ENDIAN_SYSTEM 0
#define LV_ATTRIBUTE_TICK_INC
#define LV_ATTRIBUTE_TIMER_HANDLER
#define LV_ATTRIBUTE_FLUSH_READY
#define LV_ATTRIBUTE_MEM_ALIGN_SIZE 1
#define LV_ATTRIBUTE_MEM_ALIGN
#define LV_ATTRIBUTE_LARGE_CONST
#define LV_ATTRIBUTE_LARGE_RAM_ARRAY
#define LV_ATTRIBUTE_FAST_MEM
#define LV_ATTRIBUTE_DMA
#define LV_EXPORT_CONST_INT(int_value) struct _silence_gcc_warning
#define LV_USE_LARGE_COORD 0

/*==================
 *   FONT USAGE
 *===================*/
#define LV_FONT_MONTSERRAT_8  0
#define LV_FONT_MONTSERRAT_10 0
#define LV_FONT_MONTSERRAT_12 0
#define LV_FONT_MONTSERRAT_14 1
#define LV_FONT_MONTSERRAT_16 1
#define LV_FONT_MONTSERRAT_18 0
#define LV_FONT_MONTSERRAT_20 0
#define LV_FONT_MONTSERRAT_22 1
#define LV_FONT_MONTSERRAT_24 0
#define LV_FONT_MONTSERRAT_26 0
#define LV_FONT_MONTSERRAT_28 0
#define LV_FONT_MONTSERRAT_30 0
#define LV_FONT_MONTSERRAT_32 1
#define LV_FONT_MONTSERRAT_34 0
#define LV_FONT_MONTSERRAT_36 0
#define LV_FONT_MONTSERRAT_38 0
#define LV_FONT_MONTSERRAT_40 0
#define LV_FONT_MONTSERRAT_42 0
#define LV_FONT_MONTSERRAT_44 0
#define LV_FONT_MONTSERRAT_46 0
#define LV_FONT_MONTSERRAT_48 0

#define LV_FONT_MONTSERRAT_12_SUBPX      0
#define LV_FONT_MONTSERRAT_28_COMPRESSED 0
#define LV_FONT_DEJAVU_16_PERSIAN_HEBREW 0
#define LV_FONT_SIMSUN_16_CJK            0
#define LV_FONT_UNSCII_8  0
#define LV_FONT_UNSCII_16 0
#define LV_FONT_CUSTOM_DECLARE
#define LV_FONT_DEFAULT &lv_font_montserrat_14
#define LV_FONT_FMT_TXT_LARGE 0
#define LV_USE_FONT_COMPRESSED 0
#define LV_USE_FONT_SUBPX 0

/*=================
 *  TEXT SETTINGS
 *=================*/
#define LV_TXT_ENC LV_TXT_ENC_UTF8
#define LV_TXT_BREAK_CHARS " ,.;:-_"
#define LV_TXT_LINE_BREAK_LONG_LEN 0
#define LV_TXT_COLOR_CMD "#"
#define LV_USE_BIDI 0
#define LV_USE_ARABIC_PERSIAN_CHARS 0

/*===================
 *  WIDGET USAGE
 *==================*/
#define LV_USE_OBJ_ID 0
#define LV_USE_ARC        1
#define LV_USE_BAR        1
#define LV_USE_BTN        1
#define LV_USE_BTNMATRIX  1
#define LV_USE_CANVAS     1
#define LV_USE_CHECKBOX   1
#define LV_USE_DROPDOWN   1
#define LV_USE_IMG        1
#define LV_USE_LABEL      1
#define LV_USE_LINE       1
#define LV_USE_ROLLER     1
#define LV_USE_SLIDER     1
#define LV_USE_SWITCH     1
#define LV_USE_TEXTAREA   1
#define LV_USE_TABLE      1
#define LV_USE_MENU       1
#define LV_USE_MSGBOX     1
#define LV_USE_SPAN       1
#define LV_SPAN_SNIPPET_STACK_SIZE 64
#define LV_USE_SPINNER    1
#define LV_USE_SPINBOX    1
#define LV_USE_LIST       1
#define LV_USE_CHART      1
#define LV_USE_LED        1
#define LV_USE_TILEVIEW   1
#define LV_USE_TABVIEW    1
#define LV_USE_WIN        1
#define LV_USE_CALENDAR   1
#define LV_USE_COLORWHEEL 1
#define LV_USE_IMGBTN     1
#define LV_USE_KEYBOARD   1
#define LV_USE_LABEL      1

#define LV_USE_ANIMATION 1
#define LV_USE_FLEX 1
#define LV_USE_GRID 1

/* THEME */
#define LV_USE_THEME_DEFAULT 1
#if LV_USE_THEME_DEFAULT
    #define LV_THEME_DEFAULT_DARK 0
    #define LV_THEME_DEFAULT_GROW 0
    #define LV_THEME_DEFAULT_TRANSITION_TIME 80
#endif

#define LV_USE_THEME_BASIC 0
#define LV_USE_THEME_MONO  0

/* Layouts */
#define LV_USE_LAYOUTS 1

/*==================
 *  OTHERS
 *==================*/
#define LV_USE_MSG 0
#define LV_USE_FS_IF 0
#define LV_USE_PNG 0
#define LV_USE_BMP 0
#define LV_USE_QRCODE 0
#define LV_USE_GIF 0
#define LV_USE_SJPEG 0
#define LV_USE_FREETYPE 0
#define LV_USE_SNAPSHOT 0

#endif /* LV_CONF_H */
#endif /*Enable content*/
/* clang-format on */
