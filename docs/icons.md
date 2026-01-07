# Icon System (Macro Buttons)

This project supports a compiled icon library for macro buttons, designed to work across multiple screen templates and button geometries.

## Goals

- Use **one canonical icon set** across many layouts (round grids, tall side rails, etc.).
- Keep storage reasonable: **mono “mask” icons** are stored compactly; **color icons** are optional.
- Allow the web portal to discover icons dynamically from firmware.

## Asset Types

### 1) Mask icons (mono)

- Stored as **alpha-only** images (`LV_IMG_CF_ALPHA_8BIT`).
- Recolored at runtime via LVGL style (`img_recolor`), so one asset can be tinted.
- Source PNGs live in:
  - `assets/icons_mono/*.png`

### 2) Color icons (optional)

- Stored as **true color + alpha** (`LV_IMG_CF_TRUE_COLOR_ALPHA`).
- Rendered without recolor.
- Source PNGs live in:
  - `assets/icons_color/*.png`

All icon source PNGs are standardized as **64×64**.

## Build Pipeline

The build script generates LVGL C arrays and a registry when the target board enables icons.

- Generator: `tools/png2lvgl_assets.py`
  - Mono: emits `LV_IMG_CF_ALPHA_8BIT`
  - Color: emits `LV_IMG_CF_TRUE_COLOR_ALPHA`
- Registry generator: `tools/generate_icon_registry.py`

Outputs (auto-generated, not committed):

- `src/app/icon_assets_mono.{h,cpp}`
- `src/app/icon_assets_color.{h,cpp}`
- `src/app/icon_registry.{h,cpp}`

Board gating:

- Icons are only built/compiled when the target board override sets `#define HAS_ICONS true`.

## Runtime Lookup

Firmware resolves the configured `icon_id` (string) to an LVGL image descriptor through the generated registry:

- `icon_registry_lookup(icon_id, &ref)` → `ref.dsc` + `ref.kind`

`IconKind`:

- `Mask`: alpha-only mask icon (tinted by style)
- `Color`: true-color icon (no recolor)

Icon IDs are normalized in the UI path (lowercase, `-` → `_`, whitespace trimmed) to match registry IDs.

## UI Rendering (MacroPadScreen)

Where the behavior lives:

- Rendering + layout: `src/app/screens/macropad_screen.cpp`
- Template definitions: `src/app/macro_templates.*`

Macro buttons optionally include an `lv_img` child.

- If an icon is configured and found in the registry, the `lv_img` is shown and styled:
  - `Mask`: `img_recolor_opa = 255` (tinted)
  - `Color`: recolor disabled

### Layout Rules

- Icon-only buttons are centered.
- Icon+label buttons show the label **directly under the icon** (grouped visually).
- Very tall/narrow buttons (e.g. side rails) avoid making icons too small by preferring width.

#### How icon sizing is decided

The layout code computes an `iconBox` target size based on the button geometry:

- Compute base dimension from button size.
  - For tall/narrow buttons, it prefers **width** (otherwise `min(w,h)` makes icons too small).
- For icon-only buttons: `iconBox` ≈ 75% of base dimension.
- For icon+label buttons: `iconBox` ≈ 85% of base dimension.
- Clamp range is capped at 128px to support a fixed 2× step.

Fixed-step scaling behavior:

- Downscaling is continuous (e.g. 0.6×, 0.8×) to make icons fit.
- Upscaling is snapped to exactly **2×** only when `iconBox` reaches its maximum.

### Fixed-step “2×” enlargement without flash bloat

LVGL v8’s `lv_img_set_zoom()` can **not** transform alpha-only images (`LV_IMG_CF_ALPHA_*`).

To support a crisp “2×” look for mask icons without storing 128×128 assets in flash:

- Mask icons remain stored in flash as **64×64 alpha-only**.
- When the layout wants a 2× icon, firmware generates a **128×128 alpha-only** version *on the fly* (2×2 pixel replication) and swaps the `lv_img` source to the cached 2× descriptor.
- A small fixed cache is used to keep RAM/PSRAM usage bounded.

Memory behavior:

- The 2× mask buffers are allocated via `lv_mem_alloc()`.
- This project’s LVGL heap (`src/app/lvgl_heap.cpp`) **prefers PSRAM** when available, otherwise falls back to internal RAM.

## Adding New Templates (Checklist)

When you add a new macro screen template, these are the key things to keep icon rendering predictable:

1. Decide which macro slots are used
  - The macro config and renderer use **slot indexes** (0..N-1). Your template must clearly define which slots are visible.
  - Update slot visibility logic so unused slots are hidden (prevents “ghost buttons”).

2. Implement the button geometry
  - Add a `layoutButtons...()` function or extend `layoutButtons()` to handle your template.
  - Set each used button’s position and size (`lv_obj_set_pos`, `lv_obj_set_size`).
  - Ensure label width is updated for the button width (wrapping depends on it).

3. Let the shared icon/label layout run
  - Don’t hand-position icon/label in the template layout function.
  - The shared `updateButtonLayout()` is called during refresh to place icon+label consistently.

4. Watch out for tall/narrow rails
  - Very narrow buttons can make thin-stroke icons look invisible if downscaled too far.
  - The shared layout includes a special-case to avoid shrinking icons below 1× on 64px-wide rails.

5. Know the mask zoom limitation
  - Alpha-only mask icons can’t use `lv_img_set_zoom()`.
  - If your template wants 2× icons, ensure the button geometry can reasonably allocate the 128px target; otherwise it will stay at 1× (or downscale).

## Template Example (Minimal)

This is a minimal “new template” path. It’s intentionally small: the template layout function only sizes/positions buttons, and then the shared icon/label logic handles alignment.

### 1) Define a template ID

Add a new template ID in `src/app/macro_templates.*` (naming is illustrative):

```cpp
// macro_templates.h
namespace macro_templates {
  extern const char* kTemplateFoo2;
}

// macro_templates.cpp
namespace macro_templates {
  const char* kTemplateFoo2 = "foo_2";
}
```

Also ensure your template is treated as valid by `macro_templates::is_valid()` and optionally included in any UI list / metadata.

### 2) Hook it into MacroPadScreen

In `src/app/screens/macropad_screen.cpp`, extend `layoutButtons()` to call your layout function:

```cpp
if (strcmp(tpl, macro_templates::kTemplateFoo2) == 0) {
    layoutButtonsFoo2();
}
```

### 3) Implement the layout function

Example: two buttons side-by-side (slots 0 and 1). Everything else is hidden.

```cpp
void MacroPadScreen::layoutButtonsFoo2() {
    const int w = displayMgr->getActiveWidth();
    const int h = displayMgr->getActiveHeight();

    const int spacing = 6;
    const int pad = 8;

    const int btnW = (w - (2 * pad) - spacing) / 2;
    const int btnH = h - (2 * pad);

    for (int i = 0; i < MACROS_BUTTONS_PER_SCREEN; i++) {
        if (!buttons[i]) continue;
        const bool used = (i == 0 || i == 1);
        if (used) lv_obj_clear_flag(buttons[i], LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(buttons[i], LV_OBJ_FLAG_HIDDEN);
    }

    // Slot 0: left
    lv_obj_set_pos(buttons[0], pad, pad);
    lv_obj_set_size(buttons[0], btnW, btnH);
    lv_obj_set_width(labels[0], btnW - 8);

    // Slot 1: right
    lv_obj_set_pos(buttons[1], pad + btnW + spacing, pad);
    lv_obj_set_size(buttons[1], btnW, btnH);
    lv_obj_set_width(labels[1], btnW - 8);
}
```

Notes:

- Don’t manually position `icons[i]` or `labels[i]` beyond setting label width; the shared refresh path will call `updateButtonLayout()` after it decides whether each slot has an icon/label.
- Always hide unused slots to avoid stale visuals.

## Web Portal Integration

Firmware exposes the compiled icon IDs so the portal can populate UI pickers:

- `GET /api/icons` returns the list of available icons from the compiled registry.

The portal uses this list to provide autocomplete / selection when editing macros.

## Practical Notes / Limits

- Switching all mask icons to `true_color_alpha` would allow LVGL zoom directly, but increases storage ~3× for mono assets.
- Making the default icon size 128×128 (alpha-only) increases storage ~4× and can exceed the 3MB app partition on some targets.
- The current approach preserves flash size while enabling crisp 2× mask icons when needed (at a bounded PSRAM/RAM cost).
