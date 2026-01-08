# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

---

## [1.3.0] - 2026-01-08

### Added
- Macro button action: Go to a specific screen (dropdown-driven).
- Macro button action: Back (previous screen) with safe fallback to `macro1`.

### Changed
- BREAKING: `/api/macros` schema replaces `script` with `payload` and adds actions `nav_to` + `go_back`.

## [1.2.0] - 2026-01-08

### Added
- Macro button icons with scalable rendering.
- Portal Macros editor support for configurable colors (screen/button backgrounds, icon tint, label color).
- New macro layout template: `round_pie_8`.
- FFat-backed icon store for on-demand installed icons (emoji + user icons) with `/api/icons/installed` and `/api/icons/install`.
- Automatic garbage collection of unused installed icons after saving macros (`POST /api/icons/gc`).

### Changed
- Refactor MacroPad templates into layout classes.
- Replace free-text icon id input with a portal icon picker (Mono icons vs Emoji tabs).
- Drop compiled color icon pipeline; keep mono compiled icons + FFat-installed emoji.

### Fixed
- Fix boot crash when USB CDC is not connected.
- Fix installed emoji detection (route registration order + correct JSON output).
- Improve button press feedback and enforce a minimum hold time.

## [1.1.0] - 2026-01-06

### Added
- Add macro screen templates (per-screen layout selection) with multiple built-in round-screen layouts.
- Expose templates via `/api/macros` for the web editor (including selector layout hints).

### Changed
- Bump macros config version and expand stored template id length for standardized template IDs.

### Fixed
- Fix template selection being ignored (always falling back to the default template) due to stored template id truncation.
- Avoid a brief default grid flash in the web portal Macros editor while `/api/macros` is loading.

## [1.0.3] - 2026-01-06

### Added
- Add a portal-driven screen switching stress test script (`tools/screen_stress_test.py`) to quickly reproduce/validate long-run display stability.
- Show a portal hint/empty-state message on empty macro screens at boot.

### Changed
- Improve the web portal Macros editor UX.
- Rotate the Macros Editor button grid so button #1 is at 12 o'clock.

### Fixed
- Mitigate intermittent display hangs during rapid screen switching on `jc3636w518` by preferring internal RAM for the LVGL draw buffer and using an internal/DMA-capable swap buffer.
- Make ESP_Panel flush synchronous by waiting for DMA transfer completion before returning to LVGL.

## [1.0.2] - 2026-01-06

### Fixed
- Fix GitHub Pages / ESP Web Tools installer flashing by writing bootloader + partition table + boot_app0 + app at explicit offsets (instead of flashing a merged image at offset 0).

### Added
- Add a minimal partition table parser (`tools/parse_esp32_partitions.py`) so the installer can derive the correct `app0` offset from `*.partitions.bin`.
- Package `boot_app0.bin` in release assets and rehydrate it in the Pages deploy workflow.

## [1.0.1] - 2026-01-06

### Fixed
- Defer splash status updates when LVGL is busy, instead of dropping them.
- Harden `/api/macros` upload gating to avoid concurrent upload races.

## [1.0.0] - 2024-06-26

---

## Template for Future Releases

```markdown
## [X.Y.Z] - YYYY-MM-DD

### Added
- New features

### Changed
- Changes to existing features

### Deprecated
- Features marked for removal

### Removed
- Removed features

### Fixed
- Bug fixes

### Security
- Security patches
```
