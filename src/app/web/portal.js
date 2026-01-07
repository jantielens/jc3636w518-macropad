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
const API_ICONS = '/api/icons';

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
const MACROS_BUTTONS_PER_SCREEN_DEFAULT = 16;
let macrosButtonsPerScreen = MACROS_BUTTONS_PER_SCREEN_DEFAULT;

// Macro button selector visualization (Option B / KISS): a small grid layout spec.
// This keeps today's 3×3 macropad layout but makes it data-driven for future templates.
// Cells contain 0-based slot indices (or null for empty spaces).
const MACROS_SELECTOR_LAYOUT_DEFAULT = {
    columns: 4,
    cells: [
        0, 1, 2, 3,
        4, 5, 6, 7,
        8, 9, 10, 11,
        12, 13, 14, 15,
    ],
};
let macrosSelectorLayout = MACROS_SELECTOR_LAYOUT_DEFAULT;

// Keep in sync with src/app/macros_config.h (char arrays include NUL terminator).
const MACROS_LABEL_MAX = 15;
const MACROS_SCRIPT_MAX = 255;
const MACROS_ICON_ID_MAX = 31;

// Keep in sync with src/app/macro_templates.h (macro_templates::default_id()).
const MACROS_DEFAULT_TEMPLATE_ID = 'round_ring_9';

let macrosPayloadCache = null; // { screens: [ { buttons: [ {label, action, script, icon_id}, ... ] }, ... ] }
let macrosTemplatesCache = []; // [{id,name,selector_layout}, ...]
let macrosSelectedScreen = 0; // 0-based
let macrosSelectedButton = 0; // 0-based
let macrosDirty = false;
let macrosLoading = false;

// ===== ICONS (Home) =====
let iconListCache = null; // [{id, kind}, ...]

function iconsRenderDatalist() {
    const dl = document.getElementById('macro_icon_id_list');
    if (!dl) return;

    dl.innerHTML = '';

    const icons = Array.isArray(iconListCache) ? iconListCache : [];
    for (const icon of icons) {
        if (!icon || !icon.id) continue;
        const opt = document.createElement('option');
        opt.value = String(icon.id);
        dl.appendChild(opt);
    }
}

async function loadIcons() {
    const dl = document.getElementById('macro_icon_id_list');
    if (!dl) return; // Not on Home page

    try {
        const response = await fetch(API_ICONS);
        if (!response.ok) throw new Error(`HTTP ${response.status}`);
        const data = await response.json();
        iconListCache = Array.isArray(data.icons) ? data.icons : [];
        iconsRenderDatalist();
    } catch (error) {
        console.warn('Failed to load icons list:', error);
        iconListCache = [];
        iconsRenderDatalist();
    }
}

function macrosGetSelectedTemplateId() {
    if (!macrosPayloadCache) return '';
    const s = macrosPayloadCache.screens[macrosSelectedScreen];
    return (s && s.template) ? String(s.template) : '';
}

function macrosFindTemplateById(id) {
    const key = (id || '').toString();
    return macrosTemplatesCache.find(t => t && t.id === key) || null;
}

function macrosApplyTemplateLayout() {
    const tpl = macrosFindTemplateById(macrosGetSelectedTemplateId());
    if (tpl && tpl.selector_layout) {
        macrosSelectorLayout = tpl.selector_layout;
    } else {
        macrosSelectorLayout = MACROS_SELECTOR_LAYOUT_DEFAULT;
    }
}

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
        for (let b = 0; b < macrosButtonsPerScreen; b++) {
            buttons.push(macrosCreateEmptyButton());
        }
        payload.screens.push({ template: MACROS_DEFAULT_TEMPLATE_ID, buttons });
    }
    return payload;
}

function macrosNormalizePayload(payload) {
    const out = macrosCreateEmptyPayload();
    if (!payload || !Array.isArray(payload.screens)) return out;

    if (Array.isArray(payload.templates)) {
        macrosTemplatesCache = payload.templates;
    }

    for (let s = 0; s < Math.min(macrosScreenCount, payload.screens.length); s++) {
        const screen = payload.screens[s];
        if (!screen || !Array.isArray(screen.buttons)) continue;

        if (screen.template) {
            out.screens[s].template = String(screen.template);
        }

        for (let b = 0; b < Math.min(macrosButtonsPerScreen, screen.buttons.length); b++) {
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

function macrosRenderTemplateSelect() {
    const select = document.getElementById('macro_template_select');
    if (!select) return;

    // Ensure we have something to show.
    const templates = Array.isArray(macrosTemplatesCache) && macrosTemplatesCache.length > 0
        ? macrosTemplatesCache
        : [{ id: MACROS_DEFAULT_TEMPLATE_ID, name: 'Round Ring (9)' }];

    // Rebuild options if needed.
    if (select.options.length !== templates.length) {
        select.innerHTML = '';
        for (const t of templates) {
            if (!t || !t.id) continue;
            const opt = document.createElement('option');
            opt.value = String(t.id);
            opt.textContent = t.name ? String(t.name) : String(t.id);
            select.appendChild(opt);
        }
    }

    const current = macrosGetSelectedTemplateId() || MACROS_DEFAULT_TEMPLATE_ID;
    select.value = current;
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
    if (macrosButtonsPerScreen === 9 && slot === 9) return 'Center (#9)';
    return `#${slot}`;
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

    // UX: while /api/macros is loading, avoid flashing the default 4×4 selector.
    // Show a minimal placeholder instead.
    if (!macrosPayloadCache) {
        if (macrosLoading) {
            const cols = Math.max(1, parseInt((MACROS_SELECTOR_LAYOUT_DEFAULT || {}).columns, 10) || 4);
            grid.style.gridTemplateColumns = `repeat(${cols}, 1fr)`;
            grid.innerHTML = '';

            const loading = document.createElement('div');
            loading.className = 'macro-button macro-button--spacer';
            loading.style.gridColumn = '1 / -1';
            loading.style.padding = '12px';
            loading.style.textAlign = 'center';
            loading.style.opacity = '0.75';
            loading.textContent = 'Loading macros…';
            grid.appendChild(loading);
            return;
        }

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
        if (!(slotIndex >= 0 && slotIndex < macrosButtonsPerScreen)) {
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
    macrosApplyTemplateLayout();
    macrosRenderScreenSelect();
    macrosRenderTemplateSelect();
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

    const templateSelect = document.getElementById('macro_template_select');
    if (templateSelect) {
        templateSelect.addEventListener('change', () => {
            if (!macrosPayloadCache) return;
            const screen = macrosPayloadCache.screens[macrosSelectedScreen];
            if (!screen) return;
            screen.template = templateSelect.value || MACROS_DEFAULT_TEMPLATE_ID;
            macrosSelectedButton = 0;
            macrosSetDirty(true);
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

        if (payload && Array.isArray(payload.templates)) {
            macrosTemplatesCache = payload.templates;
        }

        // Keep the editor in sync with the firmware.
        // Preferred: buttons_per_screen (firmware v2+). Fallback: infer from first screen.
        if (payload && typeof payload.buttons_per_screen === 'number' && payload.buttons_per_screen > 0) {
            macrosButtonsPerScreen = payload.buttons_per_screen;
        } else if (payload && Array.isArray(payload.screens) && payload.screens[0] && Array.isArray(payload.screens[0].buttons)) {
            macrosButtonsPerScreen = payload.screens[0].buttons.length;
        } else {
            macrosButtonsPerScreen = MACROS_BUTTONS_PER_SCREEN_DEFAULT;
        }
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
        if (!screen || !Array.isArray(screen.buttons) || screen.buttons.length !== macrosButtonsPerScreen) {
            return { valid: false, message: `Invalid payload (screen ${s + 1} must have ${macrosButtonsPerScreen} buttons)` };
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
        for (let b = 0; b < macrosButtonsPerScreen; b++) {
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

    // While /api/macros is loading, render a minimal placeholder (no default grid flash).
    macrosScreenCount = MACROS_SCREEN_COUNT_DEFAULT;
    macrosButtonsPerScreen = MACROS_BUTTONS_PER_SCREEN_DEFAULT;
    macrosPayloadCache = null;

    // Kick off load first so macrosLoading becomes true synchronously.
    loadMacros();
    loadIcons();
    macrosRenderAll();
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

/**
 * Show unified reboot overlay and handle reconnection
 * @param {Object} options - Configuration options
 * @param {string} options.title - Dialog title (e.g., 'Device Rebooting')
 * @param {string} options.message - Main message to display
 * @param {string} options.context - Context: 'save', 'ota', 'reboot', 'reset'
 * @param {string} options.newDeviceName - Optional new device name if changed
 * @param {boolean} options.showProgress - Show progress bar (for OTA)
 */
function showRebootDialog(options) {
    const {
        title = 'Device Rebooting',
        message = 'Please wait while the device restarts...',
        context = 'reboot',
        newDeviceName = null,
        showProgress = false
    } = options;

    const overlay = document.getElementById('reboot-overlay');
    const titleElement = document.getElementById('reboot-title');
    const rebootMsg = document.getElementById('reboot-message');
    const rebootSubMsg = document.getElementById('reboot-submessage');
    const reconnectStatus = document.getElementById('reconnect-status');
    const progressContainer = document.getElementById('reboot-progress-container');
    const spinner = document.getElementById('reboot-spinner');

    // Robustness: if the overlay template isn't present for some reason, fail gracefully.
    if (!overlay || !titleElement || !rebootMsg || !rebootSubMsg || !reconnectStatus) {
        console.error('Reboot overlay elements missing; cannot show reboot dialog');
        try {
            alert(message);
        } catch (_) {
            // ignore
        }
        return;
    }

    // Set dialog content
    titleElement.textContent = title;
    rebootMsg.textContent = message;
    
    // Show/hide progress bar
    if (progressContainer) {
        progressContainer.style.display = showProgress ? 'block' : 'none';
    }
    
    // Show/hide spinner
    if (spinner) {
        spinner.style.display = showProgress ? 'none' : 'block';
    }
    
    // Handle AP mode reset (no auto-reconnect)
    if (context === 'reset') {
        rebootSubMsg.textContent = 'Device will restart in AP mode. You must manually reconnect to the WiFi access point.';
        reconnectStatus.style.display = 'none';
        overlay.style.display = 'flex';
        return; // Don't start polling for AP mode
    }
    
    // Handle OTA (no auto-reconnect yet - wait for upload to complete)
    if (context === 'ota') {
        rebootSubMsg.textContent = 'Uploading firmware...';
        reconnectStatus.style.display = 'none';
        overlay.style.display = 'flex';
        return; // Don't start polling yet - OTA handler will start it after upload
    }
    
    // For save/reboot cases, show best-effort reconnection message and start polling
    const targetAddress = newDeviceName ? `http://${sanitizeForMDNS(newDeviceName)}.local` : window.location.origin;

    // Special case: when saving from AP/core mode, the client usually must switch WiFi networks.
    // Automatic polling from this browser session is unlikely to succeed until the user reconnects.
    if (context === 'save' && (portalMode === 'core' || isInCaptivePortal())) {
        rebootSubMsg.innerHTML = `Device will restart and may switch networks.<br>` +
            `<small style="color: #888; margin-top: 8px; display: block;">` +
            `Reconnect your phone/PC to the configured WiFi, then open: ` +
            `<code style="color: #667eea; font-weight: 600;">${targetAddress}</code>` +
            `</small>`;
        reconnectStatus.style.display = 'none';
        overlay.style.display = 'flex';
        return;
    }

    rebootSubMsg.innerHTML = `Attempting automatic reconnection...<br><small style="color: #888; margin-top: 8px; display: block;">If this fails, manually navigate to: <code style="color: #667eea; font-weight: 600;">${targetAddress}</code></small>`;
    reconnectStatus.style.display = 'block';

    overlay.style.display = 'flex';

    // Start unified reconnection process
    startReconnection({
        context,
        newDeviceName,
        statusElement: reconnectStatus,
        messageElement: rebootMsg
    });
}

/**
 * Detect if running in a captive portal browser
 * @returns {boolean} True if in captive portal
 */
function isInCaptivePortal() {
    const ua = window.navigator.userAgent;
    
    // Android captive portal indicators
    if (ua.includes('Android')) {
        if (ua.includes('CaptiveNetworkSupport') || 
            ua.includes('wv') || // WebView indicator
            document.referrer.includes('captiveportal')) {
            return true;
        }
    }
    
    // iOS captive portal
    if (ua.includes('iPhone') || ua.includes('iPad')) {
        if (ua.includes('CaptiveNetworkSupport')) {
            return true;
        }
    }
    
    return false;
}

/**
 * Generate sanitized mDNS name from device name
 * @param {string} deviceName - Device name to sanitize
 * @returns {string} Sanitized mDNS hostname
 */
function sanitizeForMDNS(deviceName) {
    return deviceName.toLowerCase()
        .replace(/[^a-z0-9\s\-_]/g, '')
        .replace(/[\s_]+/g, '-')
        .replace(/-+/g, '-')
        .replace(/^-|-$/g, '');
}

/**
 * Show captive portal warning with device address and handle user response
 */
function showCaptivePortalWarning() {
    const modal = document.getElementById('captive-portal-warning');
    const deviceName = document.getElementById('device_name').value.trim();
    const mdnsName = sanitizeForMDNS(deviceName);
    const deviceUrl = `http://${mdnsName}.local`;
    
    // Show the device address
    document.getElementById('device-mdns-address').textContent = deviceUrl;
    modal.style.display = 'flex';
    
    // Continue button - proceed with save
    document.getElementById('continue-save-btn').onclick = () => {
        modal.style.display = 'none';
        // Re-trigger the save (flag already set, so it will proceed)
        document.getElementById('config-form').dispatchEvent(new Event('submit'));
    };
    
    // Cancel button
    document.getElementById('cancel-save-btn').onclick = () => {
        modal.style.display = 'none';
        window.captivePortalWarningShown = false; // Reset flag if cancelled
    };
}

/**
 * Unified reconnection logic for all reboot scenarios
 * @param {Object} options - Reconnection options
 * @param {string} options.context - Context: 'save', 'ota', 'reboot'
 * @param {string} options.newDeviceName - Optional new device name if changed
 * @param {HTMLElement} options.statusElement - Status message element
 * @param {HTMLElement} options.messageElement - Main message element
 */
async function startReconnection(options) {
    const { context, newDeviceName, statusElement, messageElement } = options;
    
    // Initial delay: device needs time to start rebooting
    await new Promise(resolve => setTimeout(resolve, 2000));
    
    let attempts = 0;
    const maxAttempts = 40; // 2s initial + (40 × 3s) = 122 seconds total
    const checkInterval = 3000; // Poll every 3 seconds
    
    // Determine target URL
    let targetUrl = null;
    if (newDeviceName) {
        const mdnsName = sanitizeForMDNS(newDeviceName);
        targetUrl = `http://${mdnsName}.local`;
    }
    
    const checkConnection = async () => {
        attempts++;
        
        // Try new address first (if device name changed), then current location as fallback
        const urlsToTry = targetUrl 
            ? [targetUrl + API_VERSION, window.location.origin + API_VERSION]
            : [window.location.origin + API_VERSION];
        
        // Update status with progress
        const elapsed = 2 + (attempts * 3);
        statusElement.textContent = `Checking connection (attempt ${attempts}/${maxAttempts}, ${elapsed}s elapsed)...`;
        
        for (const url of urlsToTry) {
            try {
                const response = await fetch(url, { 
                    cache: 'no-cache',
                    mode: 'cors',
                    signal: AbortSignal.timeout(3000)
                });
                
                if (response.ok) {
                    messageElement.textContent = 'Device is back online!';
                    statusElement.textContent = 'Redirecting...';
                    const redirectUrl = targetUrl || window.location.origin;
                    setTimeout(() => {
                        window.location.href = redirectUrl;
                    }, 1000);
                    return;
                }
            } catch (e) {
                // Connection failed, try next URL
                console.debug(`Connection attempt ${attempts} failed for ${url}:`, e.message);
            }
        }
        
        // All URLs failed, continue trying
        if (attempts < maxAttempts) {
            setTimeout(checkConnection, checkInterval);
        } else {
            // Timeout - provide manual fallback
            const fallbackUrl = targetUrl || window.location.origin;
            messageElement.textContent = 'Automatic reconnection failed';
            statusElement.innerHTML = 
                `<div style="color:#e74c3c; margin-bottom: 10px;">Could not reconnect after ${2 + (maxAttempts * 3)} seconds.</div>` +
                `<div style="margin-top: 10px;">Please manually navigate to:<br>` +
                `<a href="${fallbackUrl}" style="color:#667eea; font-weight: 600; font-size: 16px;">${fallbackUrl}</a></div>` +
                `<div style="margin-top: 15px; font-size: 13px; color: #888;">` +
                `Possible issues: WiFi connection failed, incorrect credentials, or device taking longer to boot.</div>`;
        }
    };
    
    checkConnection();
}

/**
 * Update sanitized device name field
 */
function updateSanitizedName() {
    const deviceNameField = document.getElementById('device_name');
    const sanitizedField = document.getElementById('device_name_sanitized');
    
    // Only proceed if both elements exist
    if (!deviceNameField || !sanitizedField) return;
    
    const deviceName = deviceNameField.value;
    
    // Sanitize: lowercase, alphanumeric + hyphens
    let sanitized = deviceName.toLowerCase()
        .replace(/[^a-z0-9\s\-_]/g, '')
        .replace(/[\s_]+/g, '-')
        .replace(/-+/g, '-')
        .replace(/^-|-$/g, '');
    
    sanitizedField.textContent = (sanitized || 'esp32-xxxx') + '.local';
}

/**
 * Load portal mode (core vs full)
 */
async function loadMode() {
    try {
        const response = await fetch(API_MODE);
        if (!response.ok) return;
        
        const mode = await response.json();
        portalMode = mode.mode || 'full';
        
        // Show/hide additional settings based on mode (only if element exists)
        const additionalSettings = document.getElementById('additional-settings');
        if (additionalSettings) {
            if (portalMode === 'core') {
                additionalSettings.style.display = 'none';
            } else {
                additionalSettings.style.display = 'block';
            }
        }
        
        // Hide Home and Firmware navigation buttons in AP mode (core mode)
        if (portalMode === 'core') {
            document.querySelectorAll('.nav-tab[data-page="home"], .nav-tab[data-page="firmware"]').forEach(tab => {
                tab.style.display = 'none';
            });
            
            // Show setup notice on network page
            const setupNotice = document.getElementById('setup-notice');
            if (setupNotice) {
                setupNotice.style.display = 'block';
            }
            
            // Hide unnecessary buttons on network page (only "Save and Reboot" makes sense)
            const saveOnlyBtn = document.getElementById('save-only-btn');
            const rebootBtn = document.getElementById('reboot-btn');
            if (saveOnlyBtn) saveOnlyBtn.style.display = 'none';
            if (rebootBtn) rebootBtn.style.display = 'none';
            
            // Change primary button text to be more intuitive
            const submitBtn = document.querySelector('#config-form button[type="submit"]');
            if (submitBtn) {
                submitBtn.textContent = 'Save & Connect';
            }

            // Hide security settings in AP/core mode
            // (auth is intentionally disabled during onboarding/recovery)
            const securitySection = document.getElementById('security-section');
            if (securitySection) {
                securitySection.style.display = 'none';
                securitySection.querySelectorAll('input, select, textarea').forEach(el => {
                    el.disabled = true;
                });
            }
        }
    } catch (error) {
        console.error('Error loading mode:', error);
    }
}

/**
 * Load and display version information
 */
async function loadVersion() {
    try {
        const response = await fetch(API_INFO);
        if (!response.ok) return;
        
        const version = await response.json();
        deviceInfoCache = version;

        // Strategy B: Hide/disable MQTT settings if firmware was built without MQTT support
        const mqttSection = document.getElementById('mqtt-settings-section');
        if (mqttSection && version.has_mqtt === false) {
            mqttSection.style.display = 'none';
            mqttSection.querySelectorAll('input, select, textarea').forEach(el => {
                el.disabled = true;
            });
        }

        // Hide/disable display settings if firmware was built without backlight support
        const displaySection = document.getElementById('display-settings-section');
        if (displaySection) {
            if (version.has_backlight === true || version.has_display === true) {
                displaySection.style.display = 'block';
            } else {
                displaySection.style.display = 'none';
                displaySection.querySelectorAll('input').forEach(el => {
                    el.disabled = true;
                });
            }
        }
        
        // Populate screen selection dropdown if device has display
        const screenSelect = document.getElementById('screen_selection');
        const screenGroup = document.getElementById('screen-selection-group');
        if (screenSelect && screenGroup && version.has_display === true && version.available_screens) {
            // Clear existing options
            screenSelect.innerHTML = '';
            
            // Add option for each available screen
            version.available_screens.forEach(screen => {
                const option = document.createElement('option');
                option.value = screen.id;
                option.textContent = screen.name;
                if (screen.id === version.current_screen) {
                    option.selected = true;
                }
                screenSelect.appendChild(option);
            });
            
            // Show screen selection group
            screenGroup.style.display = 'block';
        } else if (screenGroup) {
            screenGroup.style.display = 'none';
        }

        document.getElementById('firmware-version').textContent = `Firmware v${version.version}`;
        document.getElementById('chip-info').textContent = 
            `${version.chip_model} rev ${version.chip_revision}`;
        document.getElementById('cpu-cores').textContent = 
            `${version.chip_cores} ${version.chip_cores === 1 ? 'Core' : 'Cores'}`;
        document.getElementById('cpu-freq').textContent = `${version.cpu_freq} MHz`;
        document.getElementById('flash-size').textContent = 
            `${(version.flash_chip_size / 1048576).toFixed(0)} MB Flash`;
        document.getElementById('psram-status').textContent = 
            version.psram_size > 0 ? `${(version.psram_size / 1048576).toFixed(0)} MB PSRAM` : 'No PSRAM';

        // Update Firmware page online update UI if present
        updateGitHubUpdateSection(version);
    } catch (error) {
        document.getElementById('firmware-version').textContent = 'Firmware v?.?.?';
        document.getElementById('chip-info').textContent = 'Chip info unavailable';
        document.getElementById('cpu-cores').textContent = '? Cores';
        document.getElementById('cpu-freq').textContent = '? MHz';
        document.getElementById('flash-size').textContent = '? MB Flash';
        document.getElementById('psram-status').textContent = 'Unknown';

        // Still attempt to update Firmware page UI if present
        updateGitHubUpdateSection(null);
    }
}

function updateGitHubUpdateSection(info) {
    const section = document.getElementById('github-update-section');
    if (!section) return; // Only on firmware page

    const currentEl = document.getElementById('github-current-version');
    const latestEl = document.getElementById('github-latest-version');
    const availabilityEl = document.getElementById('github-update-availability');
    const updateBtn = document.getElementById('github-update-btn');
    const metaEl = document.getElementById('github-update-meta');

    const hasInfo = !!info;
    const currentVersion = hasInfo && info.version ? info.version : '-.-.-';
    if (currentEl) currentEl.textContent = currentVersion;
    if (latestEl && latestEl.textContent.trim() === '') latestEl.textContent = '-.-.-';

    const enabled = !!(hasInfo && info.github_updates_enabled === true);

    const hasRepo = hasInfo && info.github_owner && info.github_repo;
    const repoText = hasRepo ? `${info.github_owner}/${info.github_repo}` : null;

    if (!enabled) {
        if (availabilityEl) availabilityEl.textContent = '—';
        if (updateBtn) updateBtn.disabled = true;
        if (metaEl) {
            metaEl.textContent = repoText
                ? `Online updates require git remote origin metadata at build time. Detected repo: ${repoText}`
                : 'Online updates require git remote origin metadata at build time.';
        }
        return;
    }

    if (updateBtn && availabilityEl && availabilityEl.textContent !== 'Update available') {
        // Will be enabled after a successful latest-check if needed.
        updateBtn.disabled = true;
    }

    if (metaEl) {
        const board = info.board_name || 'unknown';
        // Split across 3 lines (as requested)
        metaEl.innerHTML = `Repo: ${info.github_owner}/${info.github_repo}<br>Board: ${board}<br>Stable releases only`;
    }

    // Auto-check on first load so the page immediately shows update availability.
    if (!githubAutoChecked) {
        githubAutoChecked = true;
        checkGitHubLatestFirmware();
    }
}

async function checkGitHubLatestFirmware() {
    const latestEl = document.getElementById('github-latest-version');
    const availabilityEl = document.getElementById('github-update-availability');
    const updateBtn = document.getElementById('github-update-btn');

    try {
        if (availabilityEl) availabilityEl.textContent = 'Checking…';

        const response = await fetch(API_FIRMWARE_LATEST, { cache: 'no-cache' });
        const data = await response.json().catch(() => ({}));

        if (!response.ok || !data.success) {
            if (availabilityEl) availabilityEl.textContent = 'Check failed';
            showMessage(data.message || 'Failed to check latest firmware', 'error');
            if (updateBtn) updateBtn.disabled = true;
            return;
        }

        if (latestEl) latestEl.textContent = data.latest_version || '-.-.-';

        if (data.update_available) {
            if (availabilityEl) availabilityEl.textContent = 'Update available';
            if (updateBtn) updateBtn.disabled = false;
        } else {
            if (availabilityEl) availabilityEl.textContent = 'Up to date';
            if (updateBtn) updateBtn.disabled = true;
        }
    } catch (error) {
        if (availabilityEl) availabilityEl.textContent = 'Check failed';
        showMessage('Error checking latest firmware: ' + error.message, 'error');
        if (updateBtn) updateBtn.disabled = true;
    }
}

async function startGitHubFirmwareUpdate() {
    const updateBtn = document.getElementById('github-update-btn');

    if (!confirm('Download and install the latest stable firmware from GitHub Releases?')) {
        return;
    }

    if (updateBtn) updateBtn.disabled = true;

    showRebootDialog({
        title: 'Firmware Update',
        message: 'Downloading firmware...',
        context: 'ota',
        showProgress: true
    });

    const overlay = document.getElementById('reboot-overlay');
    const message = document.getElementById('reboot-message');
    const progressFill = document.getElementById('reboot-progress-fill');
    const progressText = document.getElementById('reboot-progress-text');
    const progressContainer = document.getElementById('reboot-progress-container');
    const reconnectStatus = document.getElementById('reconnect-status');

    try {
        const startResp = await fetch(API_FIRMWARE_UPDATE, { method: 'POST' });
        const startData = await startResp.json().catch(() => ({}));

        if (!startResp.ok || !startData.success) {
            throw new Error(startData.message || 'Failed to start update');
        }

        // If device reports up-to-date, stop.
        if (startData.update_started === false) {
            message.textContent = startData.message || 'Already up to date';
            progressContainer.style.display = 'none';
            setTimeout(() => { overlay.style.display = 'none'; }, 2000);
            return;
        }

        message.textContent = 'Downloading firmware...';

        // Poll status until reboot.
        const poll = async () => {
            try {
                const statusResp = await fetch(API_FIRMWARE_UPDATE_STATUS, { cache: 'no-cache' });
                const status = await statusResp.json().catch(() => ({}));

                if (status.state === 'error') {
                    throw new Error(status.error || 'Update failed');
                }

                const total = status.total || 0;
                const progress = status.progress || 0;

                if (total > 0) {
                    const percent = Math.min(100, Math.round((progress / total) * 100));
                    progressFill.style.width = percent + '%';
                    progressText.textContent = percent + '%';
                } else {
                    // Unknown total; show an indeterminate-ish UI.
                    progressText.textContent = '…';
                }

                if (status.state === 'writing') {
                    message.textContent = 'Installing firmware...';
                }

                if (status.state === 'rebooting' || (total > 0 && progress >= total && progress > 0)) {
                    message.textContent = 'Rebooting...';
                    // Switch to reconnection behavior
                    progressContainer.style.display = 'none';
                    document.getElementById('reboot-spinner').style.display = 'block';
                    reconnectStatus.style.display = 'block';
                    document.getElementById('reboot-submessage').innerHTML =
                        `Attempting automatic reconnection...<br><small style="color: #888; margin-top: 8px; display: block;">If this fails, manually navigate to: <code style="color: #667eea; font-weight: 600;">${window.location.origin}</code></small>`;

                    startReconnection({
                        context: 'ota',
                        newDeviceName: null,
                        statusElement: reconnectStatus,
                        messageElement: message
                    });
                    return;
                }

                setTimeout(poll, 750);
            } catch (e) {
                // If polling fails after we've started, the device may already be rebooting.
                console.debug('Update status polling error:', e.message);

                message.textContent = 'Rebooting...';
                progressContainer.style.display = 'none';
                document.getElementById('reboot-spinner').style.display = 'block';
                reconnectStatus.style.display = 'block';
                startReconnection({
                    context: 'ota',
                    newDeviceName: null,
                    statusElement: reconnectStatus,
                    messageElement: message
                });
            }
        };

        poll();
    } catch (error) {
        message.textContent = 'Update failed: ' + error.message;
        progressContainer.style.display = 'none';
        setTimeout(() => { overlay.style.display = 'none'; }, 3500);
        showMessage('Online update failed: ' + error.message, 'error');
    } finally {
        // updateBtn stays disabled until next check.
    }
}

/**
 * Load current configuration from device
 */
async function loadConfig() {
    try {
        
        const response = await fetch(API_CONFIG);
        if (!response.ok) {
            throw new Error('Failed to load configuration');
        }
        
        const config = await response.json();
        // Cache for validation logic (e.g., whether passwords are already set)
        window.deviceConfig = config;
        const hasConfig = config.wifi_ssid && config.wifi_ssid !== '';
        
        // Helper to safely set element value
        const setValueIfExists = (id, value) => {
            const element = document.getElementById(id);
            if (element) element.value = (value === 0 ? '0' : (value || ''));
        };

        const setCheckedIfExists = (id, checked) => {
            const element = document.getElementById(id);
            if (element && element.type === 'checkbox') {
                element.checked = !!checked;
            }
        };
        
        const setTextIfExists = (id, text) => {
            const element = document.getElementById(id);
            if (element) element.textContent = text;
        };
        
        // WiFi settings
        setValueIfExists('wifi_ssid', config.wifi_ssid);
        const wifiPwdField = document.getElementById('wifi_password');
        if (wifiPwdField) {
            wifiPwdField.value = '';
            wifiPwdField.placeholder = hasConfig ? '(saved - leave blank to keep)' : '';
        }
        
        // Device settings
        setValueIfExists('device_name', config.device_name);
        setTextIfExists('device_name_sanitized', (config.device_name_sanitized || 'esp32-xxxx') + '.local');
        
        // Fixed IP settings
        setValueIfExists('fixed_ip', config.fixed_ip);
        setValueIfExists('subnet_mask', config.subnet_mask);
        setValueIfExists('gateway', config.gateway);
        setValueIfExists('dns1', config.dns1);
        setValueIfExists('dns2', config.dns2);
        
        // Dummy setting
        setValueIfExists('dummy_setting', config.dummy_setting);

        // MQTT settings
        setValueIfExists('mqtt_host', config.mqtt_host);
        setValueIfExists('mqtt_port', config.mqtt_port);
        setValueIfExists('mqtt_username', config.mqtt_username);
        setValueIfExists('mqtt_interval_seconds', config.mqtt_interval_seconds);

        const mqttPwdField = document.getElementById('mqtt_password');
        if (mqttPwdField) {
            mqttPwdField.value = '';
            mqttPwdField.placeholder = hasConfig ? '(saved - leave blank to keep)' : '';
        }

        // Basic Auth settings
        setCheckedIfExists('basic_auth_enabled', config.basic_auth_enabled);
        setValueIfExists('basic_auth_username', config.basic_auth_username);
        const authPwdField = document.getElementById('basic_auth_password');
        if (authPwdField) {
            authPwdField.value = '';
            const saved = config.basic_auth_password_set === true;
            authPwdField.placeholder = saved ? '(saved - leave blank to keep)' : '';
        }
        
        // Display settings - backlight brightness
        const brightness = config.backlight_brightness !== undefined ? config.backlight_brightness : 100;
        setValueIfExists('backlight_brightness', brightness);
        setTextIfExists('brightness-value', brightness);
        updateBrightnessSliderBackground(brightness);

        // Screen saver settings
        setCheckedIfExists('screen_saver_enabled', config.screen_saver_enabled);
        setValueIfExists('screen_saver_timeout_seconds', config.screen_saver_timeout_seconds);
        setValueIfExists('screen_saver_fade_out_ms', config.screen_saver_fade_out_ms);
        setValueIfExists('screen_saver_fade_in_ms', config.screen_saver_fade_in_ms);
        setCheckedIfExists('screen_saver_wake_on_touch', config.screen_saver_wake_on_touch);
        
        // Hide loading overlay (silent load)
        const overlay = document.getElementById('form-loading-overlay');
        if (overlay) overlay.style.display = 'none';
    } catch (error) {
        // Hide loading overlay even on error so form is usable
        const overlay = document.getElementById('form-loading-overlay');
        if (overlay) overlay.style.display = 'none';
        showMessage('Error loading configuration: ' + error.message, 'error');
        console.error('Load error:', error);
    }
}

/**
 * Extract form fields that exist on the current page
 * @param {FormData} formData - Form data to extract from
 * @returns {Object} Configuration object with only fields present on page
 */
function extractFormFields(formData) {
    // Helper to get value only if field exists
    const getFieldValue = (name) => {
        const element = document.querySelector(`[name="${name}"]`);
        if (!element || element.disabled) return null;
        return element ? formData.get(name) : null;
    };

    const getCheckboxValue = (name) => {
        const element = document.querySelector(`[name="${name}"]`);
        if (!element || element.disabled) return null;
        if (element.type !== 'checkbox') return formData.get(name);
        // Explicit boolean so unchecked can be persisted as false.
        return element.checked;
    };
    
    // Build config from only the fields that exist on this page
    const config = {};
    const fields = ['wifi_ssid', 'wifi_password', 'device_name', 'fixed_ip', 
                    'subnet_mask', 'gateway', 'dns1', 'dns2', 'dummy_setting',
                    'mqtt_host', 'mqtt_port', 'mqtt_username', 'mqtt_password', 'mqtt_interval_seconds',
                    'basic_auth_enabled', 'basic_auth_username', 'basic_auth_password',
                    'backlight_brightness',
                    'screen_saver_enabled', 'screen_saver_timeout_seconds', 'screen_saver_fade_out_ms', 'screen_saver_fade_in_ms', 'screen_saver_wake_on_touch'];
    
    fields.forEach(field => {
        const element = document.querySelector(`[name="${field}"]`);
        const value = (element && element.type === 'checkbox') ? getCheckboxValue(field) : getFieldValue(field);
        if (value !== null) config[field] = value;
    });
    
    return config;
}

/**
 * Validate configuration fields
 * @param {Object} config - Configuration object to validate
 * @returns {Object} { valid: boolean, message: string }
 */
function validateConfig(config) {
    // Validate required fields only if they exist on this page
    if (config.wifi_ssid !== undefined && (!config.wifi_ssid || config.wifi_ssid.trim() === '')) {
        return { valid: false, message: 'WiFi SSID is required' };
    }
    
    if (config.device_name !== undefined && (!config.device_name || config.device_name.trim() === '')) {
        return { valid: false, message: 'Device name is required' };
    }
    
    // Validate fixed IP configuration only if on network page
    if (config.fixed_ip !== undefined && config.fixed_ip && config.fixed_ip.trim() !== '') {
        if (!config.subnet_mask || config.subnet_mask.trim() === '') {
            return { valid: false, message: 'Subnet mask is required when using fixed IP' };
        }
        if (!config.gateway || config.gateway.trim() === '') {
            return { valid: false, message: 'Gateway is required when using fixed IP' };
        }
    }

    // Validate Basic Auth only if fields exist on this page
    if (config.basic_auth_enabled === true) {
        const user = (config.basic_auth_username || '').trim();
        const pass = (config.basic_auth_password || '').trim();
        const passwordAlreadySet = !!(window.deviceConfig && window.deviceConfig.basic_auth_password_set === true);

        if (!user) {
            return { valid: false, message: 'Basic Auth username is required when enabled' };
        }
        // Only require a password if none is already set.
        if (!passwordAlreadySet && !pass) {
            return { valid: false, message: 'Basic Auth password is required the first time you enable it' };
        }
    }
    
    return { valid: true };
}

/**
 * Save configuration to device
 * @param {Event} event - Form submit event
 */
async function saveConfig(event) {
    event.preventDefault();
    
    // Check if in captive portal and show warning (only once)
    if (isInCaptivePortal() && !window.captivePortalWarningShown) {
        window.captivePortalWarningShown = true;
        showCaptivePortalWarning();
        return;
    }
    
    const formData = new FormData(event.target);
    const config = extractFormFields(formData);
    
    // Validate configuration
    const validation = validateConfig(config);
    if (!validation.valid) {
        showMessage(validation.message, 'error');
        return;
    }

    // Home page: persist macros as part of the same save workflow.
    // Do this before showing the reboot overlay so we can surface errors.
    const macrosOk = await saveMacrosIfNeeded({ silent: true });
    if (!macrosOk) {
        showMessage('Error saving macros (configuration not saved)', 'error');
        return;
    }
    
    const currentDeviceNameField = document.getElementById('device_name');
    const currentDeviceName = currentDeviceNameField ? currentDeviceNameField.value : null;
    
    // Show overlay immediately
    showRebootDialog({
        title: 'Saving Configuration',
        message: 'Saving configuration...',
        context: 'save',
        newDeviceName: currentDeviceName
    });
    
    try {
        const response = await fetch(API_CONFIG, {
            method: 'POST',
            headers: {
                'Content-Type': 'application/json'
            },
            body: JSON.stringify(config)
        });
        
        if (!response.ok) {
            throw new Error('Failed to save configuration');
        }
        
        const result = await response.json();
        if (result.success) {
            // Update dialog message
            document.getElementById('reboot-message').textContent = 'Configuration saved. Device is rebooting...';
        }
    } catch (error) {
        // If save request fails (e.g., device already rebooting), assume success
        if (error.message.includes('Failed to fetch') || error.message.includes('NetworkError')) {
            document.getElementById('reboot-message').textContent = 'Configuration saved. Device is rebooting...';
        } else {
            // Hide overlay and show error
            document.getElementById('reboot-overlay').style.display = 'none';
            showMessage('Error saving configuration: ' + error.message, 'error');
            console.error('Save error:', error);
        }
    }
}

/**
 * Save configuration without rebooting
 */
async function saveOnly(event) {
    event.preventDefault();

    // Home page: persist macros as part of the same save workflow.
    const macrosOk = await saveMacrosIfNeeded({ silent: true });
    if (!macrosOk) {
        showMessage('Error saving macros (configuration not saved)', 'error');
        return;
    }
    
    const formData = new FormData(document.getElementById('config-form'));
    const config = extractFormFields(formData);
    
    // Validate configuration
    const validation = validateConfig(config);
    if (!validation.valid) {
        showMessage(validation.message, 'error');
        return;
    }
    
    try {
        showMessage('Saving configuration...', 'info');
        
        // Add no_reboot parameter to prevent automatic reboot
        const response = await fetch(API_CONFIG + '?no_reboot=1', {
            method: 'POST',
            headers: {
                'Content-Type': 'application/json'
            },
            body: JSON.stringify(config)
        });
        
        if (!response.ok) {
            throw new Error('Failed to save configuration');
        }
        
        const result = await response.json();
        if (result.success) {
            showMessage('Configuration saved successfully!', 'success');
        } else {
            showMessage('Failed to save configuration', 'error');
        }
    } catch (error) {
        showMessage('Error saving configuration: ' + error.message, 'error');
        console.error('Save error:', error);
    }
}

/**
 * Reboot device without saving
 */
async function rebootDevice() {
    if (!confirm('Reboot the device without saving any changes?')) {
        return;
    }

    // Show unified dialog immediately (do not wait on network)
    showRebootDialog({
        title: 'Device Rebooting',
        message: 'Device is rebooting...',
        context: 'reboot'
    });
    
    try {
        const response = await fetch(API_REBOOT, {
            method: 'POST',
            signal: AbortSignal.timeout(1500)
        });

        // If the device responds with an explicit error, surface it.
        if (!response.ok) {
            throw new Error('Failed to reboot device');
        }
    } catch (error) {
        // Network failure/timeout is expected when the device reboots quickly.
        // Only surface errors that clearly indicate the reboot request was rejected.
        if (error.message && error.message.includes('Failed to reboot device')) {
            const overlay = document.getElementById('reboot-overlay');
            if (overlay) overlay.style.display = 'none';
            showMessage('Error rebooting device: ' + error.message, 'error');
            console.error('Reboot error:', error);
        }
    }
}

/**
 * Reset configuration to defaults
 */
async function resetConfig() {
    if (!confirm('Factory reset will erase all settings and reboot the device into AP mode. Continue?')) {
        return;
    }
    
    // Show unified dialog (no auto-reconnect for AP mode)
    showRebootDialog({
        title: 'Factory Reset',
        message: 'Resetting configuration...',
        context: 'reset'
    });
    
    try {
        const response = await fetch(API_CONFIG, {
            method: 'DELETE'
        });
        
        if (!response.ok) {
            throw new Error('Failed to reset configuration');
        }
        
        const result = await response.json();
        if (result.success) {
            // Update message
            document.getElementById('reboot-message').textContent = 'Configuration reset. Device restarting in AP mode...';
        } else {
            // Hide overlay and show error
            document.getElementById('reboot-overlay').style.display = 'none';
            showMessage('Error: ' + (result.message || 'Unknown error'), 'error');
        }
    } catch (error) {
        // If reset request fails (e.g., device already rebooting), assume success
        if (error.message.includes('Failed to fetch') || error.message.includes('NetworkError')) {
            document.getElementById('reboot-message').textContent = 'Configuration reset. Device restarting in AP mode...';
        } else {
            // Hide overlay and show error
            document.getElementById('reboot-overlay').style.display = 'none';
            showMessage('Error resetting configuration: ' + error.message, 'error');
            console.error('Reset error:', error);
        }
    }
}

/**
 * Handle firmware file selection
 * @param {Event} event - File input change event
 */
function handleFileSelect(event) {
    selectedFile = event.target.files[0];
    const uploadBtn = document.getElementById('upload-btn');
    
    if (selectedFile && selectedFile.name.endsWith('.bin')) {
        uploadBtn.disabled = false;
        showMessage(`Selected: ${selectedFile.name} (${(selectedFile.size / 1024).toFixed(1)} KB)`, 'info');
    } else {
        uploadBtn.disabled = true;
        if (selectedFile) {
            showMessage('Please select a .bin file', 'error');
            selectedFile = null;
        }
    }
}

/**
 * Upload firmware file to device
 */
async function uploadFirmware() {
    if (!selectedFile) {
        showMessage('Please select a firmware file', 'error');
        return;
    }
    
    const uploadBtn = document.getElementById('upload-btn');
    const fileInput = document.getElementById('firmware-file');
    
    uploadBtn.disabled = true;
    fileInput.disabled = true;
    
    // Show unified reboot dialog with progress bar
    showRebootDialog({
        title: 'Firmware Update',
        message: 'Uploading firmware...',
        context: 'ota',
        showProgress: true
    });
    
    const overlay = document.getElementById('reboot-overlay');
    const message = document.getElementById('reboot-message');
    const progressFill = document.getElementById('reboot-progress-fill');
    const progressText = document.getElementById('reboot-progress-text');
    const progressContainer = document.getElementById('reboot-progress-container');
    const reconnectStatus = document.getElementById('reconnect-status');
    
    const formData = new FormData();
    formData.append('firmware', selectedFile);
    
    const xhr = new XMLHttpRequest();
    
    let uploadComplete = false;
    
    xhr.upload.addEventListener('progress', (e) => {
        if (e.lengthComputable) {
            const percent = Math.round((e.loaded / e.total) * 100);
            progressFill.style.width = percent + '%';
            progressText.textContent = percent + '%';
            
            // When upload reaches 95%+, show installing message
            if (percent >= 95 && !uploadComplete) {
                uploadComplete = true;
                message.textContent = 'Installing firmware & rebooting...';
                
                // After a short delay, transition to reconnection
                setTimeout(() => {
                    progressContainer.style.display = 'none';
                    document.getElementById('reboot-spinner').style.display = 'block';
                    reconnectStatus.style.display = 'block';
                    
                    // Start unified reconnection
                    const currentDeviceName = document.getElementById('device_name').value;
                    const targetAddress = window.location.origin;
                    document.getElementById('reboot-submessage').innerHTML = 
                        `Attempting automatic reconnection...<br><small style="color: #888; margin-top: 8px; display: block;">If this fails, manually navigate to: <code style="color: #667eea; font-weight: 600;">${targetAddress}</code></small>`;
                    
                    startReconnection({
                        context: 'ota',
                        newDeviceName: null,
                        statusElement: reconnectStatus,
                        messageElement: message
                    });
                }, 2000);
            }
        }
    });
    
    xhr.addEventListener('load', () => {
        console.log('[OTA] XHR load event, status:', xhr.status);
        // Upload complete handler - may not always fire if device reboots quickly
        if (!uploadComplete && xhr.status === 200) {
            uploadComplete = true;
            progressFill.style.width = '100%';
            progressText.textContent = '100%';
            message.textContent = 'Installing firmware & rebooting...';
            
            setTimeout(() => {
                progressContainer.style.display = 'none';
                document.getElementById('reboot-spinner').style.display = 'block';
                reconnectStatus.style.display = 'block';
                
                // Start unified reconnection
                const currentDeviceName = document.getElementById('device_name').value;
                const targetAddress = window.location.origin;
                document.getElementById('reboot-submessage').innerHTML = 
                    `Attempting automatic reconnection...<br><small style="color: #888; margin-top: 8px; display: block;">If this fails, manually navigate to: <code style="color: #667eea; font-weight: 600;">${targetAddress}</code></small>`;
                
                startReconnection({
                    context: 'ota',
                    newDeviceName: null,
                    statusElement: reconnectStatus,
                    messageElement: message
                });
            }, 2000);
        }
    });
    
    xhr.addEventListener('error', () => {
        console.log('[OTA] XHR error event, uploadComplete:', uploadComplete);
        // Network error - if upload was near complete, assume device is rebooting
        if (uploadComplete) {
            console.log('[OTA] Upload was complete, treating error as device rebooting');
            // Already handled in progress event
        } else {
            console.log('[OTA] Upload failed with network error');
            message.textContent = 'Upload failed: Network error';
            progressContainer.style.display = 'none';
            uploadBtn.disabled = false;
            fileInput.disabled = false;
            
            // Close dialog after 3 seconds
            setTimeout(() => {
                overlay.style.display = 'none';
            }, 3000);
        }
    });
    
    xhr.open('POST', API_UPDATE);
    xhr.send(formData);
}



/**
 * Update brightness slider background gradient based on value
 * @param {number} brightness - Brightness value (0-100)
 */
function updateBrightnessSliderBackground(brightness) {
    const slider = document.getElementById('backlight_brightness');
    if (slider) {
        const percentage = brightness;
        slider.style.background = `linear-gradient(to right, #007aff 0%, #007aff ${percentage}%, #e5e5e5 ${percentage}%, #e5e5e5 100%)`;
    }
}

/**
 * Handle brightness slider changes - update device immediately
 * @param {Event} event - Input event from slider
 */
async function handleBrightnessChange(event) {
    const brightness = parseInt(event.target.value);
    
    // Update displayed value
    const valueDisplay = document.getElementById('brightness-value');
    if (valueDisplay) {
        valueDisplay.textContent = brightness;
    }
    
    // Update slider background
    updateBrightnessSliderBackground(brightness);
    
    // Send brightness update to device immediately (no persist)
    try {
        const response = await fetch('/api/display/brightness', {
            method: 'PUT',
            headers: {
                'Content-Type': 'application/json'
            },
            body: JSON.stringify({ brightness: brightness })
        });
        
        if (!response.ok) {
            console.error('Failed to update brightness:', response.statusText);
        }
    } catch (error) {
        console.error('Error updating brightness:', error);
    }
}

/**
 * Handle screen selection change - switch screens immediately
 * @param {Event} event - Change event from select dropdown
 */
async function handleScreenChange(event) {
    const screenId = event.target.value;
    
    if (!screenId) return;
    
    try {
        const response = await fetch('/api/display/screen', {
            method: 'PUT',
            headers: {
                'Content-Type': 'application/json'
            },
            body: JSON.stringify({ screen: screenId })
        });
        
        if (!response.ok) {
            console.error('Failed to switch screen:', response.statusText);
            showMessage('Failed to switch screen', 'error');
            // Revert dropdown to previous value
            loadVersion(); // Refresh to get current screen
        }
        // Success - dropdown already shows new value
    } catch (error) {
        console.error('Error switching screen:', error);
        showMessage('Error switching screen: ' + error.message, 'error');
        // Revert dropdown to previous value
        loadVersion(); // Refresh to get current screen
    }
}

/**
 * Initialize page on DOM ready
 */
document.addEventListener('DOMContentLoaded', () => {
    // Initialize navigation highlighting
    initNavigation();
    
    // Attach event handlers (check if elements exist for multi-page support)
    const configForm = document.getElementById('config-form');
    if (configForm) {
        configForm.addEventListener('submit', saveConfig);
    }
    
    const saveOnlyBtn = document.getElementById('save-only-btn');
    if (saveOnlyBtn) {
        saveOnlyBtn.addEventListener('click', saveOnly);
    }
    
    const rebootBtn = document.getElementById('reboot-btn');
    if (rebootBtn) {
        rebootBtn.addEventListener('click', rebootDevice);
    }
    
    const resetBtn = document.getElementById('reset-btn');
    if (resetBtn) {
        resetBtn.addEventListener('click', resetConfig);
    }
    
    const firmwareFile = document.getElementById('firmware-file');
    if (firmwareFile) {
        firmwareFile.addEventListener('change', handleFileSelect);
    }
    
    const uploadBtn = document.getElementById('upload-btn');
    if (uploadBtn) {
        uploadBtn.addEventListener('click', uploadFirmware);
    }

    // Firmware page: GitHub online update controls
    const githubUpdateBtn = document.getElementById('github-update-btn');
    if (githubUpdateBtn) {
        githubUpdateBtn.addEventListener('click', startGitHubFirmwareUpdate);
    }
    
    const deviceName = document.getElementById('device_name');
    if (deviceName) {
        deviceName.addEventListener('input', updateSanitizedName);
    }
    
    // Add focus handlers for all inputs to prevent keyboard from covering them
    const inputs = document.querySelectorAll('input[type="text"], input[type="password"], textarea');
    inputs.forEach(input => {
        input.addEventListener('focus', handleInputFocus);
    });
    
    // Add brightness slider event handler
    const brightnessSlider = document.getElementById('backlight_brightness');
    if (brightnessSlider) {
        brightnessSlider.addEventListener('input', handleBrightnessChange);
    }
    
    // Add screen selection dropdown event handler
    const screenSelect = document.getElementById('screen_selection');
    if (screenSelect) {
        screenSelect.addEventListener('change', handleScreenChange);
    }
    
    // Load initial data
    loadMode();
    
    // Only load config if config form exists (home and network pages)
    if (configForm) {
        loadConfig();
    } else {
        // Hide loading overlay on pages without config form (firmware page)
        const overlay = document.getElementById('form-loading-overlay');
        if (overlay) overlay.style.display = 'none';
    }
    
    loadVersion();

    if (currentPage === 'home') {
        initMacrosEditor();
    }
    
    // Initialize health widget
    initHealthWidget();
});

// ===== HEALTH WIDGET =====

const API_HEALTH = '/api/health';
let healthExpanded = false;
let healthUpdateInterval = null;

// Format uptime to readable string
function formatUptime(seconds) {
    const days = Math.floor(seconds / 86400);
    const hours = Math.floor((seconds % 86400) / 3600);
    const minutes = Math.floor((seconds % 3600) / 60);
    const secs = seconds % 60;
    
    if (days > 0) {
        return `${days}d ${hours}h ${minutes}m`;
    } else if (hours > 0) {
        return `${hours}h ${minutes}m ${secs}s`;
    } else if (minutes > 0) {
        return `${minutes}m ${secs}s`;
    } else {
        return `${secs}s`;
    }
}

// Format heap size
function formatHeap(bytes) {
    if (bytes >= 1024) {
        return `${(bytes / 1024).toFixed(1)} KB`;
    }
    return `${bytes} B`;
}

// Get signal strength description
function getSignalStrength(rssi) {
    if (rssi >= -50) return 'Excellent';
    if (rssi >= -60) return 'Good';
    if (rssi >= -70) return 'Fair';
    if (rssi >= -80) return 'Weak';
    return 'Very Weak';
}

// Update health stats
async function updateHealth() {
    try {
        const response = await fetch(API_HEALTH);
        if (!response.ok) return;
        
        const health = await response.json();
        
        // Update compact view
        document.getElementById('health-cpu').textContent = `CPU ${health.cpu_usage}%`;
        
        // Trigger breathing animation on status dots
        const dot = document.getElementById('health-status-dot');
        dot.classList.remove('breathing');
        void dot.offsetWidth; // Force reflow
        dot.classList.add('breathing');
        
        const dotExpanded = document.getElementById('health-status-dot-expanded');
        if (dotExpanded) {
            dotExpanded.classList.remove('breathing');
            void dotExpanded.offsetWidth; // Force reflow
            dotExpanded.classList.add('breathing');
        }
        
        // Update expanded view - System
        document.getElementById('health-uptime').textContent = formatUptime(health.uptime_seconds);
        document.getElementById('health-reset').textContent = health.reset_reason || 'Unknown';
        
        // CPU
        document.getElementById('health-cpu-full').textContent = `${health.cpu_usage}%`;
        document.getElementById('health-cpu-minmax').textContent = `${health.cpu_usage_min}% / ${health.cpu_usage_max}%`;
        document.getElementById('health-temp').textContent = health.cpu_temperature !== null ? 
            `${health.cpu_temperature}°C` : 'N/A';
        
        // Memory
        document.getElementById('health-heap').textContent = formatHeap(health.heap_free);
        document.getElementById('health-heap-min').textContent = formatHeap(health.heap_min);
        document.getElementById('health-heap-frag').textContent = `${health.heap_fragmentation}%`;
        
        // Flash
        const flashUsed = (health.flash_used / 1024).toFixed(0);
        const flashTotal = (health.flash_total / 1024).toFixed(0);
        document.getElementById('health-flash').textContent = 
            `${flashUsed} / ${flashTotal} KB`;
        
        // Network
        if (health.wifi_rssi !== null) {
            const rssi = health.wifi_rssi;
            const strength = getSignalStrength(rssi);
            document.getElementById('health-rssi').textContent = `${rssi} dBm (${strength})`;
            document.getElementById('health-ip').textContent = health.ip_address || 'N/A';
        } else {
            document.getElementById('health-rssi').textContent = 'Not connected';
            document.getElementById('health-ip').textContent = 'N/A';
        }
    } catch (error) {
        console.error('Failed to fetch health stats:', error);
    }
}

// Toggle widget expansion
function toggleHealthWidget() {
    healthExpanded = !healthExpanded;
    
    if (healthExpanded) {
        document.getElementById('health-expanded').style.display = 'block';
        updateHealth(); // Immediate update when opened
        // Update every 5 seconds when expanded
        healthUpdateInterval = setInterval(updateHealth, 5000);
    } else {
        document.getElementById('health-expanded').style.display = 'none';
        if (healthUpdateInterval) {
            clearInterval(healthUpdateInterval);
            healthUpdateInterval = null;
        }
    }
}

// Initialize health widget
function initHealthWidget() {
    // Click health badge in header to expand
    const healthBadge = document.getElementById('health-badge');
    if (healthBadge) {
        healthBadge.addEventListener('click', toggleHealthWidget);
    }
    
    // Click close button to collapse
    document.getElementById('health-close').addEventListener('click', toggleHealthWidget);
    
    // Initial health fetch
    updateHealth();
    
    // Update compact view every 10 seconds
    setInterval(() => {
        if (!healthExpanded) {
            updateHealth();
        }
    }, 10000);
}
