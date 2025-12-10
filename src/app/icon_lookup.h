/*
 * Icon Lookup Helper
 * Maps icon names to UTF-8 encoded Material Design Icons glyphs
 */

#ifndef ICON_LOOKUP_H
#define ICON_LOOKUP_H

#include <Arduino.h>

/**
 * Look up icon by name and return UTF-8 encoded glyph string
 * 
 * @param icon_name Icon name (e.g., "play", "content-copy", "home")
 * @return UTF-8 encoded icon string, or nullptr if icon not found
 * 
 * Example usage:
 *   const char* icon = icon_lookup("play");
 *   if (icon) {
 *     lv_label_set_text(label, icon);
 *   }
 */
const char* icon_lookup(const char* icon_name);

#endif // ICON_LOOKUP_H
