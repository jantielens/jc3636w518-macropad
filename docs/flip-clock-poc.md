# Flip Clock Screen (POC)

This document describes the proof-of-concept (POC) “flip clock” screen implemented for the `jc3636w518-macropad` firmware.

## Goals

- Provide a dedicated clock screen with a mechanical “split-flap” / flip-clock feel.
- Display `HHMMSS` (no colons) and animate digit changes with a 2‑phase flip.
- Keep configuration minimal for the POC:
  - No web portal settings.
  - Time is UTC (NTP).

## Constraints

- LVGL version: 8.4 (Arduino environment).
- Built-in font availability is limited (e.g., `lv_font_montserrat_56` was not available in this build), so the POC uses `lv_font_montserrat_48`.
- LVGL transform zoom on labels proved unreliable in this build (digits disappeared), so transform scaling was removed for correctness.

## Where the Code Lives

- Screen implementation:
  - [src/app/screens/clock_screen.h](../src/app/screens/clock_screen.h)
  - [src/app/screens/clock_screen.cpp](../src/app/screens/clock_screen.cpp)
- Screen integration/registration (screen list / routing):
  - [src/app/screens.cpp](../src/app/screens.cpp)
  - Display manager integration (screen creation + id `"clock"`):
    - [src/app/display_manager.cpp](../src/app/display_manager.cpp)
    - [src/app/display_manager.h](../src/app/display_manager.h)
- NTP / UTC time init hook (POC):
  - [src/app/app.ino](../src/app/app.ino)

## UI Structure

Each digit is a small LVGL object tree (“card”):

- `root`: the full digit card (background material, shadow, rounded corners)
- `topHalf`: transparent clipping container for the top half of the digit
- `bottomHalf`: transparent clipping container for the bottom half of the digit
- `hinge`: thick black hinge bar at the split line
- `rivets`: small dots near the hinge (subtle physical detail)
- Two static labels:
  - `topLabel` (inside `topHalf`)
  - `bottomLabel` (inside `bottomHalf`)
- Two animated flap overlays (hidden unless animating):
  - `topFlap` + `topFlapLabel`
  - `bottomFlap` + `bottomFlapLabel`

### Half-clipping approach

The POC avoids rendering “two stacked full digits” by placing a single glyph so that:

- `topLabel` is positioned within the card, but only the top half is visible because it’s parented to `topHalf`.
- `bottomLabel` is positioned with a negative Y offset in `bottomHalf`, so only the lower portion of the same glyph is visible.

This makes the digit appear as one split character rather than two separate lines.

## Flip Animation

Digit changes use a 2‑phase animation:

1. **Top flap closes**
   - `topFlap` animates height from `halfH` → `0`.
   - Flap background darkens slightly as it closes.
   - Flap label opacity fades (quadratic curve) for a physical feel.

2. **Bottom flap opens**
   - After a short pause, `bottomFlap` animates height from `0` → `halfH`.
   - Flap label opacity fades in.
   - Flap background brightens slightly as it opens.

The static `topLabel` / `bottomLabel` are updated at the phase boundary to “commit” the new digit.

Animation timing constants (as of this POC) are defined near the top of the implementation file.

## Time Source (UTC)

- The screen renders placeholders (`------`) until time is considered valid.
- Validity check: epoch must be after 2020-01-01.
- When valid, the screen displays `HHMMSS` derived from `localtime_r()`.
- UTC mode is enabled via `configTzTime("UTC0", ...)` (called after WiFi comes up).

## Burn-in / Retention Mitigation

The firmware already contains an inactivity-based screen saver that fades the backlight. In addition, the clock screen includes a small **anti burn-in drift**:

- The clock container is shifted by a few pixels periodically (max ±3 px) once time is valid.
- Drift interval: 15 seconds.
- Deterministic drift pattern based on current time (no random dependency).

This reduces the chance of long-term static content retention if the clock is left on.

## Current Visual Style

- Background material is applied to the card `root` as a subtle vertical gradient.
- `topHalf` / `bottomHalf` are transparent (clipping only) to avoid visible “three section” banding.
- Hinge is black and thicker than the original POC.

## How to Build / Flash

Build:

- `./build.sh jc3636w518`

Flash:

- `./upload.sh jc3636w518`

## Known Limitations / Next Improvements

- Bigger digits without resizing cards:
  - Best path: generate a digits-only LVGL font (0–9 and `-`) at a larger size and use it for this screen.
- Optional: add clock-specific dimming after prolonged display-on time (independent of global screen saver).
- Optional: add a subtle specular highlight sweep on the flap edge during flips for more realism.
