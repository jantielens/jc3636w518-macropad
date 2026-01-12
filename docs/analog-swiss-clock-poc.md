# Analog Clock Screen (Swiss Railway Style) — POC

This document describes the proof-of-concept (POC) Swiss railway-style analog clock screen for the `jc3636w518-macropad` firmware.

## Goals

- Add a full-screen analog clock that fills a 360×360 round display.
- Match the Swiss railway aesthetic closely:
  - White dial.
  - Minute tick marks.
  - Thick hour marks.
  - Black hour/minute hands.
  - Red second hand with the “lollipop” near the tip.
- Smooth second hand motion.
- POC simplicity:
  - No web portal configuration.
  - UTC time (NTP).

## Constraints

- LVGL version: 8.4 (Arduino environment).
- Keep allocations and per-frame work minimal.
- Avoid relying on LVGL label transform scaling (it was unreliable in this build).

## Where the Code Lives

- Screen implementation:
  - [src/app/screens/analog_clock_screen.h](../src/app/screens/analog_clock_screen.h)
  - [src/app/screens/analog_clock_screen.cpp](../src/app/screens/analog_clock_screen.cpp)
- Screen compilation unit:
  - [src/app/screens.cpp](../src/app/screens.cpp)
- Screen registry / navigation:
  - Registered in DisplayManager as `id: "analog_clock"`:
    - [src/app/display_manager.cpp](../src/app/display_manager.cpp)
    - [src/app/display_manager.h](../src/app/display_manager.h)
- Time source / UTC NTP hook (shared with flip clock POC):
  - [src/app/app.ino](../src/app/app.ino)

## Rendering Approach

The dial is drawn using a single LVGL object with a custom draw-event handler:

- One full-screen object (`dial`) is created.
- A draw callback (`LV_EVENT_DRAW_MAIN`) renders:
  - The dial face (filled circle).
  - 60 tick marks (minute ticks + thicker hour ticks).
  - Hour, minute, and second hands.
  - Center hub.

This approach avoids assembling the clock from many child objects and keeps the visuals crisp at full-screen size.

## Time Handling (UTC)

- The screen uses the same time validity check as the flip clock:
  - time is valid when epoch > 2020-01-01.
- For smooth hand motion, the screen maintains a stable `time()` + `millis()` reference:
  - `baseEpoch` + `baseMillis` are updated when NTP causes jumps.
  - Seconds are computed using integer math (`baseEpoch + elapsedMs/1000`) and a fractional part from millis (`(elapsedMs%1000)/1000`).
    - This avoids `float` precision loss at Unix epoch magnitudes (which can make the second hand appear “stuck”).

If time is not valid yet, the clock renders a “neutral” state (hands drawn in a dimmer color).

## Burn-in / Retention Mitigation

The firmware already supports inactivity-based backlight dimming via the screen saver manager.

This screen also implements a subtle anti burn-in drift:

- The dial’s center is shifted by a few pixels periodically (max ±3 px).
- Drift interval: 15 seconds.
- Deterministic drift pattern based on wall-clock time.

The dial radius includes a small margin so the drift does not clip important content.

## Screen ID

- `analog_clock`

## How to Build / Flash

Build:

- `./build.sh jc3636w518`

Flash:

- `./upload.sh jc3636w518`

## Known Limitations / Next Improvements

- Add optional elements for closer fidelity if desired:
  - Slight outer ring/border.
  - More authentic hand geometry (rectangular hand bodies, tapered tips).
  - Subtle shadow under hands.
- Optional: timezone support (portal setting) once POC is proven.
