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
const API_ICONS_INSTALLED = '/api/icons/installed';
const API_ICON_INSTALL = '/api/icons/install';
const API_ICONS_GC = '/api/icons/gc';

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
const MACROS_PAYLOAD_MAX = 255;
const MACROS_MQTT_TOPIC_MAX = 127;
const MACROS_ICON_ID_MAX = 31;
// UTF-8 bytes (not code points). Firmware uses 64 bytes including NUL.
const MACROS_ICON_DISPLAY_MAX = 63;

// Keep in sync with src/app/macro_templates.h (macro_templates::default_id()).
const MACROS_DEFAULT_TEMPLATE_ID = 'round_ring_9';

// Default colors (RGB only: 0xRRGGBB). Must match firmware defaults.
const MACROS_DEFAULT_COLORS = {
    screen_bg: 0x000000,
    button_bg: 0x1E1E1E,
    icon_color: 0xFFFFFF,
    label_color: 0xFFFFFF,
};

let macrosPayloadCache = null; // { defaults:{...}, screens: [ { template, screen_bg?, buttons:[{label, action, payload, mqtt_topic, icon:{type,id,display}, button_bg?, icon_color?, label_color?}, ...] }, ... ] }
let macrosTemplatesCache = []; // [{id,name,selector_layout}, ...]
let macrosSelectedScreen = 0; // 0-based
let macrosSelectedButton = 0; // 0-based
let macrosDirty = false;
let macrosLoading = false;

function macrosClampRgb24(value) {
    const v = (typeof value === 'number' && isFinite(value)) ? value : 0;
    return (v >>> 0) & 0x00FFFFFF;
}

function macrosColorToHex(value) {
    const v = macrosClampRgb24(value);
    return '#' + v.toString(16).padStart(6, '0');
}

function macrosHexToColor(hex) {
    const s = (hex || '').toString().trim();
    const m = /^#?([0-9a-fA-F]{6})$/.exec(s);
    if (!m) return null;
    return parseInt(m[1], 16) & 0x00FFFFFF;
}

function initDuckyHelpDialog() {
    const openBtn = document.getElementById('ducky_help_open');
    const overlay = document.getElementById('ducky_help_overlay');
    if (!openBtn || !overlay) return; // Not on Home page

    const closeBtn = document.getElementById('ducky_help_close');

    const open = () => {
        overlay.style.display = 'flex';
    };

    const close = () => {
        overlay.style.display = 'none';
    };

    openBtn.addEventListener('click', open);
    if (closeBtn) closeBtn.addEventListener('click', close);

    // Click outside to close
    overlay.addEventListener('click', (e) => {
        if (e.target === overlay) close();
    });

    // Escape to close
    document.addEventListener('keydown', (e) => {
        if (e.key === 'Escape' && overlay.style.display !== 'none') close();
    });
}

function macrosGetSelectedScreenObj() {
    if (!macrosPayloadCache) return null;
    return macrosPayloadCache.screens[macrosSelectedScreen] || null;
}

// ===== ICONS (Home) =====
let iconListCache = null; // [{id, kind}, ...]
let macrosBuiltinIconIdsCache = []; // sorted list of builtin mask icon ids

function getBuiltinMaskIcons() {
    const icons = Array.isArray(iconListCache) ? iconListCache : [];
    return icons
        .filter(it => it && it.id && (it.kind === 'mask' || it.kind === 'mono' || it.kind === 'Mask'))
        .map(it => String(it.id));
}

function refreshBuiltinMaskIconCache() {
    macrosBuiltinIconIdsCache = getBuiltinMaskIcons().sort();
}

function macrosRenderBuiltinIconOptions({ filterText = '', preserveId = '' } = {}) {
    const select = document.getElementById('macro_icon_builtin_select');
    const countEl = document.getElementById('macro_icon_builtin_count');
    if (!select) return;

    const all = Array.isArray(macrosBuiltinIconIdsCache) ? macrosBuiltinIconIdsCache : [];
    const filter = (filterText || '').toString().trim().toLowerCase();
    const filtered = filter ? all.filter(id => id.toLowerCase().includes(filter)) : all;

    select.innerHTML = '';
    for (const id of filtered) {
        const opt = document.createElement('option');
        opt.value = id;
        opt.textContent = id;
        select.appendChild(opt);
    }

    if (countEl) {
        countEl.textContent = `${filtered.length}/${all.length}`;
    }

    // Preserve selection when possible; otherwise select first option.
    if (preserveId && filtered.includes(preserveId)) {
        select.value = preserveId;
    } else if (select.options.length > 0) {
        select.selectedIndex = 0;
    }
}

async function loadIcons() {
    // Home page only.
    const overlay = document.getElementById('macro_icon_overlay');
    if (!overlay) return;

    try {
        const compiledRes = await fetch(API_ICONS);
        if (!compiledRes.ok) throw new Error(`HTTP ${compiledRes.status}`);

        const compiled = await compiledRes.json().catch(() => ({}));

        const merged = [];
        for (const it of (Array.isArray(compiled.icons) ? compiled.icons : [])) {
            if (!it || !it.id) continue;
            merged.push({ id: String(it.id), kind: it.kind || 'mask' });
        }
        iconListCache = merged;
        refreshBuiltinMaskIconCache();
    } catch (error) {
        console.warn('Failed to load icons list:', error);
        iconListCache = [];
        refreshBuiltinMaskIconCache();
    }
}

// ===== TWEMOJI LAZY-INSTALL (Home) =====

function isLikelyEmojiLiteral(input) {
    const s = (input || '').toString().trim();
    if (!s) return false;

    // If it already looks like a normal icon id (e.g. "ic_home", "emoji_u1f525"),
    // don't treat it as a literal emoji.
    if (/^[a-zA-Z0-9_]+$/.test(s)) return false;

    // Heuristic: anything with non-ASCII, variation selectors, or ZWJ sequences.
    for (const ch of s) {
        const cp = ch.codePointAt(0);
        if (cp > 0x7f) return true;
    }
    return false;
}

function fnv1a32(str) {
    let h = 0x811c9dc5;
    for (let i = 0; i < str.length; i++) {
        h ^= str.charCodeAt(i);
        h = (h * 0x01000193) >>> 0;
    }
    return h >>> 0;
}

function emojiToCodepoints(emojiStr, { includeVS16 = true } = {}) {
    const cps = [];
    for (const ch of (emojiStr || '')) {
        const cp = ch.codePointAt(0);
        if (!includeVS16 && cp === 0xfe0f) continue;
        cps.push(cp.toString(16));
    }
    return cps;
}

function suggestIconIdForEmoji(emojiStr) {
    const cps = emojiToCodepoints(emojiStr, { includeVS16: false });
    if (cps.length === 1) return `emoji_u${cps[0]}`;
    const h = fnv1a32(emojiStr).toString(16).padStart(8, '0');
    return `emoji_${h}`;
}

async function fetchTwemojiPng(emojiStr) {
    const cps1 = emojiToCodepoints(emojiStr, { includeVS16: true }).join('-');
    const cps2 = emojiToCodepoints(emojiStr, { includeVS16: false }).join('-');

    const base = 'https://cdnjs.cloudflare.com/ajax/libs/twemoji/14.0.2/72x72/';
    const tryUrls = [];
    if (cps1) tryUrls.push(base + cps1 + '.png');
    if (cps2 && cps2 !== cps1) tryUrls.push(base + cps2 + '.png');

    let lastErr = null;
    for (const url of tryUrls) {
        try {
            const res = await fetch(url, { cache: 'force-cache' });
            if (!res.ok) throw new Error(`HTTP ${res.status}`);
            const blob = await res.blob();
            return { blob, url };
        } catch (e) {
            lastErr = e;
        }
    }
    throw (lastErr || new Error('Failed to fetch Twemoji'));
}

async function convertPngBlobToLvgl565aBlob(pngBlob, { width = 64, height = 64 } = {}) {
    const bitmap = await createImageBitmap(pngBlob);

    const canvas = document.createElement('canvas');
    canvas.width = width;
    canvas.height = height;

    const ctx = canvas.getContext('2d', { willReadFrequently: true });
    ctx.clearRect(0, 0, width, height);
    ctx.imageSmoothingEnabled = true;
    ctx.drawImage(bitmap, 0, 0, width, height);

    const img = ctx.getImageData(0, 0, width, height);
    const rgba = img.data;

    const headerSize = 16;
    const payloadSize = width * height * 3; // RGB565 (2) + A (1)
    const buf = new ArrayBuffer(headerSize + payloadSize);
    const dv = new DataView(buf);
    const out = new Uint8Array(buf);

    // Magic "ICN1"
    out[0] = 0x49; out[1] = 0x43; out[2] = 0x4e; out[3] = 0x31;
    dv.setUint16(4, width, true);
    dv.setUint16(6, height, true);
    dv.setUint8(8, 1); // format 1 = RGB565+Alpha
    dv.setUint8(9, 0);
    dv.setUint8(10, 0);
    dv.setUint8(11, 0);
    dv.setUint32(12, payloadSize, true);

    let o = headerSize;
    for (let i = 0; i < rgba.length; i += 4) {
        const r = rgba[i + 0];
        const g = rgba[i + 1];
        const b = rgba[i + 2];
        const a = rgba[i + 3];

        const rgb565 = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
        out[o + 0] = rgb565 & 0xff;        // little-endian
        out[o + 1] = (rgb565 >> 8) & 0xff;
        out[o + 2] = a;
        o += 3;
    }

    return buf;
}

async function installTwemojiToDevice(emojiStr, { onProgress } = {}) {
    const iconId = suggestIconIdForEmoji(emojiStr);

    if (typeof onProgress === 'function') onProgress(`Downloading ${emojiStr}…`);
    const { blob: pngBlob, url } = await fetchTwemojiPng(emojiStr);

    if (typeof onProgress === 'function') onProgress(`Preparing ${emojiStr}…`);
    const lvglBlob = await convertPngBlobToLvgl565aBlob(pngBlob, { width: 64, height: 64 });

    if (typeof onProgress === 'function') onProgress(`Uploading ${emojiStr}…`);
    const res = await fetch(`${API_ICON_INSTALL}?id=${encodeURIComponent(iconId)}`, {
        method: 'POST',
        headers: { 'Content-Type': 'application/octet-stream' },
        body: lvglBlob,
    });

    const data = await res.json().catch(() => ({}));
    if (!res.ok || data.success === false) {
        throw new Error(data.message || `Install failed (${res.status})`);
    }

    return { iconId, sourceUrl: url };
}

async function macrosAutoInstallEmojiIcons(payload, { silent = false, onProgress } = {}) {
    if (!payload || !Array.isArray(payload.screens)) return { ok: true, installed: 0 };

    // Fetch installed icon IDs once (so we don't re-download/upload on every save).
    const installedIds = new Set();
    try {
        if (typeof onProgress === 'function') onProgress('Checking installed emoji icons…');
        const res = await fetch(API_ICONS_INSTALLED);
        if (res.ok) {
            const data = await res.json().catch(() => ({}));
            for (const it of (Array.isArray(data.icons) ? data.icons : [])) {
                if (it && it.id) installedIds.add(String(it.id));
            }
        }
    } catch (_) {
        // Best-effort; fall back to always attempting installs.
    }

    const emojiSet = new Set();
    for (const screen of payload.screens) {
        if (!screen || !Array.isArray(screen.buttons)) continue;
        for (const btn of screen.buttons) {
            if (!btn) continue;
            const icon = btn.icon;
            if (!icon || icon.type !== 'emoji') continue;

            const display = (icon.display || '').toString().trim();
            if (!display) continue;
            // Only install if it looks like a literal emoji.
            if (!isLikelyEmojiLiteral(display)) continue;

            emojiSet.add(display);
        }
    }

    const emojiList = Array.from(emojiSet);
    if (emojiList.length === 0) return { ok: true, installed: 0 };

    const toInstall = emojiList.filter(e => !installedIds.has(suggestIconIdForEmoji(e)));

    if (toInstall.length > 0) {
        if (!silent) showMessage(`Installing ${toInstall.length} emoji icon(s)…`, 'info');
        if (typeof onProgress === 'function') onProgress(`Installing ${toInstall.length} emoji icon(s)…`);
    }

    const mapping = new Map(); // emojiStr -> iconId
    let installIndex = 0;
    for (const emojiStr of emojiList) {
        const expectedId = suggestIconIdForEmoji(emojiStr);
        if (installedIds.has(expectedId)) {
            mapping.set(emojiStr, expectedId);
            continue;
        }

        installIndex += 1;
        const idx = installIndex;
        const total = toInstall.length;
        try {
            if (typeof onProgress === 'function' && total > 0) onProgress(`(${idx}/${total}) ${emojiStr}`);
            const { iconId } = await installTwemojiToDevice(emojiStr, { onProgress });
            mapping.set(emojiStr, iconId);
            installedIds.add(iconId);
        } catch (e) {
            const msg = (e && e.message) ? e.message : 'Install failed';
            return { ok: false, message: `Failed to install ${emojiStr}: ${msg}` };
        }
    }

    // Replace literals in the payload with stable icon IDs.
    for (const screen of payload.screens) {
        if (!screen || !Array.isArray(screen.buttons)) continue;
        for (const btn of screen.buttons) {
            if (!btn) continue;
            const icon = btn.icon;
            if (!icon || icon.type !== 'emoji') continue;

            const display = (icon.display || '').toString().trim();
            if (!display) continue;
            if (!isLikelyEmojiLiteral(display)) continue;

            const iconId = mapping.get(display);
            if (iconId) icon.id = iconId;
        }
    }

    // Refresh icon list so autocomplete includes newly installed icons.
    if (toInstall.length > 0) {
        await loadIcons();
    }

    return { ok: true, installed: toInstall.length };
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
    return { label: '', action: 'none', payload: '', mqtt_topic: '', icon: { type: 'none', id: '', display: '' } };
}

function macrosCreateEmptyPayload() {
    const payload = { defaults: { ...MACROS_DEFAULT_COLORS }, screens: [] };
    for (let s = 0; s < macrosScreenCount; s++) {
        const buttons = [];
        for (let b = 0; b < macrosButtonsPerScreen; b++) {
            buttons.push(macrosCreateEmptyButton());
        }
        payload.screens.push({ template: MACROS_DEFAULT_TEMPLATE_ID, buttons });
    }
    return payload;
}

function macrosIsValidScreenId(id) {
    const key = (id || '').toString();
    if (!key) return false;
    return macrosGetAvailableScreens().some(s => s.id === key);
}

function macrosNormalizePayload(payload) {
    const out = macrosCreateEmptyPayload();
    if (!payload || !Array.isArray(payload.screens)) return out;

    function normalizeIcon(input) {
        // New schema: {type,id,display}
        if (input && typeof input === 'object') {
            const t = (input.type || 'none').toString();
            const type = (t === 'builtin' || t === 'emoji' || t === 'asset') ? t : 'none';
            return {
                type,
                id: (input.id || '').toString(),
                display: (input.display || '').toString(),
            };
        }

        // Legacy fallback (should not happen anymore, but keeps UI resilient).
        const legacy = (input || '').toString();
        if (!legacy) return { type: 'none', id: '', display: '' };
        return { type: 'builtin', id: legacy, display: '' };
    }

    // Defaults
    if (payload.defaults && typeof payload.defaults === 'object') {
        const d = payload.defaults;
        out.defaults = {
            screen_bg: macrosClampRgb24(typeof d.screen_bg === 'number' ? d.screen_bg : MACROS_DEFAULT_COLORS.screen_bg),
            button_bg: macrosClampRgb24(typeof d.button_bg === 'number' ? d.button_bg : MACROS_DEFAULT_COLORS.button_bg),
            icon_color: macrosClampRgb24(typeof d.icon_color === 'number' ? d.icon_color : MACROS_DEFAULT_COLORS.icon_color),
            label_color: macrosClampRgb24(typeof d.label_color === 'number' ? d.label_color : MACROS_DEFAULT_COLORS.label_color),
        };
    }

    if (Array.isArray(payload.templates)) {
        macrosTemplatesCache = payload.templates;
    }

    for (let s = 0; s < Math.min(macrosScreenCount, payload.screens.length); s++) {
        const screen = payload.screens[s];
        if (!screen || !Array.isArray(screen.buttons)) continue;

        if (screen.template) {
            out.screens[s].template = String(screen.template);
        }

        // Optional per-screen override
        if (typeof screen.screen_bg === 'number' && isFinite(screen.screen_bg)) {
            out.screens[s].screen_bg = macrosClampRgb24(screen.screen_bg);
        }

        for (let b = 0; b < Math.min(macrosButtonsPerScreen, screen.buttons.length); b++) {
            const btn = screen.buttons[b] || {};
            const next = {
                label: (btn.label || ''),
                action: (btn.action || 'none'),
                payload: (btn.payload || ''),
                mqtt_topic: (btn.mqtt_topic || ''),
                icon: normalizeIcon(btn.icon || btn.icon_id),
            };

            // Optional per-button appearance overrides
            if (typeof btn.button_bg === 'number' && isFinite(btn.button_bg)) next.button_bg = macrosClampRgb24(btn.button_bg);
            if (typeof btn.icon_color === 'number' && isFinite(btn.icon_color)) next.icon_color = macrosClampRgb24(btn.icon_color);
            if (typeof btn.label_color === 'number' && isFinite(btn.label_color)) next.label_color = macrosClampRgb24(btn.label_color);

            out.screens[s].buttons[b] = next;
        }
    }

    return out;
}

function macrosActionIsMqttSend(action) {
    return action === 'mqtt_send';
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

function macrosClampUtf8Bytes(value, maxBytes) {
    const s = (value || '').toString();
    const enc = new TextEncoder();
    if (enc.encode(s).length <= maxBytes) return s;

    const cps = Array.from(s);
    while (cps.length > 0) {
        cps.pop();
        const t = cps.join('');
        if (enc.encode(t).length <= maxBytes) return t;
    }
    return '';
}

function macrosFirstGrapheme(input) {
    const s = (input || '').toString().trim();
    if (!s) return '';
    try {
        if (typeof Intl !== 'undefined' && Intl.Segmenter) {
            const seg = new Intl.Segmenter(undefined, { granularity: 'grapheme' });
            for (const part of seg.segment(s)) {
                return part.segment;
            }
        }
    } catch (_) {
        // Ignore and fall back.
    }
    const cps = Array.from(s);
    return cps.length ? cps[0] : '';
}

function macrosEnsureIconObject(btn) {
    if (!btn) return null;
    if (!btn.icon || typeof btn.icon !== 'object') {
        btn.icon = { type: 'none', id: '', display: '' };
    }
    if (!btn.icon.type) btn.icon.type = 'none';
    if (btn.icon.id === undefined) btn.icon.id = '';
    if (btn.icon.display === undefined) btn.icon.display = '';
    return btn.icon;
}

function macrosClearIcon(btn) {
    const icon = macrosEnsureIconObject(btn);
    if (!icon) return;
    icon.type = 'none';
    icon.id = '';
    icon.display = '';
}

function macrosIconSummaryText(icon) {
    if (!icon || icon.type === 'none') return '—';
    if (icon.type === 'emoji') {
        const d = (icon.display || '').toString().trim();
        return d || '—';
    }

    // For builtin/asset: show id (stable lookup key).
    const id = (icon.id || '').toString().trim();
    if (!id) return '—';
    return id;
}

function macrosRenderIconSummary() {
    const cfg = macrosGetSelectedButton();
    const summaryEl = document.getElementById('macro_icon_summary');
    const editBtn = document.getElementById('macro_icon_edit_btn');
    const clearBtn = document.getElementById('macro_icon_clear_btn');
    if (!summaryEl || !editBtn || !clearBtn) return;

    if (!cfg) {
        summaryEl.textContent = '—';
        editBtn.disabled = true;
        clearBtn.disabled = true;
        return;
    }

    const icon = macrosEnsureIconObject(cfg);
    summaryEl.textContent = macrosIconSummaryText(icon);

    const action = (cfg.action || 'none');
    const enabled = action !== 'none';
    editBtn.disabled = !enabled;
    clearBtn.disabled = !enabled || (icon && icon.type === 'none');
}

function macrosActionUsesTextPayload(action) {
    return action === 'send_keys' || action === 'mqtt_send';
}

function macrosActionUsesScreenPayload(action) {
    return action === 'nav_to';
}

function macrosActionSupportsPayload(action) {
    return macrosActionUsesTextPayload(action) || macrosActionUsesScreenPayload(action);
}

function macrosGetAvailableScreens() {
    const screens = deviceInfoCache && Array.isArray(deviceInfoCache.available_screens)
        ? deviceInfoCache.available_screens
        : [];
    return screens.filter(s => s && typeof s.id === 'string' && s.id.length > 0);
}

function macrosScreenNameForId(id) {
    const key = (id || '').toString();
    if (!key) return '';
    for (const s of macrosGetAvailableScreens()) {
        if (s.id === key) return (s.name || s.id).toString();
    }
    return '';
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

function macrosUpdatePayloadCharCounter() {
    const charsEl = document.getElementById('macro_payload_chars');
    const payloadEl = document.getElementById('macro_payload');
    if (!charsEl || !payloadEl) return;
    charsEl.textContent = String((payloadEl.value || '').length);
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
            subtitle.textContent = 'Send Keys';
        } else if (action === 'nav_prev') {
            subtitle.textContent = 'Prev Macro';
        } else if (action === 'nav_next') {
            subtitle.textContent = 'Next Macro';
        } else if (action === 'nav_to') {
            const target = (cfg && cfg.payload) ? String(cfg.payload) : '';
            const name = macrosScreenNameForId(target);
            subtitle.textContent = name ? `Go → ${name}` : (target ? `Go → ${target}` : 'Go → (select)');
        } else if (action === 'mqtt_send') {
            const topic = (cfg && cfg.mqtt_topic) ? String(cfg.mqtt_topic) : '';
            subtitle.textContent = topic ? `MQTT → ${topic}` : 'MQTT → (set topic)';
        } else if (action === 'go_back') {
            subtitle.textContent = 'Back';
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
    const mqttTopicEl = document.getElementById('macro_mqtt_topic');
    const mqttTopicGroupEl = document.getElementById('macro_mqtt_topic_group');
    const payloadEl = document.getElementById('macro_payload');
    const payloadGroupEl = document.getElementById('macro_payload_group');
    const navGroupEl = document.getElementById('macro_nav_to_group');
    const navSelectEl = document.getElementById('macro_nav_target');
    const payloadLabelEl = document.getElementById('macro_payload_label');
    const payloadHelpEl = document.getElementById('macro_payload_help');
    const duckyHelpBtn = document.getElementById('ducky_help_open');

    if (!cfg) {
        if (labelEl) labelEl.value = '';
        if (actionEl) actionEl.value = 'none';
        if (mqttTopicEl) mqttTopicEl.value = '';
        if (payloadEl) payloadEl.value = '';
        if (payloadGroupEl) payloadGroupEl.style.display = 'none';
        if (mqttTopicGroupEl) mqttTopicGroupEl.style.display = 'none';
        if (navGroupEl) navGroupEl.style.display = 'none';
        macrosUpdatePayloadCharCounter();
        macrosRenderIconSummary();
        return;
    }

    if (labelEl) labelEl.value = cfg.label || '';
    if (actionEl) actionEl.value = cfg.action || 'none';

    const action = (cfg.action || 'none');

    if (mqttTopicEl) mqttTopicEl.value = cfg.mqtt_topic || '';
    if (payloadEl) payloadEl.value = cfg.payload || '';
    const textEnabled = macrosActionUsesTextPayload(action);
    const navEnabled = macrosActionUsesScreenPayload(action);

    // MQTT topic textbox: only show it for mqtt_send.
    if (mqttTopicGroupEl) mqttTopicGroupEl.style.display = (action === 'mqtt_send') ? 'block' : 'none';
    if (mqttTopicEl) {
        mqttTopicEl.disabled = false;
        if (action !== 'mqtt_send') mqttTopicEl.value = '';
    }

    // Payload textarea: only show it for Send Keys.
    if (payloadGroupEl) payloadGroupEl.style.display = textEnabled ? 'block' : 'none';
    if (payloadEl) {
        payloadEl.disabled = false;
        if (!textEnabled) payloadEl.value = '';

        if (action === 'send_keys') {
            payloadEl.maxLength = MACROS_PAYLOAD_MAX;
            payloadEl.placeholder = 'Example:\nSTRING Hello world\nENTER\n';
            if (payloadLabelEl) payloadLabelEl.textContent = 'Send Keys (Script)';
            if (payloadHelpEl) {
                payloadHelpEl.innerHTML = `Used when Action is <strong>Send Keys (Script)</strong> (DuckyScript-like subset). <span style="margin-left: 8px;">Chars: <span id="macro_payload_chars">0</span>/${MACROS_PAYLOAD_MAX}</span>`;
            }
            if (duckyHelpBtn) duckyHelpBtn.style.display = 'inline';
        } else if (action === 'mqtt_send') {
            payloadEl.maxLength = MACROS_PAYLOAD_MAX;
            payloadEl.placeholder = 'Payload (optional)\nExample:\n{"button":"mute"}\n';
            if (payloadLabelEl) payloadLabelEl.textContent = 'MQTT payload';
            if (payloadHelpEl) {
                payloadHelpEl.innerHTML = `Optional payload text. <span style="margin-left: 8px;">Chars: <span id="macro_payload_chars">0</span>/${MACROS_PAYLOAD_MAX}</span>`;
            }
            if (duckyHelpBtn) duckyHelpBtn.style.display = 'none';
        } else {
            // Other payload actions handled elsewhere.
            if (duckyHelpBtn) duckyHelpBtn.style.display = 'none';
        }
    }

    // Screen dropdown: only show it for Go to Screen.
    if (navGroupEl) navGroupEl.style.display = navEnabled ? 'block' : 'none';
    if (navSelectEl) {
        if (!navEnabled) {
            navSelectEl.innerHTML = '';
        } else {
            const screens = macrosGetAvailableScreens();
            navSelectEl.innerHTML = '';

            if (screens.length === 0) {
                const opt = document.createElement('option');
                opt.value = '';
                opt.textContent = '(loading screens…)';
                navSelectEl.appendChild(opt);
                navSelectEl.disabled = true;
            } else {
                navSelectEl.disabled = false;
                for (const s of screens) {
                    const opt = document.createElement('option');
                    opt.value = s.id;
                    opt.textContent = (s.name || s.id).toString();
                    navSelectEl.appendChild(opt);
                }

                const current = (cfg.payload || '').toString();
                const exists = screens.some(s => s.id === current);
                if (exists) {
                    navSelectEl.value = current;
                } else {
                    // Prefer macro1 if available; otherwise pick first.
                    const prefer = screens.some(s => s.id === 'macro1') ? 'macro1' : screens[0].id;
                    navSelectEl.value = prefer;
                    cfg.payload = prefer;
                    macrosSetDirty(true);
                }
            }
        }
    }

    macrosUpdatePayloadCharCounter();

    if (action === 'none') {
        macrosClearIcon(cfg);
    }
    macrosRenderIconSummary();
}

function macrosRenderColorFields() {
    const payload = macrosPayloadCache;
    if (!payload) return;

    // Defaults
    const d = payload.defaults || MACROS_DEFAULT_COLORS;
    const defScreenEl = document.getElementById('macro_default_screen_bg');
    const defBtnEl = document.getElementById('macro_default_button_bg');
    const defIconEl = document.getElementById('macro_default_icon_color');
    const defLabelEl = document.getElementById('macro_default_label_color');
    if (defScreenEl) defScreenEl.value = macrosColorToHex(d.screen_bg);
    if (defBtnEl) defBtnEl.value = macrosColorToHex(d.button_bg);
    if (defIconEl) defIconEl.value = macrosColorToHex(d.icon_color);
    if (defLabelEl) defLabelEl.value = macrosColorToHex(d.label_color);

    // Per-screen override
    const screen = macrosGetSelectedScreenObj();
    const screenEnabledEl = document.getElementById('macro_screen_bg_enabled');
    const screenColorEl = document.getElementById('macro_screen_bg');
    const screenHasOverride = !!(screen && typeof screen.screen_bg === 'number');
    if (screenEnabledEl) screenEnabledEl.checked = screenHasOverride;
    if (screenColorEl) {
        screenColorEl.disabled = !screenHasOverride;
        screenColorEl.value = macrosColorToHex(screenHasOverride ? screen.screen_bg : d.screen_bg);
    }

    // Per-button overrides
    const btn = macrosGetSelectedButton();
    const btnBgEnabledEl = document.getElementById('macro_button_bg_enabled');
    const btnBgEl = document.getElementById('macro_button_bg');
    const iconEnabledEl = document.getElementById('macro_icon_color_enabled');
    const iconEl = document.getElementById('macro_icon_color');
    const labelEnabledEl = document.getElementById('macro_label_color_enabled');
    const labelEl = document.getElementById('macro_label_color');

    const hasBtnBg = !!(btn && typeof btn.button_bg === 'number');
    const hasIconColor = !!(btn && typeof btn.icon_color === 'number');
    const hasLabelColor = !!(btn && typeof btn.label_color === 'number');

    if (btnBgEnabledEl) btnBgEnabledEl.checked = hasBtnBg;
    if (btnBgEl) {
        btnBgEl.disabled = !hasBtnBg;
        btnBgEl.value = macrosColorToHex(hasBtnBg ? btn.button_bg : d.button_bg);
    }

    if (iconEnabledEl) iconEnabledEl.checked = hasIconColor;
    if (iconEl) {
        iconEl.disabled = !hasIconColor;
        iconEl.value = macrosColorToHex(hasIconColor ? btn.icon_color : d.icon_color);
    }

    if (labelEnabledEl) labelEnabledEl.checked = hasLabelColor;
    if (labelEl) {
        labelEl.disabled = !hasLabelColor;
        labelEl.value = macrosColorToHex(hasLabelColor ? btn.label_color : d.label_color);
    }
}

function macrosRenderAll() {
    macrosApplyTemplateLayout();
    macrosRenderScreenSelect();
    macrosRenderTemplateSelect();
    macrosRenderButtonGrid();
    macrosRenderEditorFields();
    macrosRenderColorFields();
}

function macrosOpenIconDialog() {
    const overlay = document.getElementById('macro_icon_overlay');
    if (!overlay) return;

    const cfg = macrosGetSelectedButton();
    if (!cfg) return;
    const action = (cfg.action || 'none');
    if (action === 'none') return;

    const icon = macrosEnsureIconObject(cfg);
    const tabBuiltin = document.getElementById('macro_icon_tab_builtin');
    const tabEmoji = document.getElementById('macro_icon_tab_emoji');
    const panelBuiltin = document.getElementById('macro_icon_panel_builtin');
    const panelEmoji = document.getElementById('macro_icon_panel_emoji');
    const select = document.getElementById('macro_icon_builtin_select');
    const searchEl = document.getElementById('macro_icon_builtin_search');
    const emojiInput = document.getElementById('macro_icon_emoji_input');

    refreshBuiltinMaskIconCache();
    if (searchEl) searchEl.value = '';

    function setTab(which) {
        const isBuiltin = which === 'builtin';
        if (panelBuiltin) panelBuiltin.style.display = isBuiltin ? 'block' : 'none';
        if (panelEmoji) panelEmoji.style.display = isBuiltin ? 'none' : 'block';
        if (tabBuiltin) tabBuiltin.setAttribute('aria-selected', isBuiltin ? 'true' : 'false');
        if (tabEmoji) tabEmoji.setAttribute('aria-selected', !isBuiltin ? 'true' : 'false');
    }

    if (icon && icon.type === 'emoji') {
        setTab('emoji');
    } else {
        setTab('builtin');
    }

    if (select) {
        const preserveId = (icon && icon.type === 'builtin' && icon.id) ? String(icon.id) : '';
        macrosRenderBuiltinIconOptions({ filterText: searchEl ? searchEl.value : '', preserveId });
    }

    if (emojiInput) {
        const v = (icon && icon.type === 'emoji' && icon.display) ? String(icon.display) : '';
        emojiInput.value = macrosFirstGrapheme(v);
    }

    // Focus the most relevant control.
    if (icon && icon.type === 'emoji') {
        if (emojiInput) emojiInput.focus();
    } else {
        if (searchEl) searchEl.focus();
        else if (select) select.focus();
    }

    overlay.style.display = 'flex';
}

function macrosCloseIconDialog() {
    const overlay = document.getElementById('macro_icon_overlay');
    if (overlay) overlay.style.display = 'none';
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

            if (cfg.action === 'mqtt_send') {
                cfg.mqtt_topic = '';
                cfg.payload = '';
            }

            // Keep stored data tidy.
            if (!macrosActionSupportsPayload(cfg.action)) cfg.payload = '';
            if (cfg.action !== 'mqtt_send') cfg.mqtt_topic = '';
            if (cfg.action === 'none') macrosClearIcon(cfg);

            macrosSetDirty(true);
            macrosRenderAll();
        });
    }

    const mqttTopicEl = document.getElementById('macro_mqtt_topic');
    if (mqttTopicEl) {
        mqttTopicEl.addEventListener('input', () => {
            const cfg = macrosGetSelectedButton();
            if (!cfg) return;
            cfg.mqtt_topic = macrosClampString(mqttTopicEl.value, MACROS_MQTT_TOPIC_MAX);
            if (mqttTopicEl.value !== cfg.mqtt_topic) mqttTopicEl.value = cfg.mqtt_topic;
            macrosSetDirty(true);
            macrosRenderButtonGrid();
        });
    }

    const payloadEl = document.getElementById('macro_payload');
    if (payloadEl) {
        // Guard against odd captive-browser/mobile behavior that sometimes treats Enter as form-submit.
        // We still want newlines in the textarea, so do NOT preventDefault.
        payloadEl.addEventListener('keydown', (e) => {
            if (e.key === 'Enter') {
                e.stopPropagation();
            }
        });

        payloadEl.addEventListener('input', () => {
            const cfg = macrosGetSelectedButton();
            if (!cfg) return;

            cfg.payload = macrosClampString(payloadEl.value, MACROS_PAYLOAD_MAX);
            if (payloadEl.value !== cfg.payload) payloadEl.value = cfg.payload;
            macrosSetDirty(true);
            macrosUpdatePayloadCharCounter();
        });
    }

    const navSelectEl = document.getElementById('macro_nav_target');
    if (navSelectEl) {
        navSelectEl.addEventListener('change', () => {
            const cfg = macrosGetSelectedButton();
            if (!cfg) return;
            cfg.payload = (navSelectEl.value || '').toString();
            macrosSetDirty(true);
            macrosRenderButtonGrid();
        });
    }

    // ===== Icon dialog (Mono / Emoji) =====
    const iconEditBtn = document.getElementById('macro_icon_edit_btn');
    const iconClearBtn = document.getElementById('macro_icon_clear_btn');
    const iconCancelBtn = document.getElementById('macro_icon_cancel_btn');
    const iconApplyBtn = document.getElementById('macro_icon_apply_btn');
    const iconDialogClearBtn = document.getElementById('macro_icon_dialog_clear_btn');
    const tabBuiltin = document.getElementById('macro_icon_tab_builtin');
    const tabEmoji = document.getElementById('macro_icon_tab_emoji');
    const panelBuiltin = document.getElementById('macro_icon_panel_builtin');
    const panelEmoji = document.getElementById('macro_icon_panel_emoji');
    const builtinSelect = document.getElementById('macro_icon_builtin_select');
    const builtinSearch = document.getElementById('macro_icon_builtin_search');
    const emojiInput = document.getElementById('macro_icon_emoji_input');
    const overlay = document.getElementById('macro_icon_overlay');

    function setTab(which) {
        const isBuiltin = which === 'builtin';
        if (panelBuiltin) panelBuiltin.style.display = isBuiltin ? 'block' : 'none';
        if (panelEmoji) panelEmoji.style.display = isBuiltin ? 'none' : 'block';
        if (tabBuiltin) tabBuiltin.setAttribute('aria-selected', isBuiltin ? 'true' : 'false');
        if (tabEmoji) tabEmoji.setAttribute('aria-selected', !isBuiltin ? 'true' : 'false');
    }

    if (tabBuiltin) tabBuiltin.addEventListener('click', () => setTab('builtin'));
    if (tabEmoji) tabEmoji.addEventListener('click', () => setTab('emoji'));

    function applyBuiltinFilter({ preserveId = '' } = {}) {
        refreshBuiltinMaskIconCache();
        macrosRenderBuiltinIconOptions({
            filterText: builtinSearch ? builtinSearch.value : '',
            preserveId: preserveId || (builtinSelect ? builtinSelect.value : ''),
        });
    }

    if (builtinSearch) {
        builtinSearch.addEventListener('input', () => {
            applyBuiltinFilter({ preserveId: '' });
        });

        builtinSearch.addEventListener('keydown', (e) => {
            if (e.key === 'ArrowDown') {
                e.preventDefault();
                if (builtinSelect) {
                    builtinSelect.focus();
                    if (builtinSelect.options.length > 0 && builtinSelect.selectedIndex < 0) builtinSelect.selectedIndex = 0;
                }
            } else if (e.key === 'Escape') {
                e.preventDefault();
                macrosCloseIconDialog();
            }
        });
    }

    if (builtinSelect) {
        builtinSelect.addEventListener('keydown', (e) => {
            if (e.key === 'Enter') {
                e.preventDefault();
                if (iconApplyBtn) iconApplyBtn.click();
            } else if (e.key === 'Escape') {
                e.preventDefault();
                macrosCloseIconDialog();
            }
        });
        builtinSelect.addEventListener('dblclick', () => {
            if (iconApplyBtn) iconApplyBtn.click();
        });
    }

    if (overlay) {
        overlay.addEventListener('keydown', (e) => {
            if (e.key === 'Escape') {
                e.preventDefault();
                macrosCloseIconDialog();
            }
        });
    }

    // Emoji input: force single emoji/grapheme.
    if (emojiInput) {
        const normalize = () => {
            const one = macrosFirstGrapheme(emojiInput.value);
            if (emojiInput.value !== one) emojiInput.value = one;
        };
        emojiInput.addEventListener('input', normalize);
        emojiInput.addEventListener('change', normalize);
    }

    if (iconEditBtn) {
        iconEditBtn.addEventListener('click', () => {
            macrosOpenIconDialog();
        });
    }

    if (iconClearBtn) {
        iconClearBtn.addEventListener('click', () => {
            const cfg = macrosGetSelectedButton();
            if (!cfg) return;
            macrosClearIcon(cfg);
            macrosSetDirty(true);
            macrosRenderIconSummary();
            macrosRenderButtonGrid();
        });
    }

    const closeDialog = () => macrosCloseIconDialog();
    if (iconCancelBtn) iconCancelBtn.addEventListener('click', closeDialog);

    if (iconDialogClearBtn) {
        iconDialogClearBtn.addEventListener('click', () => {
            if (builtinSelect) builtinSelect.value = '';
            if (builtinSearch) {
                builtinSearch.value = '';
                applyBuiltinFilter({ preserveId: '' });
            }
            if (emojiInput) emojiInput.value = '';
        });
    }

    if (iconApplyBtn) {
        iconApplyBtn.addEventListener('click', () => {
            const cfg = macrosGetSelectedButton();
            if (!cfg) return;
            const action = (cfg.action || 'none');
            if (action === 'none') return;

            const icon = macrosEnsureIconObject(cfg);

            const builtinVisible = panelBuiltin && panelBuiltin.style.display !== 'none';
            if (builtinVisible) {
                const id = builtinSelect ? (builtinSelect.value || '').toString().trim() : '';
                if (!id) {
                    macrosClearIcon(cfg);
                } else {
                    icon.type = 'builtin';
                    icon.id = macrosClampString(id, MACROS_ICON_ID_MAX);
                    icon.display = '';
                }
            } else {
                const raw = emojiInput ? macrosFirstGrapheme(emojiInput.value) : '';
                if (!raw) {
                    macrosClearIcon(cfg);
                } else {
                    icon.type = 'emoji';
                    icon.display = macrosClampUtf8Bytes(raw, MACROS_ICON_DISPLAY_MAX);
                    // Always overwrite any previous builtin id; use stable emoji id.
                    if (isLikelyEmojiLiteral(icon.display)) {
                        icon.id = suggestIconIdForEmoji(icon.display);
                    } else {
                        icon.id = '';
                    }
                }
            }

            macrosSetDirty(true);
            macrosRenderIconSummary();
            macrosRenderButtonGrid();
            macrosCloseIconDialog();
        });
    }

    // ===== Color defaults (global) =====
    const defScreenEl = document.getElementById('macro_default_screen_bg');
    const defBtnEl = document.getElementById('macro_default_button_bg');
    const defIconEl = document.getElementById('macro_default_icon_color');
    const defLabelEl = document.getElementById('macro_default_label_color');

    function ensureDefaults() {
        if (!macrosPayloadCache) return null;
        if (!macrosPayloadCache.defaults) macrosPayloadCache.defaults = { ...MACROS_DEFAULT_COLORS };
        return macrosPayloadCache.defaults;
    }

    function bindDefaultColor(inputEl, key) {
        if (!inputEl) return;
        inputEl.addEventListener('input', () => {
            const d = ensureDefaults();
            if (!d) return;
            const v = macrosHexToColor(inputEl.value);
            if (v === null) return;
            d[key] = macrosClampRgb24(v);
            macrosSetDirty(true);
        });
    }

    bindDefaultColor(defScreenEl, 'screen_bg');
    bindDefaultColor(defBtnEl, 'button_bg');
    bindDefaultColor(defIconEl, 'icon_color');
    bindDefaultColor(defLabelEl, 'label_color');

    // ===== Per-screen override =====
    const screenEnabledEl = document.getElementById('macro_screen_bg_enabled');
    const screenColorEl = document.getElementById('macro_screen_bg');
    function applyScreenBgFromUI() {
        const screen = macrosGetSelectedScreenObj();
        if (!screen || !screenEnabledEl || !screenColorEl) return;
        if (!screenEnabledEl.checked) {
            delete screen.screen_bg;
        } else {
            const v = macrosHexToColor(screenColorEl.value);
            if (v !== null) screen.screen_bg = macrosClampRgb24(v);
        }
        macrosSetDirty(true);
        macrosRenderColorFields();
    }
    if (screenEnabledEl) screenEnabledEl.addEventListener('change', applyScreenBgFromUI);
    if (screenColorEl) screenColorEl.addEventListener('input', () => {
        if (screenEnabledEl && screenEnabledEl.checked) applyScreenBgFromUI();
    });

    // ===== Per-button overrides =====
    const btnBgEnabledEl = document.getElementById('macro_button_bg_enabled');
    const btnBgEl = document.getElementById('macro_button_bg');
    const iconColorEnabledEl = document.getElementById('macro_icon_color_enabled');
    const iconColorEl = document.getElementById('macro_icon_color');
    const labelColorEnabledEl = document.getElementById('macro_label_color_enabled');
    const labelColorEl = document.getElementById('macro_label_color');

    function applyButtonColorsFromUI() {
        const btn = macrosGetSelectedButton();
        if (!btn) return;

        if (btnBgEnabledEl && btnBgEl) {
            if (!btnBgEnabledEl.checked) delete btn.button_bg;
            else {
                const v = macrosHexToColor(btnBgEl.value);
                if (v !== null) btn.button_bg = macrosClampRgb24(v);
            }
        }

        if (iconColorEnabledEl && iconColorEl) {
            if (!iconColorEnabledEl.checked) delete btn.icon_color;
            else {
                const v = macrosHexToColor(iconColorEl.value);
                if (v !== null) btn.icon_color = macrosClampRgb24(v);
            }
        }

        if (labelColorEnabledEl && labelColorEl) {
            if (!labelColorEnabledEl.checked) delete btn.label_color;
            else {
                const v = macrosHexToColor(labelColorEl.value);
                if (v !== null) btn.label_color = macrosClampRgb24(v);
            }
        }

        macrosSetDirty(true);
        macrosRenderColorFields();
    }

    if (btnBgEnabledEl) btnBgEnabledEl.addEventListener('change', applyButtonColorsFromUI);
    if (btnBgEl) btnBgEl.addEventListener('input', () => {
        if (btnBgEnabledEl && btnBgEnabledEl.checked) applyButtonColorsFromUI();
    });

    if (iconColorEnabledEl) iconColorEnabledEl.addEventListener('change', applyButtonColorsFromUI);
    if (iconColorEl) iconColorEl.addEventListener('input', () => {
        if (iconColorEnabledEl && iconColorEnabledEl.checked) applyButtonColorsFromUI();
    });

    if (labelColorEnabledEl) labelColorEnabledEl.addEventListener('change', applyButtonColorsFromUI);
    if (labelColorEl) labelColorEl.addEventListener('input', () => {
        if (labelColorEnabledEl && labelColorEnabledEl.checked) applyButtonColorsFromUI();
    });

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

    if (!payload.defaults || typeof payload.defaults !== 'object') {
        return { valid: false, message: 'Invalid payload (missing defaults)' };
    }
    const d = payload.defaults;
    for (const k of ['screen_bg', 'button_bg', 'icon_color', 'label_color']) {
        if (typeof d[k] !== 'number' || !isFinite(d[k])) {
            return { valid: false, message: `Invalid payload (defaults.${k} must be a number)` };
        }
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
    const onProgress = typeof options.onProgress === 'function' ? options.onProgress : null;

    // Normalize before sending.
    const payload = macrosNormalizePayload(macrosPayloadCache);

    // If a user entered an emoji icon, auto-install it now and fill in `icon.id`.
    const auto = await macrosAutoInstallEmojiIcons(payload, { silent, onProgress });
    if (!auto.ok) {
        // Even when running as part of the broader Home "Save" workflow, an error here
        // should be visible to the user (otherwise they only see a generic macros save failure).
        showMessage(auto.message || 'Failed to install emoji icons', 'error');
        return false;
    }

    // Clamp before sending.
    for (let s = 0; s < macrosScreenCount; s++) {
        for (let b = 0; b < macrosButtonsPerScreen; b++) {
            const btn = payload.screens[s].buttons[b];
            btn.label = macrosClampString(btn.label, MACROS_LABEL_MAX);
            btn.action = (btn.action || 'none');
            btn.payload = macrosClampString(btn.payload, MACROS_PAYLOAD_MAX);
            macrosEnsureIconObject(btn);
            btn.icon.type = (btn.icon.type || 'none').toString();
            if (!['none', 'builtin', 'emoji', 'asset'].includes(btn.icon.type)) btn.icon.type = 'none';
            btn.icon.id = macrosClampString(btn.icon.id, MACROS_ICON_ID_MAX);
            btn.icon.display = macrosClampUtf8Bytes(btn.icon.display, MACROS_ICON_DISPLAY_MAX);

            if (!macrosActionSupportsPayload(btn.action)) btn.payload = '';
            if (btn.action === 'nav_to') {
                // Dropdown-only: coerce to a valid screen id when possible.
                // If the screen list isn't available yet, keep the payload as-is.
                const screens = macrosGetAvailableScreens();
                if (screens.length > 0 && !macrosIsValidScreenId(btn.payload)) {
                    btn.payload = screens.some(s2 => s2.id === 'macro1') ? 'macro1' : screens[0].id;
                }
            }
            if (btn.action === 'none') {
                btn.icon.type = 'none';
                btn.icon.id = '';
                btn.icon.display = '';
            }
            if (btn.icon.type === 'none') {
                btn.icon.id = '';
                btn.icon.display = '';
            }
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

        // Best-effort: keep FFat tidy by deleting unused installed icons after macro changes.
        // No UI control is exposed; errors are intentionally non-fatal.
        if (onProgress) onProgress('Cleaning up unused icons...');
        try {
            const gcRes = await fetch(API_ICONS_GC, { method: 'POST', cache: 'no-cache' });
            const gcData = await gcRes.json().catch(() => ({}));
            if (gcRes.ok && gcData && gcData.success !== false) {
                if (onProgress) {
                    const deleted = Number(gcData.deleted || 0);
                    const bytes = Number(gcData.bytes_freed || 0);
                    onProgress(`Icon cleanup done (${deleted} removed, ${bytes} bytes freed).`);
                }
            } else {
                console.warn('Icon cleanup failed:', gcData);
                if (onProgress) onProgress('Icon cleanup skipped (device returned an error).');
            }
        } catch (e) {
            console.warn('Icon cleanup error:', e);
            if (onProgress) onProgress('Icon cleanup skipped (request failed).');
        }

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
    return await saveMacros({
        silent: options.silent === true,
        onProgress: options.onProgress,
    });
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

// ===== REBOOT DIALOG STEP LOG =====

function rebootDialogClearSteps() {
    const details = document.getElementById('reboot-details');
    if (!details) return;
    details.innerHTML = '';
    details.style.display = 'none';
}

function rebootDialogAppendStep(text) {
    const details = document.getElementById('reboot-details');
    if (!details) return;
    const t = (text || '').toString().trim();
    if (!t) return;

    details.style.display = 'block';

    const row = document.createElement('div');
    row.className = 'step';

    const dot = document.createElement('span');
    dot.className = 'dot';

    const label = document.createElement('span');
    label.textContent = t;

    row.appendChild(dot);
    row.appendChild(label);
    details.appendChild(row);
    details.scrollTop = details.scrollHeight;
}

function hideRebootDialog() {
    const overlay = document.getElementById('reboot-overlay');
    if (overlay) overlay.style.display = 'none';
    rebootDialogClearSteps();
}

// ===== OVERLAY SCROLL LOCK =====

function portalIsElementVisible(el) {
    if (!el) return false;
    const style = window.getComputedStyle(el);
    return style && style.display !== 'none';
}

function portalUpdateOverlayScrollLock() {
    const overlays = document.querySelectorAll('.reboot-overlay, .form-loading-overlay');
    const anyOpen = Array.from(overlays).some(portalIsElementVisible);
    document.documentElement.classList.toggle('portal-overlay-open', anyOpen);
    document.body.classList.toggle('portal-overlay-open', anyOpen);
}

function initOverlayScrollLock() {
    portalUpdateOverlayScrollLock();

    const overlays = document.querySelectorAll('.reboot-overlay, .form-loading-overlay');
    if (!overlays || overlays.length === 0) return;

    const observer = new MutationObserver(() => {
        portalUpdateOverlayScrollLock();
    });

    for (const el of overlays) {
        observer.observe(el, {
            attributes: true,
            attributeFilter: ['style', 'class'],
        });
    }
}

/**
 * Show unified reboot overlay and handle reconnection
 * @param {Object} options - Configuration options
 * @param {string} options.title - Dialog title (e.g., 'Device Rebooting')
 * @param {string} options.message - Main message to display
 * @param {string} options.context - Context: 'save', 'ota', 'reboot', 'reset'
 * @param {string} options.newDeviceName - Optional new device name if changed
 * @param {boolean} options.showProgress - Show progress bar (for OTA)
 * @param {boolean} options.autoReconnect - Start reconnection polling (default true)
 */
function showRebootDialog(options) {
    const {
        title = 'Device Rebooting',
        message = 'Please wait while the device restarts...',
        context = 'reboot',
        newDeviceName = null,
        showProgress = false,
        autoReconnect = true
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

    // Reset step log for the new operation.
    rebootDialogClearSteps();
    
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

    // Save-only (no reboot): use the same dialog surface, but don't start reconnection.
    if (context === 'save_only') {
        rebootSubMsg.textContent = '';
        reconnectStatus.style.display = 'none';
        overlay.style.display = 'flex';
        return;
    }

    // Some callers show the dialog while doing work *before* triggering a reboot
    // (e.g. saving macros / installing emoji icons). In that case we must not start
    // reconnection polling yet, or it will succeed immediately and reload the page.
    if (!autoReconnect) {
        rebootSubMsg.textContent = '';
        reconnectStatus.style.display = 'none';
        overlay.style.display = 'flex';
        return;
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

    // Make the reconnect panel visibly "active" immediately (not only after the first poll tick).
    reconnectStatus.textContent = 'Waiting for device to restart…';

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

    const startedAt = Date.now();
    let sawOffline = false;
    let offlineWaits = 0;
    const offlineWaitInterval = 1000;
    const maxOfflineWaits = 15; // ~15s max waiting for the reboot to actually start
    
    let attempts = 0;
    const maxAttempts = 40;
    const checkInterval = 3000; // Poll every 3 seconds
    
    // Determine target URL
    let targetUrl = null;
    if (newDeviceName) {
        const mdnsName = sanitizeForMDNS(newDeviceName);
        targetUrl = `http://${mdnsName}.local`;
    }
    
    const urlsToTry = targetUrl
        ? [targetUrl + API_VERSION, window.location.origin + API_VERSION]
        : [window.location.origin + API_VERSION];

    const tryAnyUrl = async () => {
        for (const url of urlsToTry) {
            try {
                const response = await fetch(url, {
                    cache: 'no-cache',
                    mode: 'cors',
                    signal: AbortSignal.timeout(2000),
                });
                if (response.ok) return true;
            } catch (_) {
                // ignore
            }
        }
        return false;
    };

    const checkConnection = async () => {
        attempts++;

        const elapsedSec = Math.max(0, Math.round((Date.now() - startedAt) / 1000));
        statusElement.textContent = `Reconnecting (attempt ${attempts}/${maxAttempts}, ${elapsedSec}s)…`;
        
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

    // Phase 1: show UI immediately, and wait until the device actually goes offline.
    const waitForOfflineThenReconnect = async () => {
        const elapsedSec = Math.max(0, Math.round((Date.now() - startedAt) / 1000));
        statusElement.textContent = `Waiting for reboot to start… (${elapsedSec}s)`;

        const online = await tryAnyUrl();
        if (!online) {
            sawOffline = true;
        }

        if (sawOffline) {
            statusElement.textContent = 'Device offline. Waiting to reconnect…';
            checkConnection();
            return;
        }

        offlineWaits++;
        if (offlineWaits >= maxOfflineWaits) {
            // If we never observe an offline transition, proceed anyway.
            checkConnection();
            return;
        }

        setTimeout(waitForOfflineThenReconnect, offlineWaitInterval);
    };

    waitForOfflineThenReconnect();
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
    const currentDeviceNameField = document.getElementById('device_name');
    const currentDeviceName = currentDeviceNameField ? currentDeviceNameField.value : null;
    
    // Always use the blocking dialog for Save & Reboot.
    showRebootDialog({
        title: 'Saving Configuration',
        message: 'Saving…',
        context: 'save',
        newDeviceName: currentDeviceName
    });

    rebootDialogAppendStep('Validating settings…');

    // Home page: persist macros as part of the same save workflow.
    const needMacrosSave = (currentPage === 'home' && typeof macrosDirty !== 'undefined' && macrosDirty);
    if (needMacrosSave) {
        rebootDialogAppendStep('Saving macros…');
    }

    const macrosOk = await saveMacrosIfNeeded({
        silent: true,
        onProgress: needMacrosSave ? (text) => rebootDialogAppendStep(text) : undefined,
    });
    if (!macrosOk) {
        hideRebootDialog();
        showMessage('Error saving macros (configuration not saved)', 'error');
        return;
    }

    rebootDialogAppendStep('Saving configuration…');

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
            rebootDialogAppendStep('Saved. Device is rebooting…');
        }
    } catch (error) {
        // If save request fails (e.g., device already rebooting), assume success
        if (error.message.includes('Failed to fetch') || error.message.includes('NetworkError')) {
            document.getElementById('reboot-message').textContent = 'Configuration saved. Device is rebooting...';
            rebootDialogAppendStep('Saved. Device is rebooting…');
        } else {
            // Hide overlay and show error
            hideRebootDialog();
            showMessage('Error saving configuration: ' + error.message, 'error');
            console.error('Save error:', error);
            return;
        }
    }
}

/**
 * Save configuration without rebooting
 */
async function saveOnly(event) {
    event.preventDefault();

    // Always use the blocking dialog for Save (no reboot).
    showRebootDialog({
        title: 'Saving Configuration',
        message: 'Saving…',
        context: 'save_only'
    });

    rebootDialogAppendStep('Validating settings…');

    // Home page: persist macros as part of the same save workflow.
    const needMacrosSave = (currentPage === 'home' && typeof macrosDirty !== 'undefined' && macrosDirty);
    if (needMacrosSave) {
        rebootDialogAppendStep('Saving macros…');
    }

    const macrosOk = await saveMacrosIfNeeded({
        silent: true,
        onProgress: needMacrosSave ? (text) => rebootDialogAppendStep(text) : undefined,
    });
    if (!macrosOk) {
        hideRebootDialog();
        showMessage('Error saving macros (configuration not saved)', 'error');
        return;
    }
    
    const formData = new FormData(document.getElementById('config-form'));
    const config = extractFormFields(formData);
    
    // Validate configuration
    const validation = validateConfig(config);
    if (!validation.valid) {
        hideRebootDialog();
        showMessage(validation.message, 'error');
        return;
    }
    
    try {
        rebootDialogAppendStep('Saving configuration…');
        
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
            document.getElementById('reboot-message').textContent = 'Configuration saved.';
            rebootDialogAppendStep('Saved successfully.');
            setTimeout(() => hideRebootDialog(), 1000);
        } else {
            hideRebootDialog();
            showMessage('Failed to save configuration', 'error');
        }
    } catch (error) {
        hideRebootDialog();
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
    // Prevent background page scroll whenever any overlay is open.
    initOverlayScrollLock();

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

    // Home page: DuckyScript help modal
    initDuckyHelpDialog();
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

const HEALTH_POLL_INTERVAL_DEFAULT_MS = 5000;
const HEALTH_HISTORY_DEFAULT_SECONDS = 300;
let healthPollIntervalMs = HEALTH_POLL_INTERVAL_DEFAULT_MS;
let healthHistoryMaxSamples = 60;

const healthHistory = {
    cpu: [],
    cpuTs: [],
    heapInternalFree: [],
    heapInternalFreeTs: [],
    heapInternalFreeMin: [],
    heapInternalFreeMax: [],
    psramFree: [],
    psramFreeTs: [],
    psramFreeMin: [],
    psramFreeMax: [],
    wifiRssi: [],
    wifiRssiTs: [],
};

const healthSeriesStats = {
    cpu: { min: null, max: null },
    heapInternalFree: { min: null, max: null },
    psramFree: { min: null, max: null },
    wifiRssi: { min: null, max: null },
};

function healthComputeMinMaxMulti(arrays) {
    const list = Array.isArray(arrays) ? arrays : [];
    let min = Infinity;
    let max = -Infinity;
    let seen = false;

    for (let k = 0; k < list.length; k++) {
        const arr = list[k];
        if (!Array.isArray(arr) || arr.length < 1) continue;
        for (let i = 0; i < arr.length; i++) {
            const v = arr[i];
            if (typeof v !== 'number' || !isFinite(v)) continue;
            seen = true;
            if (v < min) min = v;
            if (v > max) max = v;
        }
    }

    if (!seen || !isFinite(min) || !isFinite(max)) return { min: null, max: null };
    return { min, max };
}

function healthUpdateSeriesStats({ hasPsram = null } = {}) {
    const resolvedHasPsram = (typeof hasPsram === 'boolean') ? hasPsram : (healthHistory.psramFree && healthHistory.psramFree.length > 0);
    {
        const mm = healthComputeMinMaxMulti([healthHistory.cpu]);
        healthSeriesStats.cpu.min = mm.min;
        healthSeriesStats.cpu.max = mm.max;
    }
    {
        // Heap sparkline draws both the point series and the window band.
        const mm = healthComputeMinMaxMulti([
            healthHistory.heapInternalFree,
            healthHistory.heapInternalFreeMin,
            healthHistory.heapInternalFreeMax,
        ]);
        healthSeriesStats.heapInternalFree.min = mm.min;
        healthSeriesStats.heapInternalFree.max = mm.max;
    }
    if (resolvedHasPsram) {
        // PSRAM sparkline draws both the point series and the window band.
        const mm = healthComputeMinMaxMulti([
            healthHistory.psramFree,
            healthHistory.psramFreeMin,
            healthHistory.psramFreeMax,
        ]);
        healthSeriesStats.psramFree.min = mm.min;
        healthSeriesStats.psramFree.max = mm.max;
    } else {
        healthSeriesStats.psramFree.min = null;
        healthSeriesStats.psramFree.max = null;
    }
    {
        const mm = healthComputeMinMaxMulti([healthHistory.wifiRssi]);
        healthSeriesStats.wifiRssi.min = mm.min;
        healthSeriesStats.wifiRssi.max = mm.max;
    }
}

function healthConfigureFromDeviceInfo(info) {
    const pollMs = (info && typeof info.health_poll_interval_ms === 'number') ? info.health_poll_interval_ms : HEALTH_POLL_INTERVAL_DEFAULT_MS;
    const windowSeconds = (info && typeof info.health_history_seconds === 'number') ? info.health_history_seconds : HEALTH_HISTORY_DEFAULT_SECONDS;

    // Clamp to sane values.
    healthPollIntervalMs = Math.max(1000, Math.min(60000, pollMs | 0));
    const seconds = Math.max(30, Math.min(3600, windowSeconds | 0));
    healthHistoryMaxSamples = Math.max(10, Math.min(600, Math.floor((seconds * 1000) / healthPollIntervalMs)));
}

function healthPushSample(arr, value) {
    if (!Array.isArray(arr)) return;
    if (typeof value !== 'number' || !isFinite(value)) return;
    arr.push(value);
    while (arr.length > healthHistoryMaxSamples) arr.shift();
}

function healthPushSampleWithTs(valuesArr, tsArr, value, ts) {
    if (!Array.isArray(valuesArr) || !Array.isArray(tsArr)) return;
    if (typeof value !== 'number' || !isFinite(value)) return;
    if (typeof ts !== 'number' || !isFinite(ts)) return;
    valuesArr.push(value);
    tsArr.push(ts);
    while (valuesArr.length > healthHistoryMaxSamples) valuesArr.shift();
    while (tsArr.length > healthHistoryMaxSamples) tsArr.shift();
}

function healthFormatAgeMs(ageMs) {
    if (typeof ageMs !== 'number' || !isFinite(ageMs)) return '';
    const s = Math.max(0, Math.round(ageMs / 1000));
    if (s < 60) return `${s}s ago`;
    const m = Math.floor(s / 60);
    const r = s % 60;
    if (m < 60) return `${m}m ${r}s ago`;
    const h = Math.floor(m / 60);
    const rm = m % 60;
    return `${h}h ${rm}m ago`;
}

function healthFormatTimeOfDay(ts) {
    try {
        return new Date(ts).toLocaleTimeString([], { hour12: false });
    } catch (_) {
        return '';
    }
}

let healthSparklineTooltipEl = null;
function healthEnsureSparklineTooltip() {
    if (healthSparklineTooltipEl) return healthSparklineTooltipEl;
    const el = document.createElement('div');
    el.className = 'health-sparkline-tooltip';
    el.style.display = 'none';
    document.body.appendChild(el);
    healthSparklineTooltipEl = el;
    return el;
}

function healthTooltipSetVisible(visible) {
    const el = healthEnsureSparklineTooltip();
    el.style.display = visible ? 'block' : 'none';
}

function healthTooltipSetContent(html) {
    const el = healthEnsureSparklineTooltip();
    el.innerHTML = html;
}

function healthTooltipSetPosition(clientX, clientY) {
    const el = healthEnsureSparklineTooltip();

    const pad = 12;
    let x = (clientX || 0) + pad;
    let y = (clientY || 0) + pad;

    // Keep the tooltip within the viewport.
    const vw = window.innerWidth || 0;
    const vh = window.innerHeight || 0;

    // Prevent the tooltip from becoming narrow when close to the right edge.
    // For positioned elements, shrink-to-fit width depends on remaining space.
    const maxW = (vw > 0) ? Math.max(140, vw - pad * 2) : 320;
    const desiredW = 280;
    el.style.width = `${Math.min(desiredW, maxW)}px`;
    el.style.maxWidth = `${maxW}px`;

    // Temporarily show to measure.
    const prevDisplay = el.style.display;
    el.style.display = 'block';
    const rect = el.getBoundingClientRect();
    el.style.display = prevDisplay;

    if (vw > 0 && rect.width > 0 && x + rect.width + pad > vw) {
        x = Math.max(pad, vw - rect.width - pad);
    }
    if (vh > 0 && rect.height > 0 && y + rect.height + pad > vh) {
        y = Math.max(pad, vh - rect.height - pad);
    }

    el.style.left = `${x}px`;
    el.style.top = `${y}px`;
}

function healthSparklineIndexFromEvent(canvas, clientX) {
    if (!canvas) return null;
    const rect = canvas.getBoundingClientRect();
    const w = rect.width || 0;
    if (w <= 0) return null;
    const x = (clientX - rect.left);
    const t = Math.max(0, Math.min(1, x / w));
    return t;
}

const healthSparklineHoverIndex = {
    'health-sparkline-cpu': null,
    'health-sparkline-heap': null,
    'health-sparkline-psram': null,
    'health-sparkline-rssi': null,
};

function healthSetSparklineHoverIndex(canvasId, index) {
    if (!canvasId) return;
    if (!(canvasId in healthSparklineHoverIndex)) return;
    if (typeof index !== 'number' || !isFinite(index)) {
        healthSparklineHoverIndex[canvasId] = null;
        return;
    }
    healthSparklineHoverIndex[canvasId] = index | 0;
}

function healthGetSparklineHoverIndex(canvasId) {
    if (!canvasId) return null;
    if (!(canvasId in healthSparklineHoverIndex)) return null;
    const v = healthSparklineHoverIndex[canvasId];
    return (typeof v === 'number' && isFinite(v)) ? (v | 0) : null;
}

function healthDrawSparklinesOnly({ hasPsram = null } = {}) {
    const resolvedHasPsram = (typeof hasPsram === 'boolean') ? hasPsram : (healthHistory.psramFree && healthHistory.psramFree.length > 0);

    sparklineDraw(document.getElementById('health-sparkline-cpu'), healthHistory.cpu, {
        color: '#667eea',
        min: 0,
        max: 100,
        highlightIndex: healthGetSparklineHoverIndex('health-sparkline-cpu'),
    });

    sparklineDraw(document.getElementById('health-sparkline-heap'), healthHistory.heapInternalFree, {
        color: '#34c759',
        bandMin: healthHistory.heapInternalFreeMin,
        bandMax: healthHistory.heapInternalFreeMax,
        bandColor: 'rgba(52, 199, 89, 0.18)',
        highlightIndex: healthGetSparklineHoverIndex('health-sparkline-heap'),
    });

    if (resolvedHasPsram) {
        sparklineDraw(document.getElementById('health-sparkline-psram'), healthHistory.psramFree, {
            color: '#0a84ff',
            bandMin: healthHistory.psramFreeMin,
            bandMax: healthHistory.psramFreeMax,
            bandColor: 'rgba(10, 132, 255, 0.18)',
            highlightIndex: healthGetSparklineHoverIndex('health-sparkline-psram'),
        });
    } else {
        // Still draw baseline placeholder if canvas exists.
        sparklineDraw(document.getElementById('health-sparkline-psram'), healthHistory.psramFree, {
            color: '#0a84ff',
            bandMin: healthHistory.psramFreeMin,
            bandMax: healthHistory.psramFreeMax,
            bandColor: 'rgba(10, 132, 255, 0.18)',
            highlightIndex: null,
        });
    }

    sparklineDraw(document.getElementById('health-sparkline-rssi'), healthHistory.wifiRssi, {
        color: '#ff9500',
        min: -100,
        max: -30,
        highlightIndex: healthGetSparklineHoverIndex('health-sparkline-rssi'),
    });
}

function healthAttachSparklineTooltip(canvas, getPayloadForIndex) {
    if (!canvas || typeof getPayloadForIndex !== 'function') return;
    if (canvas.dataset && canvas.dataset.healthTooltipAttached === '1') return;
    if (canvas.dataset) canvas.dataset.healthTooltipAttached = '1';

    let hideTimer = null;
    const clearHideTimer = () => {
        if (hideTimer) {
            clearTimeout(hideTimer);
            hideTimer = null;
        }
    };

    const hide = () => {
        clearHideTimer();
        healthSetSparklineHoverIndex(canvas.id, null);
        healthDrawSparklinesOnly({
            hasPsram: (() => {
                const wrap = document.getElementById('health-sparkline-psram-wrap');
                return wrap ? (wrap.style.display !== 'none') : null;
            })(),
        });
        healthTooltipSetVisible(false);
    };

    const showAt = (clientX, clientY) => {
        clearHideTimer();
        const t = healthSparklineIndexFromEvent(canvas, clientX);
        if (t === null) return;

        const payload = getPayloadForIndex(t);
        if (!payload) return;

        if (typeof payload.index === 'number' && isFinite(payload.index)) {
            const prev = healthGetSparklineHoverIndex(canvas.id);
            const next = payload.index | 0;
            if (prev !== next) {
                healthSetSparklineHoverIndex(canvas.id, next);
                healthDrawSparklinesOnly({
                    hasPsram: (() => {
                        const wrap = document.getElementById('health-sparkline-psram-wrap');
                        return wrap ? (wrap.style.display !== 'none') : null;
                    })(),
                });
            }
        }

        healthTooltipSetContent(payload.html);
        healthTooltipSetPosition(clientX, clientY);
        healthTooltipSetVisible(true);
    };

    canvas.addEventListener('mousemove', (e) => {
        showAt(e.clientX, e.clientY);
    });
    canvas.addEventListener('mouseleave', hide);

    // Touch: tap/drag shows tooltip; auto-hide shortly after touch ends.
    canvas.addEventListener('touchstart', (e) => {
        if (!e.touches || e.touches.length < 1) return;
        const t0 = e.touches[0];
        showAt(t0.clientX, t0.clientY);
    }, { passive: true });
    canvas.addEventListener('touchmove', (e) => {
        if (!e.touches || e.touches.length < 1) return;
        const t0 = e.touches[0];
        showAt(t0.clientX, t0.clientY);
    }, { passive: true });
    canvas.addEventListener('touchend', () => {
        clearHideTimer();
        hideTimer = setTimeout(hide, 1200);
    }, { passive: true });
}

function healthInitSparklineTooltips() {
    const cpuCanvas = document.getElementById('health-sparkline-cpu');
    healthAttachSparklineTooltip(cpuCanvas, (t) => {
        const v = healthHistory.cpu;
        const ts = healthHistory.cpuTs;
        const n = v.length;
        if (n < 1) return null;
        const i = Math.max(0, Math.min(n - 1, Math.round(t * (n - 1))));
        const val = v[i];
        const tsv = ts[i];
        const age = healthFormatAgeMs(Date.now() - tsv);
        const tod = healthFormatTimeOfDay(tsv);
        const smin = healthSeriesStats.cpu.min;
        const smax = healthSeriesStats.cpu.max;
        const srange = (typeof smin === 'number' && typeof smax === 'number') ? `${(smin | 0)}% – ${(smax | 0)}%` : '—';
        const sdelta = (typeof smin === 'number' && typeof smax === 'number') ? `${Math.max(0, (smax | 0) - (smin | 0))}%` : '—';
        return {
            index: i,
            html: `<div class="health-sparkline-tooltip-title">CPU Usage</div>` +
                `<div class="health-sparkline-tooltip-row"><span>${tod}</span><span>${age}</span></div>` +
                `<div class="health-sparkline-tooltip-value">${val}%</div>` +
                `<div class="health-sparkline-tooltip-sub">Sparkline min/max: ${srange} (Δ ${sdelta})</div>`,
        };
    });

    const heapCanvas = document.getElementById('health-sparkline-heap');
    healthAttachSparklineTooltip(heapCanvas, (t) => {
        const v = healthHistory.heapInternalFree;
        const ts = healthHistory.heapInternalFreeTs;
        const bmin = healthHistory.heapInternalFreeMin;
        const bmax = healthHistory.heapInternalFreeMax;
        const n = v.length;
        if (n < 1) return null;
        const i = Math.max(0, Math.min(n - 1, Math.round(t * (n - 1))));
        const val = v[i];
        const tsv = ts[i];
        const wmin = (i < bmin.length) ? bmin[i] : val;
        const wmax = (i < bmax.length) ? bmax[i] : val;
        const age = healthFormatAgeMs(Date.now() - tsv);
        const tod = healthFormatTimeOfDay(tsv);
        const range = (typeof wmin === 'number' && typeof wmax === 'number') ? `${healthFormatBytes(wmin)} – ${healthFormatBytes(wmax)}` : '—';
        const delta = (typeof wmin === 'number' && typeof wmax === 'number') ? healthFormatBytes(Math.max(0, wmax - wmin)) : '—';
        const smin = healthSeriesStats.heapInternalFree.min;
        const smax = healthSeriesStats.heapInternalFree.max;
        const srange = (typeof smin === 'number' && typeof smax === 'number') ? `${healthFormatBytes(smin)} – ${healthFormatBytes(smax)}` : '—';
        const sdelta = (typeof smin === 'number' && typeof smax === 'number') ? healthFormatBytes(Math.max(0, smax - smin)) : '—';
        return {
            index: i,
            html: `<div class="health-sparkline-tooltip-title">Internal Free Heap</div>` +
                `<div class="health-sparkline-tooltip-row"><span>${tod}</span><span>${age}</span></div>` +
                `<div class="health-sparkline-tooltip-value">${healthFormatBytes(val)}</div>` +
            `<div class="health-sparkline-tooltip-sub">Window min/max: ${range} (Δ ${delta})</div>` +
            `<div class="health-sparkline-tooltip-sub">Sparkline min/max: ${srange} (Δ ${sdelta})</div>`,
        };
    });

    const psramCanvas = document.getElementById('health-sparkline-psram');
    healthAttachSparklineTooltip(psramCanvas, (t) => {
        const v = healthHistory.psramFree;
        const ts = healthHistory.psramFreeTs;
        const bmin = healthHistory.psramFreeMin;
        const bmax = healthHistory.psramFreeMax;
        const n = v.length;
        if (n < 1) return null;
        const i = Math.max(0, Math.min(n - 1, Math.round(t * (n - 1))));
        const val = v[i];
        const tsv = ts[i];
        const wmin = (i < bmin.length) ? bmin[i] : val;
        const wmax = (i < bmax.length) ? bmax[i] : val;
        const age = healthFormatAgeMs(Date.now() - tsv);
        const tod = healthFormatTimeOfDay(tsv);
        const range = (typeof wmin === 'number' && typeof wmax === 'number') ? `${healthFormatBytes(wmin)} – ${healthFormatBytes(wmax)}` : '—';
        const delta = (typeof wmin === 'number' && typeof wmax === 'number') ? healthFormatBytes(Math.max(0, wmax - wmin)) : '—';
        const smin = healthSeriesStats.psramFree.min;
        const smax = healthSeriesStats.psramFree.max;
        const srange = (typeof smin === 'number' && typeof smax === 'number') ? `${healthFormatBytes(smin)} – ${healthFormatBytes(smax)}` : '—';
        const sdelta = (typeof smin === 'number' && typeof smax === 'number') ? healthFormatBytes(Math.max(0, smax - smin)) : '—';
        return {
            index: i,
            html: `<div class="health-sparkline-tooltip-title">PSRAM Free</div>` +
                `<div class="health-sparkline-tooltip-row"><span>${tod}</span><span>${age}</span></div>` +
                `<div class="health-sparkline-tooltip-value">${healthFormatBytes(val)}</div>` +
            `<div class="health-sparkline-tooltip-sub">Window min/max: ${range} (Δ ${delta})</div>` +
            `<div class="health-sparkline-tooltip-sub">Sparkline min/max: ${srange} (Δ ${sdelta})</div>`,
        };
    });

    const rssiCanvas = document.getElementById('health-sparkline-rssi');
    healthAttachSparklineTooltip(rssiCanvas, (t) => {
        const v = healthHistory.wifiRssi;
        const ts = healthHistory.wifiRssiTs;
        const n = v.length;
        if (n < 1) return null;
        const i = Math.max(0, Math.min(n - 1, Math.round(t * (n - 1))));
        const val = v[i];
        const tsv = ts[i];
        const age = healthFormatAgeMs(Date.now() - tsv);
        const tod = healthFormatTimeOfDay(tsv);
        const smin = healthSeriesStats.wifiRssi.min;
        const smax = healthSeriesStats.wifiRssi.max;
        const srange = (typeof smin === 'number' && typeof smax === 'number') ? `${(smin | 0)} – ${(smax | 0)} dBm` : '—';
        const sdelta = (typeof smin === 'number' && typeof smax === 'number') ? `${Math.max(0, (smax | 0) - (smin | 0))} dBm` : '—';
        return {
            index: i,
            html: `<div class="health-sparkline-tooltip-title">WiFi RSSI</div>` +
                `<div class="health-sparkline-tooltip-row"><span>${tod}</span><span>${age}</span></div>` +
                `<div class="health-sparkline-tooltip-value">${val} dBm</div>` +
                `<div class="health-sparkline-tooltip-sub">Sparkline min/max: ${srange} (Δ ${sdelta})</div>`,
        };
    });
}

function healthFormatBytes(bytes) {
    if (typeof bytes !== 'number' || !isFinite(bytes)) return '—';
    return formatHeap(bytes);
}

function sparklineDraw(canvas, values, {
    color = '#667eea',
    strokeWidth = 2,
    min = null,
    max = null,
    bandMin = null,
    bandMax = null,
    bandColor = 'rgba(102, 126, 234, 0.18)',
    highlightIndex = null,
    highlightRadius = 3.25,
    highlightFill = 'rgba(255,255,255,0.95)',
    highlightStroke = null,
    highlightStrokeWidth = 2,
} = {}) {
    if (!canvas) return;
    const ctx = canvas.getContext('2d');
    if (!ctx) return;

    const w = canvas.width;
    const h = canvas.height;
    ctx.clearRect(0, 0, w, h);

    const data = Array.isArray(values) ? values : [];
    if (data.length < 1) {
        // Placeholder baseline.
        ctx.strokeStyle = 'rgba(0,0,0,0.08)';
        ctx.lineWidth = 1;
        ctx.beginPath();
        ctx.moveTo(0, h - 1);
        ctx.lineTo(w, h - 1);
        ctx.stroke();
        return;
    }

    const bandMinArr = Array.isArray(bandMin) ? bandMin : null;
    const bandMaxArr = Array.isArray(bandMax) ? bandMax : null;

    let vmin = (typeof min === 'number') ? min : Infinity;
    let vmax = (typeof max === 'number') ? max : -Infinity;
    if (!(typeof min === 'number') || !(typeof max === 'number')) {
        for (let i = 0; i < data.length; i++) {
            const v = data[i];
            if (typeof v === 'number' && isFinite(v)) {
                if (v < vmin) vmin = v;
                if (v > vmax) vmax = v;
            }
            if (bandMinArr && i < bandMinArr.length) {
                const bmin = bandMinArr[i];
                if (typeof bmin === 'number' && isFinite(bmin)) {
                    if (bmin < vmin) vmin = bmin;
                    if (bmin > vmax) vmax = bmin;
                }
            }
            if (bandMaxArr && i < bandMaxArr.length) {
                const bmax = bandMaxArr[i];
                if (typeof bmax === 'number' && isFinite(bmax)) {
                    if (bmax < vmin) vmin = bmax;
                    if (bmax > vmax) vmax = bmax;
                }
            }
        }
    }
    if (!isFinite(vmin) || !isFinite(vmax)) {
        vmin = 0;
        vmax = 1;
    } else if (vmin === vmax) {
        // Flat series: expand around the value so it renders on-canvas.
        const eps = Math.max(1, Math.abs(vmin) * 0.01);
        vmin = vmin - eps;
        vmax = vmax + eps;
    }

    const pad = 4;
    const xStep = (data.length >= 2) ? ((w - pad * 2) / (data.length - 1)) : 0;
    const yScale = (h - pad * 2) / (vmax - vmin);

    // Subtle grid baseline
    ctx.strokeStyle = 'rgba(0,0,0,0.06)';
    ctx.lineWidth = 1;
    ctx.beginPath();
    ctx.moveTo(0, h - 1);
    ctx.lineTo(w, h - 1);
    ctx.stroke();

    // Optional min/max band behind the line.
    if (bandMinArr && bandMaxArr && data.length >= 2) {
        const n = Math.min(data.length, bandMinArr.length, bandMaxArr.length);
        if (n >= 2) {
            ctx.fillStyle = bandColor;
            ctx.beginPath();
            for (let i = 0; i < n; i++) {
                const bmax = bandMaxArr[i];
                if (typeof bmax !== 'number' || !isFinite(bmax)) continue;
                const x = pad + i * xStep;
                const y = h - pad - ((bmax - vmin) * yScale);
                if (i === 0) ctx.moveTo(x, y);
                else ctx.lineTo(x, y);
            }
            for (let i = n - 1; i >= 0; i--) {
                const bmin = bandMinArr[i];
                if (typeof bmin !== 'number' || !isFinite(bmin)) continue;
                const x = pad + i * xStep;
                const y = h - pad - ((bmin - vmin) * yScale);
                ctx.lineTo(x, y);
            }
            ctx.closePath();
            ctx.fill();
        }
    }

    ctx.strokeStyle = color;
    ctx.lineWidth = strokeWidth;
    ctx.lineJoin = 'round';
    ctx.lineCap = 'round';

    if (data.length === 1) {
        // Single sample: draw a small marker so the user sees "something" immediately.
        const v = data[0];
        const x = pad;
        const y = h - pad - ((v - vmin) * yScale);
        ctx.beginPath();
        ctx.arc(x, y, 2.5, 0, Math.PI * 2);
        ctx.stroke();

        if (highlightIndex === 0) {
            const strokeCol = highlightStroke || color;
            ctx.fillStyle = highlightFill;
            ctx.beginPath();
            ctx.arc(x, y, Math.max(2.5, highlightRadius), 0, Math.PI * 2);
            ctx.fill();
            ctx.strokeStyle = strokeCol;
            ctx.lineWidth = highlightStrokeWidth;
            ctx.beginPath();
            ctx.arc(x, y, Math.max(2.5, highlightRadius), 0, Math.PI * 2);
            ctx.stroke();
        }
        return;
    }

    ctx.beginPath();
    for (let i = 0; i < data.length; i++) {
        const v = data[i];
        const x = pad + i * xStep;
        const y = h - pad - ((v - vmin) * yScale);
        if (i === 0) ctx.moveTo(x, y);
        else ctx.lineTo(x, y);
    }
    ctx.stroke();

    if (typeof highlightIndex === 'number' && isFinite(highlightIndex)) {
        const idx = Math.max(0, Math.min(data.length - 1, highlightIndex | 0));
        const v = data[idx];
        if (typeof v === 'number' && isFinite(v)) {
            const x = pad + idx * xStep;
            const y = h - pad - ((v - vmin) * yScale);
            const strokeCol = highlightStroke || color;
            const r = Math.max(2.0, highlightRadius);

            ctx.fillStyle = highlightFill;
            ctx.beginPath();
            ctx.arc(x, y, r, 0, Math.PI * 2);
            ctx.fill();

            ctx.strokeStyle = strokeCol;
            ctx.lineWidth = highlightStrokeWidth;
            ctx.beginPath();
            ctx.arc(x, y, r, 0, Math.PI * 2);
            ctx.stroke();
        }
    }
}

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

        const sampleTs = Date.now();

        const cpuUsage = (typeof health.cpu_usage === 'number' && isFinite(health.cpu_usage)) ? Math.floor(health.cpu_usage) : null;

        const hasPsram = (
            (deviceInfoCache && typeof deviceInfoCache.psram_size === 'number' && deviceInfoCache.psram_size > 0) ||
            (typeof health.psram_free === 'number' && health.psram_free > 0)
        );

        // Feed client-side history buffers (no device-side time series).
        if (cpuUsage !== null) {
            healthPushSampleWithTs(healthHistory.cpu, healthHistory.cpuTs, cpuUsage, sampleTs);
        }
        healthPushSampleWithTs(healthHistory.heapInternalFree, healthHistory.heapInternalFreeTs, health.heap_internal_free, sampleTs);
        {
            const cur = health.heap_internal_free;
            const wmin = (typeof health.heap_internal_free_min_window === 'number') ? Math.min(health.heap_internal_free_min_window, cur) : cur;
            const wmax = (typeof health.heap_internal_free_max_window === 'number') ? Math.max(health.heap_internal_free_max_window, cur) : cur;
            healthPushSample(healthHistory.heapInternalFreeMin, wmin);
            healthPushSample(healthHistory.heapInternalFreeMax, wmax);
        }
        if (hasPsram) {
            healthPushSampleWithTs(healthHistory.psramFree, healthHistory.psramFreeTs, health.psram_free, sampleTs);
            {
                const cur = health.psram_free;
                const wmin = (typeof health.psram_free_min_window === 'number') ? Math.min(health.psram_free_min_window, cur) : cur;
                const wmax = (typeof health.psram_free_max_window === 'number') ? Math.max(health.psram_free_max_window, cur) : cur;
                healthPushSample(healthHistory.psramFreeMin, wmin);
                healthPushSample(healthHistory.psramFreeMax, wmax);
            }
        }
        if (health.wifi_rssi !== null && health.wifi_rssi !== undefined) {
            healthPushSampleWithTs(healthHistory.wifiRssi, healthHistory.wifiRssiTs, health.wifi_rssi, sampleTs);
        }

        // Derived stats used by the sparklines tooltips.
        healthUpdateSeriesStats({ hasPsram });
        
        // Update compact view
        document.getElementById('health-cpu').textContent = (cpuUsage !== null) ? `CPU ${cpuUsage}%` : 'CPU —';
        
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
        document.getElementById('health-cpu-full').textContent = (cpuUsage !== null) ? `${cpuUsage}%` : '—';
        if (cpuUsage !== null && typeof health.cpu_usage_min === 'number' && typeof health.cpu_usage_max === 'number') {
            document.getElementById('health-cpu-minmax').textContent = `${health.cpu_usage_min}% / ${health.cpu_usage_max}%`;
        } else {
            document.getElementById('health-cpu-minmax').textContent = '—';
        }
        document.getElementById('health-temp').textContent = health.cpu_temperature !== null ? 
            `${health.cpu_temperature}°C` : 'N/A';

        // Sparklines
        const cpuSparkValue = document.getElementById('health-sparkline-cpu-value');
        if (cpuSparkValue) cpuSparkValue.textContent = (cpuUsage !== null) ? `${cpuUsage}%` : '—';

        const heapSparkValue = document.getElementById('health-sparkline-heap-value');
        if (heapSparkValue) heapSparkValue.textContent = healthFormatBytes(health.heap_internal_free);

        const psramWrap = document.getElementById('health-sparkline-psram-wrap');
        if (psramWrap) psramWrap.style.display = hasPsram ? '' : 'none';
        const psramSparkValue = document.getElementById('health-sparkline-psram-value');
        if (psramSparkValue) psramSparkValue.textContent = hasPsram ? healthFormatBytes(health.psram_free) : '—';

        const rssiSparkValue = document.getElementById('health-sparkline-rssi-value');
        if (rssiSparkValue) {
            rssiSparkValue.textContent = (health.wifi_rssi !== null && health.wifi_rssi !== undefined) ? `${health.wifi_rssi} dBm` : 'N/A';
        }
        healthDrawSparklinesOnly({ hasPsram });
        
        // Memory
        document.getElementById('health-heap').textContent = formatHeap(health.heap_free);
        document.getElementById('health-heap-min').textContent = formatHeap(health.heap_min);
        if (typeof health.heap_fragmentation_max_window === 'number') {
            document.getElementById('health-heap-frag').textContent = `${health.heap_fragmentation}% (max ${health.heap_fragmentation_max_window}%)`;
        } else {
            document.getElementById('health-heap-frag').textContent = `${health.heap_fragmentation}%`;
        }
        const internalMin = document.getElementById('health-internal-min');
        if (internalMin) internalMin.textContent = healthFormatBytes(health.heap_internal_min);

        const internalLargest = document.getElementById('health-internal-largest');
        if (internalLargest) {
            if (typeof health.heap_internal_largest_min_window === 'number') {
                internalLargest.textContent = `${healthFormatBytes(health.heap_internal_largest)} (min ${healthFormatBytes(health.heap_internal_largest_min_window)})`;
            } else {
                internalLargest.textContent = healthFormatBytes(health.heap_internal_largest);
            }
        }

        const psramMinWrap = document.getElementById('health-psram-min-wrap');
        if (psramMinWrap) psramMinWrap.style.display = hasPsram ? '' : 'none';
        const psramMin = document.getElementById('health-psram-min');
        if (psramMin) psramMin.textContent = hasPsram ? healthFormatBytes(health.psram_min) : '—';

        const psramFragWrap = document.getElementById('health-psram-frag-wrap');
        if (psramFragWrap) psramFragWrap.style.display = hasPsram ? '' : 'none';
        const psramFrag = document.getElementById('health-psram-frag');
        if (psramFrag) {
            if (hasPsram && typeof health.psram_fragmentation_max_window === 'number') {
                psramFrag.textContent = `${health.psram_fragmentation}% (max ${health.psram_fragmentation_max_window}%)`;
            } else {
                psramFrag.textContent = hasPsram ? `${health.psram_fragmentation}%` : '—';
            }
        }
        
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

        // FS
        const fsEl = document.getElementById('health-fs');
        if (fsEl) {
            const t = health.fs_type;
            if (!t) {
                fsEl.textContent = 'N/A';
            } else if (health.fs_mounted === true && typeof health.fs_used_bytes === 'number' && typeof health.fs_total_bytes === 'number') {
                fsEl.textContent = `${t}: ${(health.fs_used_bytes / 1024).toFixed(0)} / ${(health.fs_total_bytes / 1024).toFixed(0)} KB`;
            } else {
                fsEl.textContent = `${t}: not mounted`;
            }
        }

        // Display
        const fpsEl = document.getElementById('health-display-fps');
        if (fpsEl) fpsEl.textContent = (health.display_fps !== null && health.display_fps !== undefined) ? `${health.display_fps} fps` : 'N/A';
        const timesEl = document.getElementById('health-display-times');
        if (timesEl) {
            if (typeof health.display_lv_timer_us === 'number' && typeof health.display_present_us === 'number') {
                timesEl.textContent = `${(health.display_lv_timer_us / 1000).toFixed(1)}ms / ${(health.display_present_us / 1000).toFixed(1)}ms`;
            } else {
                timesEl.textContent = 'N/A';
            }
        }

        // MQTT
        const mqttEl = document.getElementById('health-mqtt');
        if (mqttEl) {
            if (health.mqtt_enabled === false) mqttEl.textContent = 'Disabled';
            else if (health.mqtt_connected === true) mqttEl.textContent = 'Connected';
            else if (health.mqtt_connected === false) mqttEl.textContent = 'Disconnected';
            else mqttEl.textContent = 'N/A';
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
    } else {
        document.getElementById('health-expanded').style.display = 'none';
    }
}

// Initialize health widget
function initHealthWidget() {
    // Configure polling based on device info if available.
    healthConfigureFromDeviceInfo(deviceInfoCache);

    // Attach hover/touch tooltips once.
    healthInitSparklineTooltips();

    // Click health badge in header to expand
    const healthBadge = document.getElementById('health-badge');
    if (healthBadge) {
        healthBadge.addEventListener('click', toggleHealthWidget);
    }
    
    // Click close button to collapse
    document.getElementById('health-close').addEventListener('click', toggleHealthWidget);
    
    // Initial health fetch
    updateHealth();

    // Always poll at the configured interval so history is continuous.
    if (healthUpdateInterval) clearInterval(healthUpdateInterval);
    healthUpdateInterval = setInterval(updateHealth, healthPollIntervalMs);
}
