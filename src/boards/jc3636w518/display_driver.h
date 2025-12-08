#ifndef JC3636W518_DISPLAY_DRIVER_H
#define JC3636W518_DISPLAY_DRIVER_H

#include <lvgl.h>
#include <stdint.h>

void board_display_init();
void board_display_loop();

// Backlight/power helpers
void display_backlight_off();
void display_backlight_on(uint8_t brightness_percent = 100);
void display_backlight_set_brightness(uint8_t percent); // clamp 0-100
uint8_t display_backlight_get_brightness();
bool display_backlight_is_on();

// Panel power helpers
void display_panel_off();
void display_panel_on();

#endif // JC3636W518_DISPLAY_DRIVER_H
