/**
 * Configuration Portal JavaScript
 * Handles configuration form, OTA updates, and device reboots
 * Supports core mode (AP) and full mode (WiFi connected)
 */

// API endpoints
const API_CONFIG = '/api/config';
const API_INFO = '/api/info';
const API_MODE = '/api/mode';
const API_UPDATE = '/api/update';
const API_VERSION = '/api/info'; // Used for connection polling

let selectedFile = null;
let portalMode = 'full'; // 'core' or 'full'

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
    const deviceName = document.getElementById('device_name').value;
    const sanitizedField = document.getElementById('device_name_sanitized');
    
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
        
        // Show/hide additional settings based on mode
        const additionalSettings = document.getElementById('additional-settings');
        if (portalMode === 'core') {
            additionalSettings.style.display = 'none';
        } else {
            additionalSettings.style.display = 'block';
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
        
        // Update build info in footer
        if (version.build_date && version.build_time) {
            // Format: "Built on Nov 25, 2025 at 14:46:48"
            const dateWithComma = version.build_date.replace(/(\w+ \d+) (\d{4})/, '$1, $2');
            document.getElementById('build-info').textContent = 
                `Built on ${dateWithComma} at ${version.build_time}`;
        }
    } catch (error) {
        document.getElementById('firmware-version').textContent = 'Firmware v?.?.?';
        document.getElementById('chip-info').textContent = 'Chip info unavailable';
        document.getElementById('cpu-cores').textContent = '? Cores';
        document.getElementById('cpu-freq').textContent = '? MHz';
        document.getElementById('flash-size').textContent = '? MB Flash';
        document.getElementById('psram-status').textContent = 'Unknown';
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
        const hasConfig = config.wifi_ssid && config.wifi_ssid !== '';
        
        // WiFi settings
        document.getElementById('wifi_ssid').value = config.wifi_ssid || '';
        const wifiPwdField = document.getElementById('wifi_password');
        wifiPwdField.value = '';
        wifiPwdField.placeholder = hasConfig ? '(saved - leave blank to keep)' : '';
        
        // Device settings
        document.getElementById('device_name').value = config.device_name || '';
        document.getElementById('device_name_sanitized').textContent = 
            (config.device_name_sanitized || 'esp32-xxxx') + '.local';
        
        // Fixed IP settings
        document.getElementById('fixed_ip').value = config.fixed_ip || '';
        document.getElementById('subnet_mask').value = config.subnet_mask || '';
        document.getElementById('gateway').value = config.gateway || '';
        document.getElementById('dns1').value = config.dns1 || '';
        document.getElementById('dns2').value = config.dns2 || '';
        
        // Dummy setting
        document.getElementById('dummy_setting').value = config.dummy_setting || '';
        
        // Hide loading overlay (silent load)
        document.getElementById('form-loading-overlay').style.display = 'none';
    } catch (error) {
        // Hide loading overlay even on error so form is usable
        document.getElementById('form-loading-overlay').style.display = 'none';
        showMessage('Error loading configuration: ' + error.message, 'error');
        console.error('Load error:', error);
    }
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
    const config = {
        wifi_ssid: formData.get('wifi_ssid'),
        wifi_password: formData.get('wifi_password'),
        device_name: formData.get('device_name'),
        fixed_ip: formData.get('fixed_ip'),
        subnet_mask: formData.get('subnet_mask'),
        gateway: formData.get('gateway'),
        dns1: formData.get('dns1'),
        dns2: formData.get('dns2'),
        dummy_setting: formData.get('dummy_setting')
    };
    
    // Validate required fields
    if (!config.wifi_ssid || config.wifi_ssid.trim() === '') {
        showMessage('WiFi SSID is required', 'error');
        return;
    }
    
    if (!config.device_name || config.device_name.trim() === '') {
        showMessage('Device name is required', 'error');
        return;
    }
    
    // Validate fixed IP configuration
    if (config.fixed_ip && config.fixed_ip.trim() !== '') {
        if (!config.subnet_mask || config.subnet_mask.trim() === '') {
            showMessage('Subnet mask is required when using fixed IP', 'error');
            return;
        }
        if (!config.gateway || config.gateway.trim() === '') {
            showMessage('Gateway is required when using fixed IP', 'error');
            return;
        }
    }
    
    const currentDeviceName = document.getElementById('device_name').value;
    
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
 * Reboot device without saving
 */
async function rebootDevice() {
    if (!confirm('Reboot the device without saving any changes?')) {
        return;
    }
    
    // Show unified dialog
    showRebootDialog({
        title: 'Device Rebooting',
        message: 'Device is rebooting...',
        context: 'reboot'
    });
    
    try {
        const response = await fetch('/api/reboot', {
            method: 'POST'
        });
        
        if (!response.ok) {
            throw new Error('Failed to reboot device');
        }
    } catch (error) {
        // If reboot request fails (e.g., device already rebooting), that's expected
        if (!error.message.includes('Failed to fetch') && !error.message.includes('NetworkError')) {
            // Only show error for non-network errors
            document.getElementById('reboot-overlay').style.display = 'none';
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
 * Initialize page on DOM ready
 */
document.addEventListener('DOMContentLoaded', () => {
    // Attach event handlers
    document.getElementById('config-form').addEventListener('submit', saveConfig);
    document.getElementById('reboot-btn').addEventListener('click', rebootDevice);
    document.getElementById('reset-btn').addEventListener('click', resetConfig);
    document.getElementById('firmware-file').addEventListener('change', handleFileSelect);
    document.getElementById('upload-btn').addEventListener('click', uploadFirmware);
    document.getElementById('device_name').addEventListener('input', updateSanitizedName);
    
    // Load initial data
    loadMode();
    loadConfig();
    loadVersion();
    
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
        document.getElementById('health-temp').textContent = health.temperature !== null ? 
            `${health.temperature}°C` : 'N/A';
        
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
        document.getElementById('health-compact').style.display = 'none';
        document.getElementById('health-expanded').style.display = 'block';
        updateHealth(); // Immediate update when opened
        // Update every 5 seconds when expanded
        healthUpdateInterval = setInterval(updateHealth, 5000);
    } else {
        document.getElementById('health-compact').style.display = 'flex';
        document.getElementById('health-expanded').style.display = 'none';
        if (healthUpdateInterval) {
            clearInterval(healthUpdateInterval);
            healthUpdateInterval = null;
        }
    }
}

// Initialize health widget
function initHealthWidget() {
    // Click compact view to expand
    document.getElementById('health-compact').addEventListener('click', toggleHealthWidget);
    
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
