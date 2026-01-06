/**
 * Configuration Portal JavaScript
 * Handles configuration form, OTA updates, and device reboots
 * Supports core mode (AP) and full mode (WiFi connected)
 * Multi-page support: home, network, firmware
 */

// API endpoints
const API_CONFIG = '/api/config';
const API_INFO = '/api/info';
const API_MODE = '/api/mode';
const API_UPDATE = '/api/update';
const API_REBOOT = '/api/reboot';
const API_VERSION = '/api/info'; // Used for connection polling
const API_FIRMWARE_LATEST = '/api/firmware/latest';
const API_FIRMWARE_UPDATE = '/api/firmware/update';
const API_FIRMWARE_UPDATE_STATUS = '/api/firmware/update/status';
const API_MACROS = '/api/macros';

let selectedFile = null;
let portalMode = 'full'; // 'core' or 'full'
let currentPage = 'home'; // Current page: 'home', 'network', or 'firmware'

let deviceInfoCache = null;
let githubAutoChecked = false;

// ===== MACROS (Home) =====

// Default (MVP) is 8 screens, but the firmware can temporarily override this
// for debugging. The UI adapts to whatever /api/macros returns.
const MACROS_SCREEN_COUNT_DEFAULT = 8;
let macrosScreenCount = MACROS_SCREEN_COUNT_DEFAULT;
const MACROS_BUTTONS_PER_SCREEN = 9;

// Macro button selector visualization (Option B / KISS): a small grid layout spec.
// This keeps today's 3×3 macropad layout but makes it data-driven for future templates.
// Cells contain 0-based slot indices (or null for empty spaces).
const MACROS_SELECTOR_LAYOUT_DEFAULT = {
    columns: 3,
    cells: [
        // Visual orientation: button #1 at 12 o'clock.
        // (Index mapping is purely for the editor UI; the underlying 0-based slot indices stay the same.)
        7, 0, 1,
        6, 8, 2,
        5, 4, 3,
    ],
};
let macrosSelectorLayout = MACROS_SELECTOR_LAYOUT_DEFAULT;

// Keep in sync with src/app/macros_config.h (char arrays include NUL terminator).
const MACROS_LABEL_MAX = 15;
const MACROS_SCRIPT_MAX = 255;
const MACROS_ICON_ID_MAX = 31;

let macrosPayloadCache = null; // { screens: [ { buttons: [ {label, action, script, icon_id}, ... ] }, ... ] }
let macrosSelectedScreen = 0; // 0-based
let macrosSelectedButton = 0; // 0-based
let macrosDirty = false;
let macrosLoading = false;

function macrosSetDirty(dirty) {
    macrosDirty = !!dirty;
    const hint = document.getElementById('macros_dirty_hint');
    if (hint) hint.style.display = macrosDirty ? 'block' : 'none';
}

function macrosCreateEmptyButton() {
    return { label: '', action: 'none', script: '', icon_id: '' };
}

function macrosCreateEmptyPayload() {
    const payload = { screens: [] };
    for (let s = 0; s < macrosScreenCount; s++) {
        const buttons = [];
        for (let b = 0; b < MACROS_BUTTONS_PER_SCREEN; b++) {
            buttons.push(macrosCreateEmptyButton());
        }
        payload.screens.push({ buttons });
    }
    return payload;
}

function macrosNormalizePayload(payload) {
    const out = macrosCreateEmptyPayload();
    if (!payload || !Array.isArray(payload.screens)) return out;

    for (let s = 0; s < Math.min(macrosScreenCount, payload.screens.length); s++) {
        const screen = payload.screens[s];
        if (!screen || !Array.isArray(screen.buttons)) continue;

        for (let b = 0; b < Math.min(MACROS_BUTTONS_PER_SCREEN, screen.buttons.length); b++) {
            const btn = screen.buttons[b] || {};
            out.screens[s].buttons[b] = {
                label: (btn.label || ''),
                action: (btn.action || 'none'),
                script: (btn.script || ''),
                icon_id: (btn.icon_id || ''),
            };
        }
    }

    return out;
}

function macrosClampString(value, maxLen) {
    const s = (value || '').toString();
    return s.length > maxLen ? s.slice(0, maxLen) : s;
}

function macrosActionSupportsScript(action) {
    return action === 'send_keys';
}

function macrosGetSelectedButton() {
    if (!macrosPayloadCache) return null;
    const screen = macrosPayloadCache.screens[macrosSelectedScreen];
    if (!screen) return null;
    return screen.buttons[macrosSelectedButton] || null;
}

function macrosSlotTitle(slotIndex) {
    const slot = slotIndex + 1;
    return slot === 9 ? 'Center (#9)' : `#${slot}`;
}

function macrosUpdateScriptCharCounter() {
    const charsEl = document.getElementById('macro_script_chars');
    if (!charsEl) return;
    const cfg = macrosGetSelectedButton();
    charsEl.textContent = cfg && cfg.script ? String(cfg.script.length) : '0';
}

function macrosRenderScreenSelect() {
    const select = document.getElementById('macro_screen_select');
    if (!select) return;

    if (select.options.length !== macrosScreenCount) {
        select.innerHTML = '';
        for (let i = 0; i < macrosScreenCount; i++) {
            const opt = document.createElement('option');
            opt.value = String(i);
            opt.textContent = `Screen ${i + 1}`;
            select.appendChild(opt);
        }
    }

    select.value = String(macrosSelectedScreen);
}

function macrosRenderButtonGrid() {
    const grid = document.getElementById('macro_button_grid');
    if (!grid) return;
    if (!macrosPayloadCache) {
        grid.innerHTML = '';
        return;
    }

    const layout = macrosSelectorLayout || MACROS_SELECTOR_LAYOUT_DEFAULT;
    const cols = Math.max(1, parseInt(layout.columns, 10) || 3);
    const cells = Array.isArray(layout.cells) ? layout.cells : MACROS_SELECTOR_LAYOUT_DEFAULT.cells;

    // Apply columns from layout spec.
    grid.style.gridTemplateColumns = `repeat(${cols}, 1fr)`;

    grid.innerHTML = '';
    for (const cell of cells) {
        // Empty spacer cell
        if (cell === null || cell === undefined) {
            const spacer = document.createElement('div');
            spacer.className = 'macro-button macro-button--spacer';
            spacer.setAttribute('aria-hidden', 'true');
            grid.appendChild(spacer);
            continue;
        }

        const slotIndex = parseInt(cell, 10);
        if (!(slotIndex >= 0 && slotIndex < MACROS_BUTTONS_PER_SCREEN)) {
            const spacer = document.createElement('div');
            spacer.className = 'macro-button macro-button--spacer';
            spacer.setAttribute('aria-hidden', 'true');
            grid.appendChild(spacer);
            continue;
        }

        const cfg = macrosPayloadCache.screens[macrosSelectedScreen].buttons[slotIndex];
        const btn = document.createElement('button');
        btn.type = 'button';
        btn.className = 'macro-button';
        btn.dataset.slot = String(slotIndex);

        const title = document.createElement('div');
        title.className = 'macro-button-title';
        title.textContent = macrosSlotTitle(slotIndex);

        const subtitle = document.createElement('div');
        subtitle.className = 'macro-button-subtitle';
        const label = (cfg && cfg.label) ? cfg.label : '';
        const action = (cfg && cfg.action) ? cfg.action : 'none';

        // Keep it user-friendly: show a short hint when there is no label.
        if (label) {
            subtitle.textContent = label;
        } else if (action === 'none') {
            subtitle.textContent = '—';
        } else if (action === 'send_keys') {
            subtitle.textContent = 'Keys Script';
        } else if (action === 'nav_prev') {
            subtitle.textContent = 'Previous';
        } else if (action === 'nav_next') {
            subtitle.textContent = 'Next';
        } else {
            subtitle.textContent = action;
        }

        btn.appendChild(title);
        btn.appendChild(subtitle);

        if ((cfg && cfg.action) === 'none') btn.classList.add('none');
        if (slotIndex === macrosSelectedButton) btn.classList.add('active');

        btn.addEventListener('click', () => {
            macrosSelectedButton = slotIndex;
            macrosRenderAll();
        });

        grid.appendChild(btn);
    }
}

function macrosRenderEditorFields() {
    const cfg = macrosGetSelectedButton();
    const labelEl = document.getElementById('macro_label');
    const actionEl = document.getElementById('macro_action');
    const scriptEl = document.getElementById('macro_script');
    const scriptGroupEl = document.getElementById('macro_script_group');
    const iconEl = document.getElementById('macro_icon_id');

    if (!cfg) {
        if (labelEl) labelEl.value = '';
        if (actionEl) actionEl.value = 'none';
        if (scriptEl) scriptEl.value = '';
        if (iconEl) iconEl.value = '';
        if (scriptGroupEl) scriptGroupEl.style.display = 'none';
        macrosUpdateScriptCharCounter();
        return;
    }

    if (labelEl) labelEl.value = cfg.label || '';
    if (actionEl) actionEl.value = cfg.action || 'none';
    if (scriptEl) scriptEl.value = cfg.script || '';
    if (iconEl) iconEl.value = cfg.icon_id || '';

    const action = (cfg.action || 'none');
    const scriptEnabled = macrosActionSupportsScript(action);

    // Script UI: only show it for Keys Script action.
    if (scriptGroupEl) scriptGroupEl.style.display = scriptEnabled ? 'block' : 'none';
    if (scriptEl) {
        scriptEl.disabled = false;
        if (!scriptEnabled) scriptEl.value = '';
    }

    macrosUpdateScriptCharCounter();

    if (iconEl) {
        // Keep editable so users can pre-assign stable IDs, but grey it out for None.
        iconEl.disabled = (action === 'none');
        if (action === 'none') iconEl.value = '';
    }
}

function macrosRenderAll() {
    macrosRenderScreenSelect();
    macrosRenderButtonGrid();
    macrosRenderEditorFields();
}

function macrosBindEditorEvents() {
    const screenSelect = document.getElementById('macro_screen_select');
    if (screenSelect) {
        screenSelect.addEventListener('change', () => {
            macrosSelectedScreen = parseInt(screenSelect.value, 10) || 0;
            macrosSelectedButton = 0;
            macrosRenderAll();
        });
    }

    const labelEl = document.getElementById('macro_label');
    if (labelEl) {
        labelEl.addEventListener('input', () => {
            const cfg = macrosGetSelectedButton();
            if (!cfg) return;
            cfg.label = macrosClampString(labelEl.value, MACROS_LABEL_MAX);
            if (labelEl.value !== cfg.label) labelEl.value = cfg.label;
            macrosSetDirty(true);
            macrosRenderButtonGrid();
        });
    }

    const actionEl = document.getElementById('macro_action');
    if (actionEl) {
        actionEl.addEventListener('change', () => {
            const cfg = macrosGetSelectedButton();
            if (!cfg) return;
            cfg.action = actionEl.value || 'none';

            // Keep stored data tidy.
            if (!macrosActionSupportsScript(cfg.action)) cfg.script = '';
            if (cfg.action === 'none') cfg.icon_id = '';

            macrosSetDirty(true);
            macrosRenderAll();
        });
    }

    const scriptEl = document.getElementById('macro_script');
    if (scriptEl) {
        // Guard against odd captive-browser/mobile behavior that sometimes treats Enter as form-submit.
        // We still want newlines in the textarea, so do NOT preventDefault.
        scriptEl.addEventListener('keydown', (e) => {
            if (e.key === 'Enter') {
                e.stopPropagation();
            }
        });

        scriptEl.addEventListener('input', () => {
            const cfg = macrosGetSelectedButton();
            if (!cfg) return;
            cfg.script = macrosClampString(scriptEl.value, MACROS_SCRIPT_MAX);
            if (scriptEl.value !== cfg.script) scriptEl.value = cfg.script;
            macrosSetDirty(true);
            macrosUpdateScriptCharCounter();
        });
    }

    const iconEl = document.getElementById('macro_icon_id');
    if (iconEl) {
        iconEl.addEventListener('input', () => {
            const cfg = macrosGetSelectedButton();
            if (!cfg) return;
            cfg.icon_id = macrosClampString(iconEl.value, MACROS_ICON_ID_MAX);
            if (iconEl.value !== cfg.icon_id) iconEl.value = cfg.icon_id;
            macrosSetDirty(true);
        });
    }

}

async function loadMacros() {
    if (macrosLoading) return;
    const section = document.getElementById('macros-section');
    if (!section) return;

    macrosLoading = true;
    try {
        const response = await fetch(API_MACROS, { cache: 'no-cache' });
        if (!response.ok) {
            if (response.status === 404) {
                section.style.display = 'none';
                return;
            }
            throw new Error(`Failed to load macros (${response.status})`);
        }

        const payload = await response.json();
        if (payload && Array.isArray(payload.screens) && payload.screens.length > 0) {
            macrosScreenCount = payload.screens.length;
        } else {
            macrosScreenCount = MACROS_SCREEN_COUNT_DEFAULT;
        }
        macrosPayloadCache = macrosNormalizePayload(payload);
        macrosSelectedScreen = 0;
        macrosSelectedButton = 0;
        macrosSetDirty(false);
        macrosRenderAll();
    } catch (error) {
        console.error('Error loading macros:', error);
        showMessage('Error loading macros: ' + error.message, 'error');
        // Fall back to empty editor so UI still works.
        macrosPayloadCache = macrosCreateEmptyPayload();
        macrosRenderAll();
    } finally {
        macrosLoading = false;
    }
}

function macrosValidatePayload(payload) {
    if (!payload || !Array.isArray(payload.screens) || payload.screens.length !== macrosScreenCount) {
        return { valid: false, message: `Invalid payload (expected ${macrosScreenCount} screens)` };
    }
    for (let s = 0; s < macrosScreenCount; s++) {
        const screen = payload.screens[s];
        if (!screen || !Array.isArray(screen.buttons) || screen.buttons.length !== MACROS_BUTTONS_PER_SCREEN) {
            return { valid: false, message: `Invalid payload (screen ${s + 1} must have ${MACROS_BUTTONS_PER_SCREEN} buttons)` };
        }
    }
    return { valid: true, message: 'OK' };
}

async function saveMacros(options = {}) {
    if (!macrosPayloadCache) return;

    const silent = options.silent === true;

    // Normalize + clamp before sending.
    const payload = macrosNormalizePayload(macrosPayloadCache);
    for (let s = 0; s < macrosScreenCount; s++) {
        for (let b = 0; b < MACROS_BUTTONS_PER_SCREEN; b++) {
            const btn = payload.screens[s].buttons[b];
            btn.label = macrosClampString(btn.label, MACROS_LABEL_MAX);
            btn.action = (btn.action || 'none');
            btn.script = macrosClampString(btn.script, MACROS_SCRIPT_MAX);
            btn.icon_id = macrosClampString(btn.icon_id, MACROS_ICON_ID_MAX);

            if (!macrosActionSupportsScript(btn.action)) btn.script = '';
            if (btn.action === 'none') btn.icon_id = '';
        }
    }

    const check = macrosValidatePayload(payload);
    if (!check.valid) {
        if (!silent) showMessage(check.message, 'error');
        return false;
    }

    try {
        const response = await fetch(API_MACROS, {
            method: 'POST',
            headers: { 'Content-Type': 'application/json' },
            body: JSON.stringify(payload),
        });

        const data = await response.json().catch(() => ({}));
        if (!response.ok || data.success === false) {
            throw new Error(data.message || `Save failed (${response.status})`);
        }

        macrosPayloadCache = payload;
        macrosSetDirty(false);
        macrosRenderAll();
        if (!silent) showMessage('Macros saved', 'success');
        return true;
    } catch (error) {
        console.error('Error saving macros:', error);
        if (!silent) showMessage('Error saving macros: ' + error.message, 'error');
        return false;
    }
}

async function saveMacrosIfNeeded(options = {}) {
    const section = document.getElementById('macros-section');
    if (!section || section.style.display === 'none') return true;
    if (!macrosDirty) return true;
    return await saveMacros({ silent: options.silent === true });
}

function initMacrosEditor() {
    const section = document.getElementById('macros-section');
    if (!section) return;

    macrosBindEditorEvents();

    // Start with an empty payload so the UI is responsive immediately.
    // This uses the default screen count until /api/macros tells us otherwise.
    macrosScreenCount = MACROS_SCREEN_COUNT_DEFAULT;
    macrosPayloadCache = macrosCreateEmptyPayload();
    macrosRenderAll();

    loadMacros();
}

/**
 * Scroll input into view when focused (prevents mobile keyboard from covering it)
 * @param {Event} event - Focus event
 */
function handleInputFocus(event) {
    // Small delay to let the keyboard animation start
    setTimeout(() => {
        const input = event.target;
        const rect = input.getBoundingClientRect();
        const viewportHeight = window.innerHeight;
        
        // Estimate keyboard height (typically 40-50% of viewport on mobile)
        const estimatedKeyboardHeight = viewportHeight * 0.45;
        const availableHeight = viewportHeight - estimatedKeyboardHeight;
        
        // Calculate if input would be covered by keyboard
        const inputBottom = rect.bottom;
        
        // Only scroll if the input would be covered by the keyboard
        if (inputBottom > availableHeight) {
            // Scroll just enough to show the input with some padding
            const padding = 20; // 20px padding above input
            const scrollAmount = inputBottom - availableHeight + padding;
            
            window.scrollTo({
                top: window.scrollY + scrollAmount,
                behavior: 'smooth'
            });
        }
    }, 300); // Wait for keyboard animation
}

/**
 * Detect current page and highlight active navigation tab
 */
function initNavigation() {
    const path = window.location.pathname;
    
    if (path === '/' || path === '/home.html') {
        currentPage = 'home';
    } else if (path === '/network.html') {
        currentPage = 'network';
    } else if (path === '/firmware.html') {
        currentPage = 'firmware';
    }
    
    // Highlight active tab
    document.querySelectorAll('.nav-tab').forEach(tab => {
        const page = tab.getAttribute('data-page');
        if (page === currentPage) {
            tab.classList.add('active');
        } else {
            tab.classList.remove('active');
        }
    });
}

/**
 * Display a message to the user
 * @param {string} message - Message text
 * @param {string} type - Message type: 'info', 'success', or 'error'
 */
function showMessage(message, type = 'info') {
    const statusDiv = document.getElementById('status-message');
    statusDiv.textContent = message;
    statusDiv.className = `message ${type}`;
    statusDiv.style.display = 'block';
    
    setTimeout(() => {
        statusDiv.style.display = 'none';
    }, 5000);
}

// ... (rest of file unchanged; included fully in workspace)