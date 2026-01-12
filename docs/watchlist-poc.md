# Watchlist Screen (POC)

This document describes the proof-of-concept (POC) “watchlist” screen implemented for the `jc3636w518-macropad` firmware.

The screen shows **3 configurable slots** (stocks and/or crypto) and refreshes periodically.

## Goals

- Provide a single “dashboard” screen with 3 slots:
  - Slot 1 is the hero (bigger)
  - Slots 2–3 are secondary
- Support both **stocks** and **crypto** in the same UI.
- Keep configuration simple:
  - Stock is the default.
  - Crypto requires a `crypto:` prefix.
- No timestamps on-screen.
- “Fancy dashboard” behavior: price color turns **green/red** based on the latest tick direction (new vs previous refresh).

## Data Providers (no API keys)

- **Stocks**: Stooq CSV endpoint
- **Crypto**: CoinGecko `simple/price` endpoint

## Configuration

Configuration lives in the Web Portal (Home page) and is persisted via NVS.

### Slot syntax

- Stock (default): `MSFT`
- Stock (explicit): `stock:MSFT`
- Stock (full Stooq symbol): `AAPL.US` (if you already know the suffix)
- Crypto: `crypto:BTC`

Stock symbols:
- If the symbol contains a dot (`.`), it’s used as-is (e.g. `MSFT.US`).
- Otherwise, the implementation defaults to `.US` (e.g. `MSFT` → `MSFT.US`).

### Refresh interval

- `watchlist_refresh_seconds` default: `60`
- Clamped at runtime to `15–3600` seconds

## Where the Code Lives

- Screen implementation:
  - [src/app/screens/watchlist_screen.h](../src/app/screens/watchlist_screen.h)
  - [src/app/screens/watchlist_screen.cpp](../src/app/screens/watchlist_screen.cpp)
- Screen integration/registration (screen list / routing):
  - [src/app/screens.cpp](../src/app/screens.cpp)
  - Display manager integration (screen creation + id `"watchlist"`):
    - [src/app/display_manager.cpp](../src/app/display_manager.cpp)
    - [src/app/display_manager.h](../src/app/display_manager.h)
- Configuration persistence:
  - [src/app/config_manager.h](../src/app/config_manager.h)
  - [src/app/config_manager.cpp](../src/app/config_manager.cpp)
- Web portal config UI:
  - [src/app/web/home.html](../src/app/web/home.html)
  - [src/app/web/portal.js](../src/app/web/portal.js)
- Config API (`/api/config`) wiring:
  - [src/app/api_config.cpp](../src/app/api_config.cpp)

## UI Structure

- Slot 1 (hero): symbol + larger price.
- Divider line.
- Slots 2 and 3: symbol + price.

Price color:
- Green: last refresh ticked up
- Red: last refresh ticked down
- Neutral gray: unknown / unchanged / error

## How to Build / Flash

Build:

- `./build.sh jc3636w518`

Flash:

- `./upload.sh jc3636w518`

## Memory Notes

The watchlist periodically performs HTTPS requests (TLS) which can cause **internal heap pressure spikes** even when plenty of PSRAM is free.

Mitigations included in this PoC:
- Watchlist UI updates are throttled and only update LVGL labels when values actually change (reduces display flush pressure).
- The Watchlist fetch task uses a PSRAM-backed stack when available; its FreeRTOS TCB is kept in internal RAM (required by FreeRTOS).

## Known Limitations / Next Improvements

- Crypto symbol resolution is intentionally simple (small hardcoded symbol → CoinGecko id mapping).
- Price formatting is fixed at 2 decimals for now (crypto would benefit from adaptive decimals).
- Remaining internal heap min dips may still occur during TLS-heavy operations; next candidates to review are other network subsystems (AsyncTCP, MQTT, BLE) and large task stacks.
