# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

---

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
