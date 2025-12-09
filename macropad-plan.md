# Plan to implement Macropad screens

## Requirements

### Screen Management
- Maximum 10 macropad screens (no reordering needed - users configure in desired order)
- Home screen displays device status/stats (not a configurable macropad screen)
- Navigation: prev/next buttons cycle through home screen → macropad screens
- Screens without any configured buttons are automatically hidden from navigation
- No disable/enable toggle - if a macropad has no buttons, it doesn't appear

### BLE Keyboard
- Firmware exposes a BLE keyboard using PROJECT_DISPLAY_NAME from config.sh as device name
- See /sample directory for implementation reference (BleKeyboard.h/cpp)
- All key types supported: regular keys, modifiers, function keys, media keys

### Button Configuration
- Each macropad screen uses a template (defines layout and max number of buttons)
- Each button can have:
  - Label: icon name OR text label
  - Keystrokes: stored as a string (format TBD in spike)
- Button interaction: single tap only (no press-and-hold, no multi-touch)
- Max number of buttons per template determined during layout spike

### Keystrokes Definition
- Keystrokes stored as strings (simple format, not structured data)
- Must support:
  - Simple single characters (e.g., 'a')
  - Multiple characters (e.g., 'abc')
  - Shortcuts (e.g., 'Ctrl+C')
  - Shortcut combinations (e.g., 'Ctrl+A then Ctrl+C')
  - Media keys (volume, play/pause, etc.)
- String format and parsing to be defined in spike

### Layout Templates
- 2 templates provided:
  - Radial layout: buttons arranged in a circle
  - Grid layout: buttons arranged in a grid (accounting for round screen)
- Exact layout specifications, button counts, and dimensions to be defined in spike

### Configuration Interface
- Web portal only (no on-device configuration UI)
- Users enter keystrokes as strings (no complex keystrokes builder UI for now)
- REST API endpoints for CRUD operations on macropad screens

## Spikes

### Keystrokes String Format

**Goal**: Define a simple string format for storing keystrokes that supports all required use cases.

**Requirements**:
- Simple single characters: 'a', 'b', '1', etc.
- Multiple characters: 'abc', 'hello world'
- Modifiers: Ctrl, Shift, Alt, Win/Cmd
- Function keys: F1-F24
- Special keys: Enter, Tab, Backspace, Arrow keys, etc.
- Media keys: Volume Up/Down, Play/Pause, Next/Previous track, etc.
- Sequences: Multiple shortcuts with optional delays (e.g., "Ctrl+A then wait 100ms then Ctrl+C")

**Investigate**:
- Existing keystrokes notation standards (AutoHotkey, xdotool, web standards)
- How to handle timing between sequence steps
- Parser implementation complexity vs flexibility tradeoff
- Error handling for invalid keystrokes strings
- Terminology for the feature (using "keystrokes" to reflect multiple key actions)

**Example formats to evaluate**:
- `[CTRL][A],[WAIT:100],[CTRL][C]`
- `Ctrl+A {wait 100} Ctrl+C`
- `<ctrl>a</ctrl> <wait>100</wait> <ctrl>c</ctrl>`

#### Spike results

**Research on Existing Standards:**

1. **AutoHotkey (AHK)**: Uses format like `^c` (Ctrl+C), `+a` (Shift+A), `!t` (Alt+T), `#r` (Win+R)
   - Pros: Very compact, widely recognized
   - Cons: Cryptic symbols, hard for non-technical users

2. **Web/JavaScript KeyboardEvent.key**: Uses full names like `Control`, `Shift`, `ArrowUp`
   - Pros: Clear, unambiguous, self-documenting
   - Cons: Verbose for complex sequences

3. **xdotool/Linux**: Uses formats like `ctrl+c`, `shift+alt+t`
   - Pros: Human-readable, intuitive
   - Cons: No standard for sequences/delays

4. **VS Code keybindings**: Uses `ctrl+k ctrl+s` for chord sequences
   - Pros: Natural representation of key chords
   - Cons: Spaces can be ambiguous

**BLE Keyboard API Analysis:**

From BleKeyboard.h/.cpp, the library provides:
- `write(uint8_t c)` - Single character (automatic press+release)
- `write(const uint8_t *buffer, size_t size)` - String of characters
- `press(uint8_t k)` + `release(uint8_t k)` - Manual key control
- `press(MediaKeyReport k)` + `release(MediaKeyReport k)` - Media key control
- Supports modifiers: Ctrl, Shift, Alt, Gui (Win/Cmd)
- Supports special keys: F1-F24, arrows, Enter, Tab, Esc, etc.
- Supports media keys: Volume, Play/Pause, Next/Previous, Mute, etc.

**Recommended Format:**

Use a **hybrid comma-separated format** that balances readability and ease of parsing:

**Syntax:**
```
<action>[,<action>[,<action>...]]
```

**Action Types:**

1. **Plain text**: `hello` → types "hello" character by character
2. **Single special key**: `{Enter}`, `{Tab}`, `{Esc}`, `{F1}` → named keys in braces
3. **Arrow keys**: `{Up}`, `{Down}`, `{Left}`, `{Right}`
4. **Modifier + key**: `Ctrl+C`, `Shift+Alt+T`, `Cmd+Space` → modifier names with plus
5. **Key sequence with delay**: `Ctrl+A,{Wait:100},Ctrl+C` → comma-separated with explicit wait
6. **Media keys**: `{VolumeUp}`, `{VolumeDown}`, `{PlayPause}`, `{Mute}`, `{NextTrack}`, `{PrevTrack}`

**Key Names:**

Modifiers: `Ctrl`, `Shift`, `Alt`, `Cmd` (or `Win`)
Special: `Enter`, `Tab`, `Esc`, `Backspace`, `Delete`, `Insert`, `Home`, `End`, `PageUp`, `PageDown`, `CapsLock`
Function: `F1` through `F24`
Arrows: `{Up}`, `{Down}`, `{Left}`, `{Right}`
Media: `{VolumeUp}`, `{VolumeDown}`, `{Mute}`, `{PlayPause}`, `{NextTrack}`, `{PrevTrack}`, `{Stop}`, `{MediaHome}`, `{Calculator}`, `{Mail}`

**Examples:**

```
a                           → types 'a'
hello world                 → types 'hello world'
{Enter}                     → press Enter
Ctrl+C                      → press Ctrl+C
ctrl+c                      → same as Ctrl+C (case-insensitive)
Ctrl+A,Ctrl+C               → press Ctrl+A, then Ctrl+C (no delay)
ctrl+a,ctrl+c               → same as above (case-insensitive)
Ctrl+A,{Wait:100},Ctrl+C    → press Ctrl+A, wait 100ms, then Ctrl+C
ctrl+a,{wait:100},ctrl+c    → same as above (case-insensitive)
Shift+Alt+F                 → press Shift+Alt+F simultaneously
shift+alt+f                 → same as above (case-insensitive)
{F5}                        → press F5
{f5}                        → same as F5 (case-insensitive)
Cmd+Space,term,{Enter}      → Cmd+Space, type 'term', press Enter (Mac Spotlight example)
cmd+space,term,{enter}      → same as above (case-insensitive, 'term' case preserved)
{VolumeUp}                  → increase volume
{volumeup}                  → same as above (case-insensitive)
{PlayPause}                 → toggle play/pause
Hello World                 → types 'Hello World' (plain text case preserved)
```

**Parser Implementation:**

1. Split string by commas (`,`) to get action list
2. For each action:
   - Normalize to lowercase for comparison (except plain text content)
   - If contains `{wait:N}` → delay N milliseconds
   - If contains `{...}` → special/media key lookup (case-insensitive)
   - If contains `+` → parse modifiers and key (case-insensitive), call `press()` for all, then `release()` all
   - Otherwise → plain text, call `write(buffer, size)` (preserves original case)
3. Between actions, add small default delay (e.g., 50ms) for reliability

**Case Sensitivity:**

- **Key names are case-insensitive**: `Ctrl+C`, `ctrl+c`, `CTRL+C` all work the same
- **Plain text preserves case**: `Hello` types "Hello", not "hello"
- **Parsing logic**: Convert key names to lowercase for lookup, but preserve original case for text content
- Examples:
  - `ctrl+c` = `Ctrl+C` = `CTRL+C` → all send Ctrl+C
  - `{enter}` = `{Enter}` = `{ENTER}` → all press Enter
  - `{volumeup}` = `{VolumeUp}` = `{VOLUMEUP}` → all increase volume
  - `Hello` types "Hello" (case preserved for plain text)

**Error Handling:**

- Unknown key names → log error, skip action (don't crash)
- Malformed syntax → log error, attempt best-effort parsing
- Invalid wait values → use default 100ms

**Terminology:**

Use **"keystrokes"** (plural) as the primary term:
- Reflects that we can send multiple key actions
- Simple and widely understood
- Matches common usage ("keyboard shortcuts", "keystroke sequences")
- Alternative terms in documentation: "keyboard actions", "key combos", "macros"

**Implementation Complexity:**

- **Low**: String parsing with basic state machine
- **Parser size**: ~500 lines including key name lookup tables
- **Memory**: Minimal (parse on-demand, no pre-compilation needed)
- **Validation**: Can be done in web UI (JavaScript) and on device (C++)

**Trade-offs:**

✅ **Chosen Format Advantages:**
- Human-readable and self-documenting
- Easy to enter in a text field (no complex UI needed)
- Case-insensitive key names reduce user errors
- Plain text content preserves original case (types exactly what user intends)
- Supports all required use cases
- Familiar to users of other automation tools
- Easy to parse with simple string operations

❌ **Limitations:**
- Longer than pure binary format (but config is stored as string anyway)
- Requires key name lookup table (all lowercase variants)
- No validation until runtime (but web UI can provide real-time validation)

### Icons

**Goal**: Determine the best approach for button icons on the 360x360 round display.

**Requirements**:
- Users specify icons by name (stored as string in config)
- Icons must render cleanly at button size (TBD in layout spike)
- Icons should cover common use cases: media controls, applications, actions, symbols
- Must work within ESP32 flash/memory constraints

**Investigate**:
- Icon font approach (e.g., Material Icons font file embedded in firmware)
  - Pros: small size, scalable, easy to reference by name
  - Cons: limited to one font family, requires font rendering
- Embedded PNG/bitmap library
  - Pros: full color, wide variety
  - Cons: larger flash usage, fixed sizes
- Google Material Icons (https://fonts.google.com/icons)
  - Can we embed a subset of commonly used icons?
  - PNG generation at build time?
- LVGL built-in icon support
  - Does LVGL provide icon solutions?
  - Can we use LVGL image/font features?
- Fallback behavior: what to display if icon name not found? (show text label instead?)

**Deliverables**:
- Recommended icon approach
- List of available icons (or link to icon set)
- Icon naming convention
- Implementation notes for rendering icons on buttons

#### Spike results

**Recommended Approach: LVGL Custom Icon Font**

Use LVGL's native font system with Material Design Icons subset embedded at build time.

**Technical Solution:**

1. **Icon Source**: Material Design Icons (Pictogrammers)
   - 7200+ icons available
   - Font file: `materialdesignicons-webfont.ttf` from [GitHub](https://github.com/Templarian/MaterialDesign-Webfont)
   - License: Apache 2.0 / Pictogrammers Free License (commercial-friendly)

2. **Build-time Font Generation**:
   - Tool: LVGL Online Font Converter (https://lvgl.io/tools/fontconverter)
   - Input: Material Design Icons TTF file
   - Settings:
     - Font name: `material_icons_48` (or button size)
     - Size: 48px (matches button dimensions from layout spike)
     - BPP: 4 (good quality/size tradeoff)
     - Compression: Enabled
     - Range: Unicode codepoints for selected icons (100-200 icons)
   - Output: C source file with embedded font bitmap

3. **Icon Selection (100-200 common icons)**:
   - Media controls: play, pause, stop, volume_up, volume_down, mute, etc.
   - Editing: copy, paste, cut, undo, redo, save
   - Navigation: arrow_up, arrow_down, arrow_left, arrow_right, home, back
   - Applications: calculator, camera, phone, email, calendar, settings
   - System: wifi, bluetooth, battery, power, refresh
   - Full list curated during implementation

4. **Icon Name Mapping**:
   - Create `icons.json` mapping file:
     ```json
     {
       "copy": "0xF0192",
       "paste": "0xF0193",
       "volume_up": "0xF057E"
     }
     ```
   - Auto-generate `material_icons.h` with UTF-8 defines:
     ```cpp
     #define ICON_COPY "\xF3\xB0\x86\x92"
     #define ICON_PASTE "\xF3\xB0\x86\x93"
     extern lv_font_t material_icons_48;
     ```

5. **User Workflow**:
   - User enters icon name in web UI: `"copy"`
   - Firmware looks up define: `ICON_COPY`
   - Renders using LVGL:
     ```cpp
     lv_obj_set_style_text_font(label, &material_icons_48, 0);
     lv_label_set_text(label, ICON_COPY);
     ```

**Memory Requirements:**

- **Flash**: ~100-200KB (stored in 16MB Flash with PROGMEM)
  - 100 icons compressed: ~100KB
  - 200 icons compressed: ~150-200KB
- **RAM**: < 1KB persistent (font descriptor in 512KB internal SRAM)
- **Temporary render buffer**: 1-2KB during icon rendering (from PSRAM)

**Advantages:**

✅ Native LVGL solution (well-documented, standard approach)
✅ Excellent memory efficiency (compressed bitmaps in Flash)
✅ Fast rendering (vector-based, GPU-optimized by LVGL)
✅ Scalable to any size (regenerate font at different sizes)
✅ No runtime dependencies (no filesystem, no PNG decoder, no internet)
✅ Professional icon library (Material Design)
✅ Simple user experience (type icon name as string)
✅ Consistent rendering quality across all icons
✅ Build-time validation (missing icons cause compile errors)

**Implementation Steps:**

1. **Create build tool**: `tools/build-icon-font.sh`
   - Downloads Material Design Icons TTF
   - Reads icon list from `icons.json`
   - Invokes LVGL font converter
   - Generates `material_icons_48.c` and `material_icons.h`
   - Integrates into Arduino build process

2. **Icon name parser**: Function to map user string to icon define
   - Input: `"copy"` (from config)
   - Output: `ICON_COPY` (UTF-8 string)
   - Fallback: Show text label if icon not found

3. **Extensibility**: Document how to regenerate with different icons
   - Edit `icons.json` to add/remove icons
   - Run `./tools/build-icon-font.sh`
   - Rebuild firmware

**Alternatives Considered and Rejected:**

❌ **User-uploaded PNGs**: 
- More complex (filesystem, PNG decoder)
- Larger memory footprint (600KB vs 200KB)
- Slower rendering
- More failure points

❌ **On-demand download from internet**:
- Pictogrammers doesn't provide PNG API (only SVG)
- Slow boot time (1-2s per icon × 30 icons = 30-60s)
- Requires internet connection at boot
- Unreliable (API dependency)

❌ **Built-in LVGL symbols only**:
- Only ~80 symbols available
- Limited selection (no customization)

**Fallback Strategy:**

If icon name not found in font:
1. Check LVGL built-in symbols (LV_SYMBOL_*)
2. If still not found, display text label instead
3. Log warning for debugging

### Layouts

**Goal**: Define precise layouts for both templates on the 360x360 round display.

**Display Specs**:
- Resolution: 360x360 pixels
- Shape: circular (corners not visible)
- Touch: single-touch capacitive

**Templates to Define**:

1. **Radial Layout**
   - Center button (size TBD)
   - Outer ring buttons arranged in circle (quantity TBD)
   - Button spacing/padding
   - Label placement (inside button or below?)
   - Maximum number of buttons: TBD

2. **Grid Layout**
   - Grid pattern that fits within circular display
   - Button size and spacing
   - How many rows/columns?
   - Handle round screen clipping (buttons at corners may be cut off)
   - Maximum number of buttons: TBD

**Navigation**:
- Fixed prev/next buttons at bottom of every screen
- Navigation cycles through: Home screen → Macropad 1 → Macropad 2 → ... → Macropad N → Home screen
- Navigation button size, position, and appearance
- Visual indication of current screen position (e.g., page dots)?

**Button Visual Design**:
- Normal state appearance
- Pressed state feedback (highlight, animation)
- Disabled state appearance (if needed)
- Icon size within button
- Text label font size, truncation

**Investigate**:
- LVGL button widget capabilities and constraints
- Touch target minimum size for usability
- Label length limits (how many characters fit?)
- Performance: how many LVGL objects can we efficiently render?

**Deliverables**:
- Mockup images or diagrams for each template
- Exact button positions, sizes, and spacing
- Maximum button count per template
- Navigation button specifications
- Implementation notes for LVGL rendering

#### Spike results

**Display Specifications:**
- Resolution: 360×360 pixels
- Shape: Circular (corners cut off beyond ~330px diagonal)
- Touch: Single-touch capacitive
- Usable area: Full 360px diameter circle

**Button Design Standards:**
- Minimum touch target: 50×50px (based on UX guidelines)
- Recommended button size: 70-80px for comfortable tapping
- Button spacing: 10-20px between buttons
- Touch feedback: Visual highlight on press (LVGL button press state)

---

### **Template 1: Radial Layout**

**Description**: Circular arrangement with center button surrounded by outer ring.

**Layout:**
```
        ┌─────────────────┐
        │    [Btn 0]      │  Button 0 (top, 0°)
  [Btn 7]       ╱ ╲  [Btn 1]  Buttons 1-7 around circle
        │   ╱  CTR  ╲    │
  [Btn 6] │  [CENTER] │ [Btn 2]  Center button
        │   ╲       ╱    │
  [Btn 5]       ╲ ╱  [Btn 3]
        │    [Btn 4]      │  Button 4 (bottom, 180°)
        └─────────────────┘
```

**Button Specifications:**
- **Center button** (position 0):
  - Size: 80×80px
  - Position: (180, 180) - screen center
  - Ideal for primary/most-used action
  
- **Outer ring buttons** (positions 1-8):
  - Size: 70×70px
  - Radius from center: 130px
  - Angular positions:
    ```
    Position 1: 0°   (top)
    Position 2: 45°  (top-right)
    Position 3: 90°  (right)
    Position 4: 135° (bottom-right)
    Position 5: 180° (bottom)
    Position 6: 225° (bottom-left)
    Position 7: 270° (left)
    Position 8: 315° (top-left)
    ```
  - Button center coordinates: `(180 + 130*cos(θ), 180 + 130*sin(θ))`

**Total buttons**: 9 (1 center + 8 outer)

**Best for**: 
- Primary action workflows (center = main action)
- Media controls (center = play/pause, outer = volume/skip/etc)
- Quick access tools (center = most frequent, outer = related actions)

---

### **Template 2: Grid Layout (Diamond Pattern)**

**Description**: Diamond/rhombus arrangement (1-2-3-2-1 rows) optimized for round display.

**Layout:**
```
        ┌─────────────────┐
        │                 │
        │      [1]        │  Row 1: 1 button
        │                 │
        │   [2]   [3]     │  Row 2: 2 buttons
        │                 │
        │ [4]  [5]  [6]   │  Row 3: 3 buttons (widest)
        │                 │
        │   [7]   [8]     │  Row 4: 2 buttons
        │                 │
        │      [9]        │  Row 5: 1 button
        └─────────────────┘
```

**Button Specifications:**
- Button size: 80×80px (uniform)
- Vertical spacing: 20px between rows
- Horizontal spacing: 20px between buttons in same row

**Row positions** (from top):
- **Row 1** (y=50): 1 button
  - Position 1: x=180 (centered)
  
- **Row 2** (y=130): 2 buttons
  - Position 2: x=130
  - Position 3: x=230
  
- **Row 3** (y=210): 3 buttons (widest row at vertical center)
  - Position 4: x=80
  - Position 5: x=180
  - Position 6: x=280
  
- **Row 4** (y=290): 2 buttons
  - Position 7: x=130
  - Position 8: x=230
  
- **Row 5** (y=370 or hidden): 1 button
  - Position 9: Could be placed at y=340 if needed, or omitted from typical use

**Note**: Row 5 (position 9) is at the very bottom edge. Depending on button size, may want to adjust y-coordinates to fit better within circle.

**Alternative tighter spacing** (all 9 buttons visible):
- Reduce button size to 70×70px
- Reduce spacing to 15px
- Shift entire grid up by 10-20px

**Total buttons**: 9 (arranged in diamond)

**Best for**:
- Balanced layouts (no dominant button)
- Familiar grid-like navigation
- Screens where all actions have equal importance
- Better use of horizontal space

---

### **Navigation Design: Flexible Per-Screen**

**Approach**: No fixed navigation area. Any button can be configured for navigation.

**Navigation Action Types:**
```cpp
enum MacroActionType {
    ACTION_KEYSTROKES = 0,  // Send keystrokes via BLE keyboard
    ACTION_NAV_HOME = 1,    // Navigate to home screen
    ACTION_NAV_NEXT = 2,    // Navigate to next macropad screen
    ACTION_NAV_PREV = 3,    // Navigate to previous macropad screen
    ACTION_NAV_GOTO = 4     // Navigate to specific screen (by ID)
};
```

**User Benefits:**
- ✅ Full flexibility - place nav buttons anywhere (or nowhere)
- ✅ All 9 buttons available for any purpose
- ✅ Screen-specific navigation patterns
- ✅ No wasted space on screens that don't need navigation

**Example Navigation Patterns:**

1. **Traditional bottom nav** (radial):
   ```
   Positions 0-6: Actions
   Position 7: Home button
   Position 8: Next screen button
   ```

2. **Corner nav** (grid):
   ```
   Position 1: Home (top)
   Positions 2-8: Actions
   Position 9: Next (bottom)
   ```

3. **No navigation** (dedicated screen):
   ```
   All 9 positions: Actions
   User power-cycles or uses web portal to change screens
   ```

4. **Home screen** (launcher):
   ```
   All 9 positions: Navigation to other screens
   ```

**Safety net**: Web portal always accessible via WiFi to reconfigure screens if user gets stuck.

---

### **Button Visual Design:**

**Normal state:**
- Background: Dark gray (#2C2C2C)
- Border: 2px, light gray (#555555)
- Border radius: 12px (rounded corners)
- Icon/label: White (#FFFFFF)
- Icon size: 48×48px (centered)
- Label font: 14-16px below icon (if both icon and label present)

**Pressed state:**
- Background: Accent color (#667EEA or similar)
- Border: 2px, lighter accent
- Icon/label: White
- Visual feedback: Immediate on touch

**Disabled state** (if needed):
- Background: Very dark gray (#1A1A1A)
- Icon/label: Dark gray (#444444)
- Border: None or very subtle

**Navigation buttons** (optional visual distinction):
- Could use icon-only (no text label)
- Could use different color scheme (e.g., blue tint)
- Or keep same style as action buttons (user choice)

---

### **LVGL Implementation Notes:**

**Creating buttons:**
```cpp
lv_obj_t *btn = lv_btn_create(parent);
lv_obj_set_size(btn, 80, 80);
lv_obj_set_pos(btn, x, y);
lv_obj_add_event_cb(btn, button_event_handler, LV_EVENT_CLICKED, user_data);

// Add icon (using custom font)
lv_obj_t *icon = lv_label_create(btn);
lv_obj_set_style_text_font(icon, &material_icons_48, 0);
lv_label_set_text(icon, ICON_COPY);
lv_obj_center(icon);
```

**Radial layout calculation:**
```cpp
float angle_rad = (position * 45.0 - 90.0) * M_PI / 180.0;
int x = 180 + (int)(130 * cos(angle_rad));
int y = 180 + (int)(130 * sin(angle_rad));
```

**Grid layout positions** (lookup table):
```cpp
const Point grid_positions[9] = {
    {180, 50},   // Position 1
    {130, 130},  // Position 2
    {230, 130},  // Position 3
    {80, 210},   // Position 4
    {180, 210},  // Position 5
    {280, 210},  // Position 6
    {130, 290},  // Position 7
    {230, 290},  // Position 8
    {180, 340}   // Position 9 (optional/adjusted)
};
```

**Touch handling:**
- LVGL automatically handles touch → button press mapping
- Button event callback receives `LV_EVENT_CLICKED`
- Lookup button config → execute action (keystrokes or navigation)

**Performance considerations:**
- All 9 buttons rendered simultaneously (static screen)
- Icon font glyphs loaded on-demand from Flash
- Smooth 60fps possible with ESP32-S3 + LVGL optimization
- Screen transitions: simple fade or slide animations (50-150ms)

---

### **Summary:**

**Both templates support exactly 9 buttons:**
- **Radial**: 1 center + 8 outer ring (circular, distinctive center action)
- **Grid**: 1-2-3-2-1 diamond pattern (balanced, familiar)

**Navigation is flexible:**
- Any button can be navigation or action
- No fixed nav area = full use of all 9 buttons
- Users design their own navigation patterns

**Implementation complexity: Medium**
- Button positioning: Simple math (radial) or lookup table (grid)
- LVGL rendering: Standard button widgets
- Touch handling: Built-in LVGL events
- Main work: Config management and action routing

### Home Screen

**Goal**: Define the purpose and content of the home screen.

**Requirements**:
- Home screen is not a configurable macropad (it's a special built-in screen)
- Displays device status and network information
- Always accessible via navigation (first screen in cycle)
- Provides quick overview without needing to open web portal

**Content to Display** (inspired by web portal health widget):

**System Info:**
- Device name / mDNS hostname
- Uptime (formatted: "2d 5h 32m" or similar)
- Firmware version
- Last reset reason

**Network Info:**
- WiFi status (connected/disconnected)
- IP address (if connected)
- WiFi signal strength / RSSI (e.g., "-65 dBm (Good)")
- mDNS address (e.g., "macropad-a1b2.local")

**Hardware Stats:**
- CPU usage percentage
- Temperature (if available on ESP32-S3)
- Free heap memory
- Flash usage

**Layout** (for 360×360 round display):
```
┌─────────────────────────┐
│   [Device Name]         │  Large centered text
│                         │
│   IP: 192.168.1.100     │  Network info section
│   WiFi: -55 dBm         │
│   macropad.local        │
│                         │
│   Uptime: 2h 15m        │  Status section
│   CPU: 12%  Temp: 45°C  │
│   Memory: 234 KB free   │
│                         │
│   v1.0.0                │  Small version footer
└─────────────────────────┘
```

**Navigation:**
- Tapping anywhere on home screen advances to next screen (first macropad)
- Or use dedicated "Next" button if we add nav buttons
- "Home" action button (ACTION_NAV_HOME) from any macropad returns here

**UI Implementation:**
- Use LVGL labels and formatted text
- Update stats every 5-10 seconds automatically
- No buttons/interactions needed (just display info)
- Simple, readable layout optimized for quick glance

#### Spike results

**Decision: Status Dashboard Home Screen**

The home screen will be a **non-configurable status dashboard** that displays device and network information. This serves as:
1. **Quick reference** - See device status without opening web portal
2. **Troubleshooting aid** - Check IP address, WiFi strength, uptime
3. **Navigation starting point** - Natural place to start after boot/reset

**Data Sources:**
All information already available via `/api/health` endpoint:
- `uptime_seconds` → formatted uptime display
- `ip_address`, `hostname` → network info
- `wifi_rssi` → signal strength
- `cpu_usage`, `temperature` → hardware stats
- `heap_free` → memory status
- Version from `src/version.h`

**Update Strategy:**
- Read stats from same functions used by `/api/health` endpoint
- Update display every 5 seconds (no need for real-time updates)
- Minimal performance impact (just text rendering)

**Implementation:**
- New file: `src/app/ui/screens/home_screen.cpp/h`
- Inherits from `base_screen.h` (same pattern as splash_screen)
- Simple LVGL label-based layout
- No user interaction needed (read-only display)
- Screen manager navigates: Home → Pad 0 → Pad 1 → ... → Pad N → Home

**Estimated Complexity: Low**
- ~150 lines for home_screen.cpp
- Reuses existing health stat collection functions
- No new backend code needed (data already available)

### Configuration structure

We need to define the structure to save the macropad screens (including buttons, icons, and keystrokes) in NVS.

**Goals:**
- Store up to 5 macropad screen configurations persistently
- Each screen has template type, buttons with labels and keystrokes
- Support flexible navigation actions (not just keystrokes)
- Efficient NVS storage and retrieval
- Easy to serialize/deserialize for web API

**Existing Infrastructure:**
- `config_manager.h/cpp`: Already manages WiFi/device config in NVS namespace "device_cfg"
- Uses ESP32 Preferences library (high-level NVS wrapper)
- Pattern: Separate namespace per logical config group

#### Spike results

**Design Decision: Two Storage Approaches**

Given the complexity of macropad configuration vs simple WiFi settings, we'll use **two different storage strategies**:

1. **DeviceConfig** (existing): Simple flat structure → Continue using Preferences (key/value pairs)
2. **MacroPadConfig** (new): Complex nested structure → Use NVS blob storage

---

### **MacroPad Configuration Structure**

**C++ Structures:**

```cpp
// Action types (expanded from sample)
enum MacroActionType {
    ACTION_KEYSTROKES = 0,  // Send keystrokes string via BLE keyboard
    ACTION_NAV_HOME = 1,    // Navigate to home screen
    ACTION_NAV_NEXT = 2,    // Navigate to next macropad screen
    ACTION_NAV_PREV = 3,    // Navigate to previous macropad screen
    ACTION_NAV_GOTO = 4     // Navigate to specific screen (by ID)
};

// Label types
enum MacroLabelType {
    LABEL_TEXT = 0,         // Plain text label
    LABEL_ICON = 1          // Icon name (from material_icons font)
};

// Template types
enum MacroTemplate {
    TEMPLATE_RADIAL = 0,    // 1 center + 8 outer buttons in circle
    TEMPLATE_GRID = 1       // 1-2-3-2-1 diamond pattern
};

// Constants
#define MAX_MACRO_PADS 10
#define MAX_BUTTONS_PER_PAD 9
#define MACRO_NAME_MAX_LEN 32
#define MACRO_LABEL_MAX_LEN 24
#define MACRO_ICON_MAX_LEN 32
#define MACRO_KEYSTROKES_MAX_LEN 256

// Button configuration
struct MacroButton {
    uint8_t position;                              // Button position (0-8)
    MacroLabelType label_type;                     // Text or icon
    char label[MACRO_LABEL_MAX_LEN];              // Text label (if label_type == LABEL_TEXT)
    char icon[MACRO_ICON_MAX_LEN];                // Icon name (if label_type == LABEL_ICON)
    MacroActionType action_type;                   // Keystrokes or navigation
    char keystrokes[MACRO_KEYSTROKES_MAX_LEN];    // Keystrokes string (if action_type == ACTION_KEYSTROKES)
    uint8_t nav_target_id;                         // Target screen ID (if action_type == ACTION_NAV_GOTO)
    bool enabled;                                  // Whether this button is configured
};

// Macro pad screen configuration
struct MacroPad {
    uint8_t id;                                    // Unique ID (0-4)
    char name[MACRO_NAME_MAX_LEN];                // User-friendly name (e.g., "Media Controls")
    MacroTemplate template_type;                   // Radial or grid layout
    uint8_t button_count;                         // Number of configured buttons (0-9)
    MacroButton buttons[MAX_BUTTONS_PER_PAD];
    bool enabled;                                  // Whether this screen is active
};

// Top-level config
struct MacroPadConfig {
    uint8_t count;                                 // Number of configured macro pads (0-5)
    MacroPad pads[MAX_MACRO_PADS];
    uint32_t magic;                                // Validation: 0xCAFEFEED
};

#define MACRO_PAD_MAGIC 0xCAFEFEED
```

**Memory Footprint:**

```
MacroButton = 
    1 (position) + 
    1 (label_type) + 
    24 (label) + 
    32 (icon) + 
    1 (action_type) + 
    256 (keystrokes) + 
    1 (nav_target_id) + 
    1 (enabled) + 
    padding
    ≈ 320 bytes

MacroPad = 
    1 (id) + 
    32 (name) + 
    1 (template_type) + 
    1 (button_count) + 
    (320 × 9 buttons) + 
    1 (enabled) + 
    padding
    ≈ 2,920 bytes

MacroPadConfig = 
    1 (count) + 
    (2,920 × 10 pads) + 
    4 (magic) + 
    padding
    ≈ 29,210 bytes (~29KB)
```

**NVS Storage: ~29KB total** - requires custom partition table with 32KB NVS

---

### **NVS Storage Strategy**

**Namespace**: `"macropad_cfg"` (separate from `"device_cfg"`)

**Storage Method**: Single blob

```cpp
// Store entire config as binary blob
bool macro_pad_manager_save() {
    Preferences prefs;
    prefs.begin("macropad_cfg", false); // RW mode
    
    MacroPadConfig *cfg = macro_pad_manager_get_config();
    cfg->magic = MACRO_PAD_MAGIC;
    
    size_t written = prefs.putBytes("config_blob", cfg, sizeof(MacroPadConfig));
    prefs.end();
    
    return (written == sizeof(MacroPadConfig));
}

// Load entire config from blob
bool macro_pad_manager_load() {
    Preferences prefs;
    prefs.begin("macropad_cfg", true); // RO mode
    
    MacroPadConfig *cfg = macro_pad_manager_get_config();
    size_t loaded = prefs.getBytes("config_blob", cfg, sizeof(MacroPadConfig));
    prefs.end();
    
    if (loaded != sizeof(MacroPadConfig)) return false;
    if (cfg->magic != MACRO_PAD_MAGIC) return false;
    
    return true;
}
```

**Why blob vs key/value?**
- ✅ Simpler code (one read/write vs 100+ operations)
- ✅ Atomic updates (no partial writes)
- ✅ Faster load time (~5ms vs 50ms+)
- ✅ Natural fit for complex nested structures
- ❌ Entire config rewritten on any change (acceptable - config changes are rare)

---

### **Configuration Manager API**

**New file: `macro_pad_manager.h/cpp`** (based on sample but simplified)

```cpp
// Initialization
void macro_pad_manager_init();                     // Initialize (call once at boot)
bool macro_pad_manager_load();                     // Load from NVS
bool macro_pad_manager_save();                     // Save to NVS
bool macro_pad_manager_reset();                    // Erase all (factory reset)

// Query
int macro_pad_manager_count();                     // Get number of active pads
bool macro_pad_manager_get(int index, MacroPad *pad);        // Get by index
bool macro_pad_manager_get_by_id(uint8_t id, MacroPad *pad); // Get by ID

// Modify
bool macro_pad_manager_add(const MacroPad *pad);   // Add new pad
bool macro_pad_manager_update(uint8_t id, const MacroPad *pad); // Update existing
bool macro_pad_manager_remove(uint8_t id);         // Remove pad

// Helpers
const char* macro_pad_manager_template_name(MacroTemplate type);
int macro_pad_manager_template_max_buttons(MacroTemplate type);
```

**Usage Pattern:**

```cpp
// Boot time
macro_pad_manager_init();
if (!macro_pad_manager_load()) {
    Logger.logMessage("MacroPad", "No saved config, starting fresh");
}

// Query for rendering
int count = macro_pad_manager_count();
for (int i = 0; i < count; i++) {
    MacroPad pad;
    if (macro_pad_manager_get(i, &pad)) {
        render_macropad_screen(&pad);
    }
}

// Modify via web API
MacroPad new_pad;
new_pad.id = 0;
strcpy(new_pad.name, "Media Controls");
new_pad.template_type = TEMPLATE_RADIAL;
// ... configure buttons ...
macro_pad_manager_add(&new_pad);
macro_pad_manager_save(); // Persist
```

---

### **Web API JSON Format**

**REST Endpoints:**

```
GET  /api/macropads           → List all macropad screens
GET  /api/macropads/:id       → Get specific macropad
POST /api/macropads           → Create new macropad
PUT  /api/macropads/:id       → Update macropad
DELETE /api/macropads/:id     → Delete macropad
```

**JSON Schema (GET/POST/PUT):**

```json
{
  "id": 0,
  "name": "Media Controls",
  "template": "radial",
  "enabled": true,
  "buttons": [
    {
      "position": 0,
      "label_type": "icon",
      "label": "",
      "icon": "play-pause",
      "action_type": "keystrokes",
      "keystrokes": "{PlayPause}",
      "nav_target_id": 0,
      "enabled": true
    },
    {
      "position": 1,
      "label_type": "icon",
      "icon": "volume-high",
      "action_type": "keystrokes",
      "keystrokes": "{VolumeUp}",
      "enabled": true
    },
    {
      "position": 8,
      "label_type": "text",
      "label": "Home",
      "action_type": "nav_home",
      "enabled": true
    }
  ]
}
```

**Conversion Functions:**

```cpp
// Serialize to JSON (for web API response)
String macro_pad_to_json(const MacroPad *pad);

// Deserialize from JSON (for web API request)
bool json_to_macro_pad(const String &json, MacroPad *pad);
```

Uses **ArduinoJson** library (already in `arduino-libraries.txt`).

---

### **Validation Rules**

**On save:**
1. Check `id` in range [0, 4]
2. Check `button_count` matches number of enabled buttons
3. Check button positions unique (no duplicates)
4. Check button positions in range [0, 8]
5. Validate keystrokes string format (warn but don't reject)
6. Check `nav_target_id` in range [0, 9] for ACTION_NAV_GOTO
7. Check total pad count ≤ MAX_MACRO_PADS (10)

**On load:**
1. Check `magic` == MACRO_PAD_MAGIC
2. Check blob size == sizeof(MacroPadConfig)
3. If validation fails, treat as factory-fresh device

**Error handling:**
- Invalid config → Log warning, start with empty config
- Corrupted NVS → Erase and reinitialize
- Parsing errors → Return HTTP 400 with error message

---

### **Comparison to Sample Code**

**Sample's Approach:**
- Complex `KeyAction` structure with sequences and delays
- Pre-parsed key codes stored in arrays
- More flexible but more complex

**Our Approach:**
- **Simpler**: Keystrokes stored as strings (parse at runtime)
- **Smaller**: 256 bytes per button vs ~400 bytes (sample)
- **Easier to maintain**: No manual key code mapping in config
- **Same capabilities**: Keystrokes parser handles all cases

**Tradeoff:**
- ❌ Slightly slower button press (string parsing overhead ~1ms)
- ✅ Cleaner config management
- ✅ Easier web UI (users type strings, not build arrays)
- ✅ Less flash usage (smaller structures)

**Conclusion**: String-based approach is better for this project.

---

### **Summary**

**Configuration Storage Decision: LittleFS + JSON (Not NVS)**

After investigating NVS size constraints (20KB max with standard partition layout), we've decided to use **LittleFS filesystem with JSON storage** instead of NVS binary blobs for macropad configuration.

**Why LittleFS instead of NVS:**
- ✅ **No size limits**: 20KB NVS only fits ~6 screens, but 64KB+ SPIFFS fits 30+ screens
- ✅ **Human-readable**: JSON files easy to debug, backup, manually edit
- ✅ **Export/Import ready**: Web UI can download/upload JSON directly
- ✅ **Already available**: LittleFS + ArduinoJson built into ESP32 Arduino core
- ✅ **Sample code exists**: Sample already uses JSON serialization pattern
- ✅ **Future-proof**: Easy to expand storage, add versioning, schema migrations

**Storage Architecture:**
- **Device config** (WiFi, device name): Keep in NVS (small, benefits from wear leveling)
- **Macropad config** (10+ screens): Store in LittleFS as JSON (large, infrequent writes)

**File Structure:**
```
/littlefs/
  macropads.json    -> All macropad screens in single JSON file
                       (or separate files: pad_0.json, pad_1.json, etc.)
```

**JSON Format (minified on disk):**
```json
{
  "version": 1,
  "count": 2,
  "pads": [
    {
      "id": 0,
      "name": "Media Controls",
      "template": "radial",
      "buttons": [
        {
          "position": 0,
          "label_type": "icon",
          "icon": "play-pause",
          "action_type": "keystrokes",
          "keystrokes": "{PlayPause}"
        }
      ]
    }
  ]
}
```

**Storage Capacity:**
- Per screen: ~1.5-2KB JSON (minified)
- 10 screens: ~15-20KB
- 64KB SPIFFS: Fits 30+ screens comfortably
- Can expand SPIFFS to 256KB+ if needed

**Performance:**
- Boot load time: +10-20ms vs NVS (negligible)
- RAM usage: +4-8KB temporary during JSON parse (acceptable with 8MB PSRAM)
- Save time: ~20-30ms per operation

**Data Model:**
- **10+ macropad screens** (no hard limit, constrained only by SPIFFS size)
- **9 buttons per screen**
- **Flexible navigation**: Any button can be action or nav
- **Keystrokes as strings**: Stored directly in JSON, parsed at runtime
- **Icons as names**: String references to material_icons font

**API:**
- **C++**: `macro_pad_manager_*()` functions (same interface as NVS approach)
- **Web**: REST JSON endpoints
- **LittleFS**: Mount at boot, load all configs into RAM
- **ArduinoJson**: For serialization/deserialization

**Implementation Changes Required:**

1. **Update partitions.csv**: Increase SPIFFS from 64KB → 256KB
   ```csv
   spiffs,   data, spiffs,  0x3F0000, 0x40000,  # 256KB instead of 64KB
   ```

2. **Create macro_pad_manager.h/cpp** using LittleFS:
   - Mount LittleFS at boot
   - Load `/littlefs/macropads.json` into RAM
   - Provide same API as NVS approach (transparent to callers)
   - Save operations write JSON to filesystem
   - Add export/import functions for web UI

3. **Add LittleFS initialization** in app.ino:
   ```cpp
   #include <LittleFS.h>
   
   void setup() {
     if (!LittleFS.begin(true)) {  // true = format if mount fails
       Logger.logMessage("FS", "Failed to mount LittleFS");
     }
     macro_pad_manager_init();
     macro_pad_manager_load();
   }
   ```

4. **Update web portal** REST API endpoints:
   - Keep same JSON schema (already designed for REST API)
   - Add `GET /api/macropads/export` → download macropads.json
   - Add `POST /api/macropads/import` → upload macropads.json

**Implementation Effort: Medium**
- ~300 lines for macro_pad_manager.cpp (LittleFS + JSON)
- ~100 lines for JSON serialization helpers
- ~200 lines for REST API endpoints
- ~50 lines for export/import
- Total: ~650 lines new code

**No changes needed:**
- Data structures (MacroPad, MacroButton) remain the same
- Web UI JSON schema unchanged
- REST API endpoints unchanged (just backend storage differs)

## Implementation plan

### Overview

Phased, incremental approach with frequent build/test opportunities on real hardware. Each phase delivers working functionality that can be tested independently.

---

## Phase 1: Foundation - Storage & Data Structures

**Goal**: Set up configuration storage without any UI, testable via serial logs.

**Deliverables**:
- LittleFS filesystem initialization
- MacroPad data structures
- JSON serialization/deserialization
- Basic config manager API
- Serial debug commands for testing

**Tasks**:

1. **Update partition table** (5 min)
   - Expand SPIFFS from 64KB → 256KB in `partitions.csv`
   - Test build to verify partition changes work

2. **Create macro_pad_manager.h** (~30 min)
   - Define `MacroPad`, `MacroButton`, `MacroPadConfig` structures
   - Define `MacroActionType`, `MacroLabelType`, `MacroTemplate` enums
   - Declare API functions (init, load, save, get, add, update, remove)
   
3. **Create macro_pad_manager.cpp** (~2 hours)
   - Implement LittleFS mount/format logic
   - Implement JSON serialization (MacroPad → JSON string)
   - Implement JSON deserialization (JSON string → MacroPad)
   - Implement load from `/littlefs/macropads.json`
   - Implement save to `/littlefs/macropads.json`
   - Implement CRUD functions (add, get, update, remove)
   - Add logging for all operations

4. **Add test code to app.ino** (~30 min)
   - Mount LittleFS at boot
   - Initialize macro_pad_manager
   - Create sample macropad config (2-3 test screens)
   - Save to filesystem
   - Load and print to serial
   - Add serial commands: `dump`, `reset`, `test`

**Test Plan**:
- Build and upload firmware
- Open serial monitor
- Verify LittleFS mounts successfully
- Run `test` command → creates sample config
- Run `dump` command → prints JSON to serial
- Reboot device → verify config persists
- Run `reset` command → erases config
- Success criteria: Config survives reboot

**Estimated Time**: 3-4 hours

---

## Phase 2: BLE Keyboard Integration

**Goal**: Send keystrokes via BLE, testable with hardcoded test sequences.

**Deliverables**:
- BLE keyboard initialization
- Keystrokes string parser
- Test function to send keystrokes from serial commands

**Tasks**:

1. **Copy BLE keyboard from sample** (~15 min)
   - Copy `sample/src/app/BleKeyboard.h` to `src/app/`
   - Copy `sample/src/app/BleKeyboard.cpp` to `src/app/`
   - Add to build (Arduino CLI compiles .cpp in sketch dir)

2. **Create keystrokes_parser.h/cpp** (~2 hours)
   - Parse keystrokes strings: `Ctrl+C`, `{Enter}`, `abc`, etc.
   - Handle modifiers: Ctrl, Shift, Alt, Cmd
   - Handle special keys: `{Enter}`, `{Tab}`, `{F1}`, etc.
   - Handle media keys: `{VolumeUp}`, `{PlayPause}`, etc.
   - Handle sequences: `Ctrl+A,{Wait:100},Ctrl+C`
   - Handle delays: `{Wait:N}` where N is milliseconds
   - Return parsed actions for BLE keyboard to execute
   - Case-insensitive parsing

3. **Create keystroke_executor.h/cpp** (~1 hour)
   - Execute parsed keystrokes via BLE keyboard
   - Handle modifier press/release
   - Handle delays between actions
   - Add logging for each keystroke sent

4. **Add BLE test code to app.ino** (~30 min)
   - Initialize BLE keyboard with device name from config
   - Add serial command: `send <keystrokes>` (e.g., `send Ctrl+C`)
   - Add serial command: `macro 0` (executes first button of first macropad)
   - Test various keystroke strings via serial

**Test Plan**:
- Build and upload firmware
- Pair device with computer via Bluetooth
- Open notepad/text editor on computer
- Run `send hello world` → verify text appears
- Run `send Ctrl+A,Ctrl+C` → verify select all + copy
- Run `send {VolumeUp}` → verify volume increases
- Run `macro 0` → executes first saved button keystroke
- Success criteria: All keystrokes work correctly

**Estimated Time**: 4-5 hours

---

## Phase 3: Home Screen UI

**Goal**: Display home screen on device, no macropad screens yet.

**Deliverables**:
- Home screen showing device stats
- Stats update every 5 seconds
- Tap screen to advance (placeholder - no next screen yet)

**Tasks**:

1. **Create home_screen.h/cpp** (~2 hours)
   - Inherit from `base_screen.h` (same pattern as splash_screen)
   - Create LVGL layout with labels for all stats
   - Device name (large, centered top)
   - Network section: IP, WiFi RSSI, mDNS hostname
   - System section: Uptime, CPU, temperature, memory
   - Footer: Firmware version
   - Implement `onCreate()` to build UI
   - Implement `onUpdate()` to refresh stats every 5s

2. **Create device_stats.h/cpp** (~1 hour)
   - Extract stats collection from web_portal.cpp `/api/health`
   - Reusable functions: `getUptime()`, `getCpuUsage()`, `getWifiRSSI()`, etc.
   - Format helpers: `formatUptime()`, `formatHeap()`, `formatSignalStrength()`
   - Both web portal and home screen use these functions

3. **Update screen_manager** (~30 min)
   - Add home_screen to screen list
   - After splash → show home_screen
   - Tap home screen → no-op for now (placeholder)

4. **Test home screen display** (~15 min)
   - Build and upload firmware
   - Verify home screen appears after splash
   - Verify all stats displayed correctly
   - Verify stats update every 5 seconds

**Test Plan**:
- Build and upload firmware
- Wait for splash screen to finish
- Verify home screen displays with correct data
- Check IP address matches web portal
- Check uptime increments
- Verify WiFi signal strength shown
- Success criteria: Home screen displays live stats

**Estimated Time**: 3-4 hours

---

## Phase 4: Macropad Screen Rendering (Single Screen)

**Goal**: Render one macropad screen with buttons (no keystroke execution yet).

**Deliverables**:
- RADIAL layout renderer
- GRID layout renderer
- Display button labels/icons (text labels for now, icons later)
- Navigation: Home → Macropad 0 → Home (loop)

**Tasks**:

1. **Create macropad_screen.h/cpp** (~3 hours)
   - Inherit from `base_screen.h`
   - Constructor takes `MacroPad*` config
   - Render buttons based on template type (RADIAL or GRID)
   - Use position lookup tables from layouts spike
   - Create LVGL button widgets
   - Add text labels to buttons (icon support comes later)
   - Handle button clicks → log button position to serial for now
   - Style buttons: normal state, pressed state

2. **Update screen_manager** (~30 min)
   - Load macropad configs from macro_pad_manager
   - Create macropad_screen instances for each config
   - Navigation cycle: Home → Pad 0 → Pad 1 → ... → Pad N → Home
   - Skip screens with no buttons (per requirements)
   - Implement touch navigation (tap anywhere to advance)

3. **Add test macropad config** (~15 min)
   - Create 2-3 test macropads in Phase 1 test code
   - Mix of RADIAL and GRID layouts
   - Text labels for all buttons
   - Various keystroke strings assigned

**Test Plan**:
- Build and upload firmware
- From home screen, tap to advance
- Verify first macropad screen renders
- Verify button layout matches template (RADIAL or GRID)
- Verify button labels display correctly
- Tap buttons → verify serial logs show button position
- Tap anywhere on background → advance to next screen
- Success criteria: Can navigate through all screens

**Estimated Time**: 4-5 hours

---

## Phase 5: Connect Buttons to Keystrokes

**Goal**: Tapping buttons executes keystrokes via BLE.

**Deliverables**:
- Button tap → parse keystrokes → send via BLE
- Navigation actions work (ACTION_NAV_HOME, etc.)
- Full end-to-end functionality (screen → button → keystroke → computer)

**Tasks**:

1. **Update macropad_screen button handler** (~1 hour)
   - On button click, get MacroButton config
   - Check action_type:
     - `ACTION_KEYSTROKES`: Parse and execute keystrokes
     - `ACTION_NAV_HOME`: Navigate to home screen
     - `ACTION_NAV_NEXT`: Navigate to next screen
     - `ACTION_NAV_PREV`: Navigate to previous screen
     - `ACTION_NAV_GOTO`: Navigate to specific screen by ID
   - Add visual feedback on button press (highlight, then restore)

2. **Test navigation actions** (~30 min)
   - Configure buttons with navigation actions
   - Test NAV_HOME from any screen
   - Test NAV_NEXT/PREV cycling
   - Test NAV_GOTO jumping to specific screen

3. **Test keystroke execution** (~30 min)
   - Configure buttons with various keystrokes
   - Test simple characters: `hello`
   - Test shortcuts: `Ctrl+C`, `Ctrl+V`
   - Test sequences: `Ctrl+A,Ctrl+C`
   - Test media keys: `{VolumeUp}`, `{PlayPause}`

**Test Plan**:
- Build and upload firmware
- Pair BLE keyboard with computer
- Navigate to first macropad screen
- Tap button configured with text → verify types in computer
- Tap button configured with shortcut → verify executes
- Tap button configured with NAV_HOME → returns to home
- Success criteria: All button actions work correctly

**Estimated Time**: 2-3 hours

---

## Phase 6: Icon Font Support

**Goal**: Replace text labels with icons on buttons.

**Deliverables**:
- Icon font build tool
- Material Design Icons embedded in firmware
- Buttons render icons instead of text

**Tasks**:

1. **Create tools/build-icon-font.sh** (~2 hours)
   - Download materialdesignicons-webfont.ttf
   - Read icon list from `icons.json` (user-curated list)
   - Generate LVGL font using online converter (or lv_font_conv CLI)
   - Output: `src/app/material_icons_48.c` and `material_icons.h`
   - Include in build process (run before Arduino compile)

2. **Create icons.json** (~1 hour)
   - List 100-200 most useful icon names for macropad use
   - Categories: media (play, pause, stop, volume)
   - Categories: navigation (home, back, forward, up, down)
   - Categories: actions (copy, paste, cut, save, delete)
   - Categories: apps (browser, terminal, calculator, mail)
   - Map icon names to Unicode codepoints from Material Design Icons

3. **Create icon_mapper.h/cpp** (~30 min)
   - Function: `const char* getIconGlyph(const char* iconName)`
   - Maps user string (e.g., "play-pause") to UTF-8 glyph
   - Returns nullptr if icon not found (fallback to text label)
   - Case-insensitive lookup

4. **Update macropad_screen rendering** (~1 hour)
   - Check button label_type
   - If LABEL_ICON: lookup icon glyph, set font to material_icons_48
   - If LABEL_TEXT: use regular font
   - If icon not found: display text label as fallback

5. **Update build.sh** (~15 min)
   - Run `tools/build-icon-font.sh` before compile (similar to web assets)
   - Only regenerate if icons.json changed (optional optimization)

**Test Plan**:
- Create icons.json with 50-100 icons
- Run build script → verify icon font generated
- Update test macropad config to use icon labels
- Build and upload firmware
- Verify icons render on buttons
- Verify fallback to text if icon not found
- Success criteria: Icons display correctly on all buttons

**Estimated Time**: 4-5 hours

---

## Phase 7: Web Portal - REST API

**Goal**: Configure macropad screens via web portal.

**Deliverables**:
- REST API endpoints for CRUD operations
- Test with curl/Postman before building web UI

**Tasks**:

1. **Add REST endpoints to web_portal.cpp** (~3 hours)
   - `GET /api/macropads` → List all macropad screens
   - `GET /api/macropads/:id` → Get specific macropad
   - `POST /api/macropads` → Create new macropad
   - `PUT /api/macropads/:id` → Update existing macropad
   - `DELETE /api/macropads/:id` → Delete macropad
   - Use ArduinoJson for request/response parsing
   - Call macro_pad_manager functions
   - Return appropriate HTTP status codes (200, 400, 404, 500)

2. **Add validation logic** (~1 hour)
   - Check ID in range [0-9]
   - Check button positions unique and in range [0-8]
   - Check template type valid (RADIAL or GRID)
   - Check action types valid
   - Return detailed error messages for invalid requests

3. **Test REST API via curl** (~1 hour)
   - GET all macropads → verify returns JSON array
   - POST new macropad → verify creates and persists
   - PUT existing macropad → verify updates
   - DELETE macropad → verify removes
   - Test error cases (invalid ID, duplicate position, etc.)

**Test Plan**:
- Build and upload firmware
- Use curl or Postman to test endpoints:
  ```bash
  curl http://macropad.local/api/macropads
  curl -X POST http://macropad.local/api/macropads -d '{"name":"Test","template":"radial",...}'
  curl -X PUT http://macropad.local/api/macropads/0 -d '{...}'
  curl -X DELETE http://macropad.local/api/macropads/0
  ```
- Verify device updates immediately (screen manager reloads config)
- Success criteria: All CRUD operations work via API

**Estimated Time**: 4-5 hours

---

## Phase 8: Web Portal - Configuration UI

**Goal**: User-friendly web interface to configure macropads.

**Deliverables**:
- Macropad list/edit page
- Button configuration form
- Icon picker
- Keystroke input field
- Export/import functionality

**Tasks**:

1. **Create macropads.html page** (~2 hours)
   - List all macropad screens in cards
   - Show template type, name, button count
   - Buttons: Edit, Delete, Add New
   - Click Edit → navigate to edit page

2. **Create macropad-edit.html page** (~4 hours)
   - Form: Macropad name, template type
   - Visual layout preview (RADIAL or GRID diagram)
   - Click button position → open button config modal
   - Button config modal:
     - Label type: Icon or Text
     - Icon picker (dropdown or searchable list)
     - Text label input
     - Action type: Keystrokes or Navigation
     - Keystrokes input (text field with examples)
     - Navigation target dropdown
   - Save button → PUT to API
   - Cancel button → return to list

3. **Create macropads.js** (~3 hours)
   - Fetch macropads from API
   - Render macropad list
   - Handle create/update/delete operations
   - Form validation (check required fields)
   - Visual feedback (loading, success, errors)

4. **Add navigation to portal** (~30 min)
   - Update nav bar with "Macropads" link
   - Update `portal.js` initNavigation()

5. **Add export/import** (~2 hours)
   - Export button → download macropads.json
   - Import button → upload file, parse, POST to API
   - Validation on import (check schema)

**Test Plan**:
- Open web portal
- Navigate to Macropads page
- Create new macropad screen
- Add buttons with icons and keystrokes
- Save and verify on device
- Export config to JSON file
- Delete all macropads
- Import JSON file
- Verify restored correctly
- Success criteria: Full CRUD via web UI works

**Estimated Time**: 10-12 hours

---

## Phase 9: Polish & Testing

**Goal**: Refinements, bug fixes, documentation.

**Deliverables**:
- Comprehensive testing on real device
- Bug fixes
- Documentation updates
- Performance optimization

**Tasks**:

1. **End-to-end testing** (~2 hours)
   - Test all keystrokes types
   - Test all navigation patterns
   - Test edge cases (empty config, max buttons, long keystrokes)
   - Test config persistence across reboots
   - Test BLE pairing with multiple devices

2. **UI polish** (~2 hours)
   - Improve button visual feedback
   - Add loading states during config changes
   - Add confirmation dialogs for delete operations
   - Improve error messages

3. **Performance optimization** (~1 hour)
   - Profile memory usage
   - Optimize JSON parsing (use filters if needed)
   - Reduce heap fragmentation
   - Test with max config (10 screens, 90 buttons)

4. **Documentation** (~2 hours)
   - Update README with macropad feature
   - Document keystrokes string format
   - Document REST API endpoints
   - Add user guide for web portal
   - Add screenshots/diagrams

5. **Create release** (~30 min)
   - Update CHANGELOG.md
   - Tag release version
   - Build binaries via GitHub Actions
   - Test release binaries on clean device

**Test Plan**:
- Full regression testing
- Test with 10 macropad screens
- Test with complex keystroke sequences
- Verify no memory leaks over 24 hours
- Success criteria: Stable, production-ready

**Estimated Time**: 7-8 hours

---

## Total Estimated Time

- Phase 1: 3-4 hours
- Phase 2: 4-5 hours
- Phase 3: 3-4 hours
- Phase 4: 4-5 hours
- Phase 5: 2-3 hours
- Phase 6: 4-5 hours
- Phase 7: 4-5 hours
- Phase 8: 10-12 hours
- Phase 9: 7-8 hours

**Total: 41-51 hours** (roughly 5-7 days of full-time work, or 2-3 weeks part-time)

---

## Phase Dependencies

```
Phase 1 (Storage)
    ↓
Phase 2 (BLE Keyboard) ← Can test independently with serial commands
    ↓
Phase 3 (Home Screen)
    ↓
Phase 4 (Macropad Rendering)
    ↓
Phase 5 (Button Actions) ← First fully functional version
    ↓
Phase 6 (Icons) ← Enhancement, not blocking
    ↓
Phase 7 (REST API)
    ↓
Phase 8 (Web UI)
    ↓
Phase 9 (Polish)
```

**Key Milestones**:
- **Phase 1 complete**: Config persists across reboots
- **Phase 2 complete**: Can send keystrokes from serial commands
- **Phase 3 complete**: Home screen displays on device
- **Phase 5 complete**: ✨ **First MVP** - Full macropad functionality works end-to-end
- **Phase 6 complete**: Icons make UI much nicer
- **Phase 8 complete**: ✨ **Feature complete** - Users can configure via web portal
- **Phase 9 complete**: ✨ **Production ready** - Stable, documented, tested

---

## Testing Strategy

**After each phase**:
1. Build firmware: `./build.sh`
2. Upload to device: `./upload.sh jc3636w518`
3. Monitor serial: `./monitor.sh`
4. Test deliverables for that phase
5. Verify no regressions from previous phases
6. Document any issues found

**Continuous validation**:
- Serial logging throughout for debugging
- Web portal always accessible for checking config
- Regular reboots to test persistence
- Test on both Windows and Mac/Linux BLE hosts

This phased approach ensures you can test on real hardware frequently and catch issues early!