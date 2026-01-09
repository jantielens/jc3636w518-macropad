# Memory Pressure Plan (Temporary)

**Context**: Internal heap is getting dangerously low (e.g. Min Free Heap ~1 KB). This is a stability risk (alloc failures, fragmentation, watchdog resets). Devices have **8MB PSRAM**, so large/long-lived allocations should preferentially go to PSRAM while keeping DMA/latency-critical buffers in internal RAM.

## Goals (measurable)

- Keep **internal** heap healthy under worst-case scenarios.
- Prevent fragmentation-driven failures.
- Shift eligible allocations to PSRAM without breaking DMA/timing constraints.

### Primary Metrics (record per scenario)

From `device_telemetry_get_memory_snapshot()` / `/api/health`:
- Internal: `heap_internal_free`, `heap_internal_min`, `heap_internal_largest`, `heap_fragmentation`
- Overall (Arduino): `heap_free`, `heap_min` (note: reflects internal heap)
- PSRAM: `psram_free`, `psram_min`, `psram_largest`, `psram_fragmentation`

### Acceptance Criteria (initial)

- `heap_internal_min` never drops below **100 KB** during worst-case flows.
- `heap_fragmentation` stays below **40%** under steady-state.
- No alloc failures or stability regressions in the test scenarios below.

## Goal (non-measurable)

- Make future enhancements **memory-efficient by default** (culture + guardrails), e.g.:
  - Add a short "memory guidelines" doc for contributors (what goes to PSRAM vs internal, how to avoid fragmentation, JSON/LVGL best practices).
  - Provide small helper abstractions (e.g. `psram_malloc_or_die()`, `make_json_doc_psram()`, reusable scratch buffers) so new code naturally does the right thing.
  - Add a lightweight PR checklist item: "What is the internal heap impact? Did you capture before/after snapshots for S1–S5?"

## Constraints / Notes

- `ESP.getFreeHeap()` / `ESP.getMinFreeHeap()` report **internal heap**, not PSRAM. Low values usually mean internal pressure (task stacks, fragmentation, library allocations).
- Some buffers must remain internal (DMA-capable, driver-specific, ISR/latency sensitive). Prefer `MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL` when required.
- Prefer PSRAM for large buffers, images, caches, JSON docs (when safe), icon payloads, etc.

## Plan

### 1) Baseline and scenario definition

- Establish baseline metrics at:
  - Boot end (after init)
  - WiFi connected
  - Web portal started
  - Idle steady-state (e.g. 60s)

### 2) Instrumentation improvements

- Add **per-task stack high-water** reporting (this often explains sudden low internal min heap).
- Add tagged memory snapshots at key lifecycle points:
  - setup/init milestones
  - MQTT connect/publish
  - web request handling hotspots
  - macro import/parse
  - LVGL screen transitions

### 3) Fixes / optimizations

Prioritize changes that reduce **internal** allocations and fragmentation.

**P0: Fix the worst measured cliffs first (S2/S4)**

- Macro POST path (`/api/macros`) internal cliff (S4)
  - Observed: internal min hit at `http_macros_post_saved` after already getting very low at `http_macros_post_parsed`.
  - Check/optimize:
    - Move ArduinoJson parsing allocations to PSRAM (custom allocator for `DynamicJsonDocument`, or PSRAM-backed arena).
    - Reduce peak JSON memory: smaller doc capacity, avoid parsing fields we don’t need, and avoid copying strings where possible.
    - Audit `macros_config_save()` for hidden internal allocations (FFat/NVS write path), and avoid temporary buffers in internal RAM.
  - Validate: re-run S4 and confirm `hin_min` improves and `frag_max` drops.

- Portal first-load cliff (`http_root`, S2)
  - Check/optimize:
    - Move embedded web assets storage to PSRAM (see issue #5) or ensure assets remain in flash without RAM copies.
    - Reduce per-request transient allocations and avoid large temporary buffers in Async request handlers.
    - If any compression/templating happens at runtime, ensure intermediate buffers are PSRAM-backed.
  - Validate: re-run S2 ×5 and compare `hin_min`/`frag_max`.

- LVGL screen transitions / UI object lifetime
  - Check/optimize:
    - Confirm all screen/UI allocations go through the LVGL heap (PSRAM-preferring) and avoid non-LVGL `malloc()` in screens.
    - Reduce peak during screen switches (lazy init, reuse widgets, reduce caches, avoid big image loads during transitions).
    - Revisit LVGL cache policy (image cache size, draw buffer strategy) per board.
  - Validate: re-run S4 and watch `lvgl_switch_pre_*`/`post_*` tags.

**P1: High ROI “known wins” (from earlier investigation)**

These come from https://github.com/jantielens/jc3636w518-macropad/issues/5 and are expected to recover substantial internal RAM with low risk:

- Disable BLE keyboard where not needed (`HAS_BLE_KEYBOARD false`) (often 40–80 KB internal).
- If BLE is required: move NimBLE allocations to PSRAM (often 40–60 KB internal).
- Move embedded web assets to PSRAM (often 10–20 KB internal).
- Pack large structs (e.g. config structs) if safe with NVS migration.

Validate: re-run S1 + S2 (and S4 if macros/UI involved).

**P2: Right-size stacks using telemetry (steady internal savings, low risk if careful)**

- AsyncTCP task stack (`CONFIG_ASYNC_TCP_STACK_SIZE`): reduce stepwise (e.g. 10240 → 8192) and re-test worst web flows.
- LVGL task stack: reduce stepwise only if stack watermark stays healthy under rapid screen switching.
- Avoid reducing stacks that already show very tight margins in tripwire dumps.

Validate: watch tripwire “worst stacks” plus long-run stability.

**P3: Fragmentation guardrails (keeps wins from regressing)**

- Prefer PSRAM for large/long-lived buffers and documents; reserve internal for DMA/latency-critical buffers.
- Reuse scratch buffers (especially for JSON/image operations) instead of repeated malloc/free.
- Avoid `String` growth patterns in hot paths; prefer fixed buffers or PSRAM-backed buffers.
- Where feasible, consider static allocation for long-lived tasks/queues to reduce heap churn.

### 4) Validate

- Re-run the exact same scenarios.
- Compare metrics and watch for regressions.

## Test Scenarios (run consistently)

- **S1 Boot + Idle**: boot → wait for readiness marker → capture first `hb` snapshot.
  - Note: “how idle” this is depends on `MEMORY_HEARTBEAT_INTERVAL_MS` (often overridden to 5s during testing).
- **S2 WiFi + Portal**: connect WiFi → start portal → load pages.
- **S6 Browser-like portal flow**: load `/` + assets → parallel `/api/*` burst → save macros + config (no reboot) → load `/network.html` + assets → parallel `/api/*` burst.
- **S3 Image API Stress**: upload max-size JPEG → decode/display → dismiss.
- **S4 Macro Config**: upload/POST macros JSON (worst-case size) and apply.
- **S5 MQTT**: connect + HA discovery publish + periodic health publish.

### Automation Harness

Use the serial + HTTP harness to make runs repeatable and agent-investigable:

#### Quick start

- List scenarios + examples:
  - `python3 tools/memory_test_harness.py`
- Run S1 (boot + idle):
  - `python3 tools/memory_test_harness.py s1 <ip> --port /dev/ttyACM0`
- Run S2 (portal page loads):
  - `python3 tools/memory_test_harness.py s2 <ip> --port /dev/ttyACM0`

Notes:
- The harness prefers the runtime IP it sees in serial (`Got IP: ...`). The `<ip>` argument is mainly used for reboot/HTTP when needed.
- Serial port can be auto-detected:
  - `--port auto` (default) tries `/dev/ttyACM0` then `/dev/ttyUSB0`.

#### Common flags

- Reduce measurement perturbation from HTTP:
  - `--no-health` skips `/api/health` calls entirely (recommended for “pure serial” runs).
  - `--no-pages` skips `GET /`, `GET /network.html`, `GET /firmware.html` during S2.
- Control reboot behavior:
  - Default behavior is to request `POST /api/reboot` (fire-and-forget) for a clean run.
  - `--no-reboot` skips reboot.
- Manual interaction checkpoints:
  - `--pause after_ip,after_get_/network.html` (also supports `after_get_/`, `after_get_/firmware.html`, or `all`).
  - `--non-interactive` disables prompts (useful for scripted runs).
- Debug visibility:
  - `--echo-serial` mirrors serial to stdout (serial is always captured to artifacts).

#### Compare runs (regression check)

Compare two artifact directories (based on `mem.jsonl`):

- `python3 tools/memory_test_harness.py --compare artifacts/memory-tests/<runA> artifacts/memory-tests/<runB>`

This prints per-tag deltas (e.g. `boot`, `setup`, `http_*`, `hb`) and overall `hin_min` delta.

Artifacts are written to `artifacts/memory-tests/<timestamp>_<scenario>/`:
- `serial.log` raw serial capture
- `mem.jsonl` parsed `[Mem]` snapshots
- `health.jsonl` captured `/api/health` payloads
- `events.jsonl` structured events (HTTP + IP detection + `[Mem]` + TRIPWIRE)
- `tasks.jsonl` parsed stack/telemetry lines (task stack margins + AsyncTCP watermark)
- `summary.json` quick derived mins/maxes for easy triage

The harness also writes `summary.md` with:
- A derived-metrics section (mins/maxes across the run)
- A Tripwire section (when it fired) including the tag + threshold and the worst stack margins
- A Panic section (when detected) with the first crash marker line from `serial.log`
- A per-tag table of all captured `[Mem]` snapshots to quickly spot which tag caused the cliff

## Replay Set (portable change series)

If we need to re-apply the measurement tooling + initial fixes on another branch, these commits are intended to be replayed together (in order):

- `5fbb6d2` docs: add memory pressure plan and Step 3 priorities
- `a746e64` tools: extend memory test harness (s4/s5) and ignore artifacts
- `66e48be` telemetry: add tagged heap snapshots and tripwire across subsystems
- `638d5e8` portal: tag macros POST and use PSRAM allocator for JSON

Cherry-pick onto another branch:

- `git checkout <target-branch>`
- `git cherry-pick 5fbb6d2 a746e64 66e48be 638d5e8`

Or export/apply as patches:

- `git format-patch main..HEAD`
- `git checkout <target-branch> && git am *.patch`

## Experiments Log (what we tried + results)

> Fill one row per change. Always note board + scenario.

| Date | Branch/Commit | Board | Change | Scenario(s) | Artifacts | `hin_min` (B) | `frag_max` (%) | PSRAM free min (B) | Tripwire | Notes |
|------|---------------|-------|--------|-------------|----------|---------------|----------------|--------------------|----------|-------|
| 2026-01-09 | issue-14-mqtt-send-action@2fb151e | jc3636w518 | Baseline batch (harness `--no-health`) | S1 (boot/setup/hb) | `20260109_111535_s1` | 20912 | 37 | 8271944 | not fired | Canonical “before” for Step 3 after harness + tripwire fixes |
| 2026-01-09 | issue-14-mqtt-send-action@2fb151e | jc3636w518 | Baseline batch (harness `--no-health`) | S2 (portal load) ×5 | `20260109_111554_s2`..`20260109_111714_s2` | 20772–20912 (avg 20884) | 37–42 (avg 41) | 8266852 | fired 5/5 @ `http_root` (threshold 30720B, `hin` 21532–21672B) | Consistently tight stacks: `ipc0` 76B, `ipc1` 84B. Largest cliff occurs at first portal page load (`http_root`). |
| 2026-01-09 | issue-14-mqtt-send-action@fb61e02 | jc3636w518 | NimBLE host alloc to PSRAM via `CONFIG_BT_NIMBLE_MEM_ALLOC_MODE_EXTERNAL` + sdkconfig shim + build define propagation | S2 (portal load) | `20260109_142954_s2` | 42916 | 42 | (see artifacts) | not fired | Confirmed: commenting out either the shim or the define propagation regresses S2 badly (tripwire / sub-30KB mins). |
| 2026-01-09 | issue-14-mqtt-send-action@2fb151e | jc3636w518 | Validate new Step 2 tags (macros + LVGL switches) | S4 (macros POST/apply) | `20260109_113140_s4` | 1832 | 60 | 8201840 | fired @ `lvgl_switch_pre_macro1` (threshold 30720B, `hin` 16116B) | Worst-case internal min observed at `http_macros_post_saved` (min dropped between `http_macros_post_parsed` → `http_macros_post_saved` and never recovered). |
| 2026-01-09 | issue-14-mqtt-send-action@2fb151e | jc3636w518 | Validate Step 2 tags (LVGL switches; MQTT best-effort) | S5 (MQTT connect/publish) | `20260109_113216_s5` | 20752 | 37 | 8270072 | fired @ `lvgl_switch_pre_macro1` (threshold 30720B, `hin` 25040B) | No `mqtt_*` tags observed (device reported MQTT host not configured). LVGL switch tags captured. |
| 2026-01-09 | issue-14-mqtt-send-action@2fb151e | jc3636w518 | Step 3: PSRAM allocator for `/api/macros` ArduinoJson doc | S4 (macros POST/apply) | `20260109_114958_s4` | 16060 | 67 | 8179772 | fired @ `lvgl_switch_pre_macro1` (threshold 30720B, `hin` 16060B) | Major improvement: `http_macros_post_parsed` no longer collapses internal free heap (~30KB free vs ~10KB before). Remaining cliff is dominated by LVGL switch. |
| 2026-01-09 | issue-14-mqtt-send-action@fb61e02 (dirty) | jc3636w518 | Fix MacroPadScreen 2× mask cache eviction (avoid leaks + in-use descriptor invalidation) | S4 (macros POST/apply) | `20260109_144659_s4` | 31668 | 42 | 8158544 | not fired | Compare vs `20260109_114958_s4`: Δ`hin_min` +15608B overall; `lvgl_switch_pre_macro1` Δhin +31696B; LVGL switch no longer dominates the worst-case min (floor is now steady-state `hb`). |
| 2026-01-09 | issue-14-mqtt-send-action@9268821 (dirty) | jc3636w518 | Prefer PSRAM for LVGL draw buffer + ESP_Panel swap buffer | S4 (macros POST/apply) | `20260109_150318_s4` | 55964 | 59 | 8133844 | not fired | Confirmed in serial: LVGL draw buffer + ST77916 swapBuf both allocated in PSRAM. Compare vs `20260109_144659_s4`: Δ`hin_min` +24296B. |
| 2026-01-09 | issue-14-mqtt-send-action@578e1b1 (dirty) | jc3636w518 | Add S6 “browser-like” multi-phase flow (home load + save + network load) | S6 | `20260109_162517_s6` | 32112 | 58 | 8126756 | not fired | Reproduced crash: `Stack canary watchpoint triggered (async_tcp)` during `/api/icons/gc` after macros apply. Root cause: large on-stack keep-set in icon GC handler. |
| 2026-01-09 | issue-14-mqtt-send-action@578e1b1 (dirty) | jc3636w518 | Fix `/api/icons/gc` stack overflow by removing keep-set (scan macros on-demand) | S6 | `20260109_163934_s6` | 30732 | 58 | 8133196 | not fired | Crash resolved under S6. Harness now also flags panics in `summary.json`/`summary.md` (see commits `e99df8b`, `e7a5d5e`). |

### Historical notes (not directly comparable)

Older rows captured with different instrumentation/timing and should not be used for before/after comparisons:

| Date | Branch/Commit | Board | Change | Scenario(s) | Notes |
|------|---------------|-------|--------|-------------|-------|
| 2026-01-09 | issue-14-mqtt-send-action | jc3636w518 | Baseline (no changes) | S1/S2/S3 (early runs) | Earlier logs showed internal mins down to ~1–2KB during portal/image flows; keep as “symptom evidence”, not a regression baseline. |

## Candidate Hotspots (to investigate)

- LVGL: draw buffers, image widgets, transforms, caches.
- Async web server / AsyncTCP task stacks and buffers.
- JSON parsing (large `DynamicJsonDocument` in request paths).
- Image decode paths (JPEG work buffers, intermediate pixel buffers).
- Icon store/cache memory policy.
- Macro parsing/execution buffers.

## Firmware Instrumentation Notes

- Low-memory tripwire (serial-only): when internal min heap drops below `MEMORY_TRIPWIRE_INTERNAL_MIN_BYTES`, the firmware logs a marker line starting with `TRIPWIRE fired` and then dumps task stack margins as `[Task] name=... stack_rem=...B`.
  - The harness parses this into `events.jsonl` (`type="tripwire"`) and includes a compact summary under `derived.tripwire` in `summary.json` (and a “Tripwire” section in `summary.md`).
- Configure thresholds in `src/app/board_config.h` (or per-board overrides).

## Step 2 Instrumentation (status check)

What we planned in Step 2 vs what’s currently present in the firmware:

- Per-task stack high-water reporting
  - Implemented as: a one-shot full task watermark dump when the low-memory tripwire fires (plus an AsyncTCP stack watermark log once).
  - Not yet: periodic stack dump (only happens on tripwire).

- Tagged memory snapshots at key lifecycle points
  - Setup/init milestones: `boot`, `setup`, periodic `hb` snapshots (implemented in `src/app/app.ino`).
  - Web request hotspots: `http_root`, `http_network`, `http_firmware` (implemented in `src/app/web_portal.cpp`, emitted from main loop to avoid Async callback perturbation).
  - Image API hotspots: many `img ...`, `strip ...`, `urlimg ...` tags (implemented in `src/app/image_api.cpp`).
  - MQTT connect/publish/discovery hotspots: `mqtt_connect_attempt`, `mqtt_connected`, `mqtt_discovery_pre/post`, `mqtt_first_publish` (implemented in `src/app/mqtt_manager.cpp`).
  - Macro import/parse/apply hotspots: `http_macros_post_*` tags (implemented in `src/app/web_portal.cpp`).
  - LVGL screen transitions: `lvgl_switch_pre_*` / `lvgl_switch_post_*` (implemented in `src/app/display_manager.cpp`).
