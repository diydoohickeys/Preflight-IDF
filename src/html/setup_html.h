#ifndef SETUP_HTML_H
#define SETUP_HTML_H

#ifndef PROGMEM
#define PROGMEM
#endif

inline constexpr char SETUP_HTML[] PROGMEM = R"rawliteral(<!DOCTYPE html>
<html>
<head>
    <meta charset="UTF-8">
    <title>WiFi Setup</title>
    <meta name="viewport" content="width=device-width, initial-scale=1">
    <link rel="stylesheet" href="wifi-setup-style.css">
    <script src="wifi-setup-theme.js"></script>
</head>
<body>
    <main>
        <div class="header">
            <h1>WiFi Setup</h1>
            <button type="button" class="theme-toggle" onclick="toggleTheme()"></button>
        </div>

        <div class="card">
            <div class="scanning" id="scanning">Scanning networks...</div>

            <form action="/save-wifi" method="POST">
                <label for="network_name">Device Name</label>
                <input type="text" id="network_name" name="network_name" placeholder="Enter hostname" required>

                <label for="wifi_network">WiFi Network</label>
                <select id="wifi_network" name="wifi_network" required>
                    <option value="">Loading...</option>
                </select>

                <label for="wifi_password">WiFi Password</label>
                <input type="password" id="wifi_password" name="wifi_password" placeholder="Enter password">
                <p class="hint" id="password_hint" style="display:none">Password required for new network</p>

                <div id="choose_device_password" style="display:none">
                    <label for="device_password">Device Password</label>
                    <input type="password" id="device_password" name="device_password" minlength="8" maxlength="64" autocomplete="new-password" placeholder="8-64 characters">
                    <label for="device_password_confirm">Confirm Device Password</label>
                    <input type="password" id="device_password_confirm" minlength="8" maxlength="64" autocomplete="new-password">
                    <p class="hint">Protects this page, the logs, firmware updates and the device's own controls. Factory reset is the only way to recover a forgotten one.</p>
                </div>

                <button type="submit">Save & Restart</button>
                <button type="button" class="danger" onclick="factoryReset()">Factory Reset</button>
            </form>
        </div>

        <div class="card" id="access_card" style="display:none">
            <form id="change_password">
                <label for="current_password">Current Device Password</label>
                <input type="password" id="current_password" autocomplete="current-password" required>
                <label for="new_password">New Device Password</label>
                <input type="password" id="new_password" minlength="8" maxlength="64" autocomplete="new-password" required>
                <label for="new_password_confirm">Confirm New Password</label>
                <input type="password" id="new_password_confirm" minlength="8" maxlength="64" autocomplete="new-password" required>
                <p class="hint" id="change_message" style="display:none"></p>
                <button type="submit">Change Password</button>
            </form>

            <div class="divider"></div>

            <label for="api_key">API Key</label>
            <input type="text" id="api_key" readonly>
            <p class="hint">For scripts: send <code>Authorization: Bearer &lt;key&gt;</code>. Regenerating stops the old key working.</p>
            <button type="button" class="secondary" onclick="regenerateApiKey()">Regenerate API Key</button>
            <button type="button" class="secondary" onclick="signOut()">Sign Out Everywhere</button>

            <div id="https_section" style="display:none">
                <div class="divider"></div>

                <label>HTTPS</label>
                <p class="hint" id="https_status"></p>
                <button type="button" class="secondary" id="https_toggle" onclick="toggleHttps()"></button>
                <div id="cert_section" style="display:none">
                    <p class="hint" id="cert_status"></p>
                    <p class="hint">Browsers warn about a certificate the device made itself, and load more slowly. To stop that, sign it with your own CA: <code>preflight-certs.ps1 -Device &lt;host&gt;</code> (in the Preflight repo's <code>tools/</code>).</p>
                    <button type="button" class="secondary" id="cert_reset" style="display:none" onclick="resetCertificate()">Use Self-Signed Again</button>
                </div>
            </div>
        </div>

        <nav>
            <a href="/">Back</a>
        </nav>
    </main>

    <script>
        let originalSettings = { hostName: '', wifiSSID: '', hasPassword: false };

        function fetchSettings() {
            return fetch('/get-current-settings')
                .then(r => r.json())
                .catch(() => null);
        }

        // 202 = scanning in the background (async servers can't block): poll, bounded.
        async function fetchNetworks() {
            const deadline = Date.now() + 20000;
            while (true) {
                let res;
                try {
                    res = await fetch('/get-networks');
                } catch (e) {
                    return null;
                }
                if (res.status !== 202) {
                    return res.ok ? res.text() : null;
                }
                if (Date.now() > deadline) return null;
                await new Promise(r => setTimeout(r, 700));
            }
        }

        function splitEscaped(text, delim) {
            const out = [];
            let cur = '';
            for (let i = 0; i < text.length; i++) {
                const c = text[i];
                if (c === '\\' && i + 1 < text.length) {
                    cur += c + text[++i];      // keep the escape for unescapeField
                } else if (c === delim) {
                    out.push(cur);
                    cur = '';
                } else {
                    cur += c;
                }
            }
            out.push(cur);
            return out;
        }

        function unescapeField(text) {
            let out = '';
            for (let i = 0; i < text.length; i++) {
                if (text[i] === '\\' && i + 1 < text.length) out += text[++i];
                else out += text[i];
            }
            return out;
        }

        function populateNetworks(data) {
            const select = document.getElementById('wifi_network');
            select.innerHTML = '';

            if (data === null) {
                select.innerHTML = '<option value="">Error loading</option>';
                return;
            }
            if (!data || !data.trim()) {
                select.innerHTML = '<option value="">No networks found</option>';
                return;
            }

            // "SSID|RSSI|secured,...", backslash-escaped: ',' and '|' are legal in an SSID.
            // A mesh or repeater reports one SSID per access point: list it once, at its strongest.
            const networks = new Map();
            splitEscaped(data, ',').forEach(net => {
                const parts = splitEscaped(net, '|');
                const ssid = unescapeField(parts[0] || '');
                const rssi = parseInt(parts[1], 10);
                const open = parts[2] === '0';
                const known = networks.get(ssid);
                if (ssid && (!known || rssi > known.rssi)) networks.set(ssid, { rssi, open });
            });
            [...networks.entries()]
                .sort((a, b) => b[1].rssi - a[1].rssi)
                .forEach(([ssid, net]) => {
                    const opt = document.createElement('option');
                    opt.value = ssid;
                    opt.dataset.open = net.open ? '1' : '0';
                    opt.textContent = `${ssid} (${net.rssi} dBm${net.open ? ', open' : ''})`;
                    select.appendChild(opt);
                });

            if (originalSettings.wifiSSID && !select.querySelector(`option[value="${CSS.escape(originalSettings.wifiSSID)}"]`)) {
                const opt = document.createElement('option');
                opt.value = originalSettings.wifiSSID;
                opt.textContent = `${originalSettings.wifiSSID} (saved)`;
                select.insertBefore(opt, select.firstChild);
            }

            if (originalSettings.wifiSSID) {
                select.value = originalSettings.wifiSSID;
            }
        }

        function applySettings(settings) {
            if (!settings) return;
            originalSettings.hostName = settings.networkName || '';
            originalSettings.wifiSSID = settings.wifiSSID || '';
            originalSettings.hasPassword = !!settings.hasPassword;
            if (settings.hasSettings) {
                document.getElementById('network_name').value = settings.networkName || '';
            }
            // The first save chooses the device password; after that it is changed on the Access card.
            const choosing = !settings.hasDevicePassword;
            document.getElementById('choose_device_password').style.display = choosing ? 'block' : 'none';
            document.getElementById('device_password').required = choosing;
            document.getElementById('device_password_confirm').required = choosing;
            document.getElementById('access_card').style.display = choosing ? 'none' : 'block';
            return choosing ? null : Promise.allSettled([loadApiKey(), loadCertificate()]);
        }

        let httpsOn = false;

        // Absent (404) when the device doesn't offer HTTPS.
        async function loadCertificate() {
            let cert = null;
            try {
                const res = await fetch('/api/access/certificate');
                if (res.ok) cert = await res.json();
            } catch (e) {}
            if (!cert) return;
            httpsOn = !!cert.https;
            document.getElementById('https_section').style.display = 'block';
            document.getElementById('https_status').textContent = httpsOn
                ? 'On - pages and passwords are encrypted on the network.'
                : 'Off - fine on a home network you trust. Turn it on to encrypt pages and passwords on the network.';
            document.getElementById('https_toggle').textContent = httpsOn ? 'Turn HTTPS Off' : 'Turn HTTPS On';
            document.getElementById('cert_section').style.display = httpsOn ? 'block' : 'none';
            if (!httpsOn) return;
            const status = document.getElementById('cert_status');
            if (!cert.present) {
                status.textContent = 'Certificate: made at the next restart';
                return;
            }
            status.textContent = cert.signedByCa
                ? `Certificate: signed by ${cert.issuer} for ${cert.subject}, valid to ${cert.validTo}`
                : `Certificate: self-signed for ${cert.subject}`;
            document.getElementById('cert_reset').style.display = cert.signedByCa ? 'inline-block' : 'none';
        }

        // The device restarts; the page comes back on the new scheme (port 80 redirects to HTTPS,
        // and nothing answers on 443 once HTTPS is off).
        function reloadAfterRestart(scheme) {
            document.getElementById('https_status').textContent = 'Restarting...';
            setTimeout(() => { location.href = `${scheme}//${location.hostname}/setup`; }, 10000);
        }

        async function toggleHttps() {
            const next = !httpsOn;
            const question = next
                ? 'Turn HTTPS on? The device restarts, and your browser warns about its certificate until you sign it.'
                : 'Turn HTTPS off? The device restarts and serves plain HTTP.';
            if (!confirm(question)) return;
            try {
                const res = await fetch('/api/access/https', { method: 'POST', body: new URLSearchParams({ enabled: next }) });
                if (!res.ok) throw new Error();
            } catch (e) {
                document.getElementById('https_status').textContent = 'Could not change the setting';
                return;
            }
            reloadAfterRestart(next ? 'https:' : 'http:');
        }

        async function resetCertificate() {
            if (!confirm('Go back to a self-signed certificate? The device restarts, and browsers warn again.')) return;
            try { await fetch('/api/access/certificate/reset', { method: 'POST' }); } catch (e) {}
            reloadAfterRestart('https:');
        }

        function showChangeMessage(text) {
            const message = document.getElementById('change_message');
            message.textContent = text;
            message.style.display = text ? 'block' : 'none';
        }

        async function loadApiKey() {
            try {
                const res = await fetch('/api/access');
                if (res.ok) document.getElementById('api_key').value = (await res.json()).apiKey || '';
            } catch (e) {}
        }

        async function regenerateApiKey() {
            if (!confirm('Regenerate the API key? Scripts using the old one stop working.')) return;
            try {
                const res = await fetch('/api/access/api-key', { method: 'POST' });
                if (res.ok) document.getElementById('api_key').value = (await res.json()).apiKey || '';
            } catch (e) {}
        }

        async function signOut() {
            try { await fetch('/logout', { method: 'POST' }); } catch (e) {}
            location.href = '/login';
        }

        document.getElementById('change_password').addEventListener('submit', async (event) => {
            event.preventDefault();
            const next = document.getElementById('new_password').value;
            if (next !== document.getElementById('new_password_confirm').value) {
                showChangeMessage('The new passwords do not match');
                return;
            }
            try {
                const res = await fetch('/api/access/password', {
                    method: 'POST',
                    body: new URLSearchParams({ current: document.getElementById('current_password').value, new: next }),
                });
                const data = await res.json().catch(() => ({}));
                showChangeMessage(res.ok ? 'Password changed - other browsers are signed out' : (data.error || 'Could not change the password'));
                if (res.ok) event.target.reset();
            } catch (e) {
                showChangeMessage('Device unreachable');
            }
        });

        document.querySelector('form[action="/save-wifi"]').addEventListener('submit', (event) => {
            const choosing = document.getElementById('device_password').required;
            if (choosing && document.getElementById('device_password').value !==
                            document.getElementById('device_password_confirm').value) {
                event.preventDefault();
                alert('The device passwords do not match');
            }
        });

        // A blank password on the saved network keeps the stored one (the device never sends it).
        function updatePasswordHint() {
            const select = document.getElementById('wifi_network');
            const selected = select.value;
            const option = select.selectedOptions[0];
            const pw = document.getElementById('wifi_password');
            const hint = document.getElementById('password_hint');
            const saved = !!selected && selected === originalSettings.wifiSSID;
            const open = !!option && option.dataset.open === '1';

            if (saved && originalSettings.hasPassword) {
                pw.placeholder = '********';
                pw.required = false;
                hint.textContent = 'Saved password - leave blank to keep it';
            } else if (open) {
                pw.placeholder = 'Open network - no password';
                pw.required = false;
                hint.textContent = '';
            } else {
                pw.placeholder = 'Enter password';
                pw.required = !!selected && !saved;
                hint.textContent = pw.required ? 'Password required for a new network' : '';
            }
            hint.style.display = hint.textContent ? 'block' : 'none';
        }

        function factoryReset() {
            if (confirm('Reset all settings? Device will restart in AP mode.')) {
                const form = document.createElement('form');
                form.method = 'POST';
                form.action = '/factory-reset';
                const token = document.createElement('input');
                token.type = 'hidden';
                token.name = 'confirm';
                token.value = 'RESET';
                form.appendChild(token);
                document.body.appendChild(form);
                form.submit();
            }
        }

        async function init() {
            // The scan goes last: the radio leaves the channel while it scans, stalling every other
            // request, so the form and the Access card fill first. The saved SSID is known by then,
            // so the list still preselects it.
            await applySettings(await fetchSettings());
            populateNetworks(await fetchNetworks());
            updatePasswordHint();
            document.getElementById('scanning').style.display = 'none';
        }

        document.addEventListener('DOMContentLoaded', init);
        document.getElementById('wifi_network').addEventListener('change', updatePasswordHint);
    </script>
</body>
</html>
)rawliteral";

#endif // SETUP_HTML_H
