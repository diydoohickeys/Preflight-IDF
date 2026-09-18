#ifndef OTA_HTML_H
#define OTA_HTML_H

#ifndef PROGMEM
#define PROGMEM
#endif

inline constexpr char OTA_HTML[] PROGMEM = R"rawliteral(<!DOCTYPE html>
<html>
<head>
    <meta charset="UTF-8">
    <title>OTA Update</title>
    <meta name="viewport" content="width=device-width, initial-scale=1">
    <link rel="stylesheet" href="wifi-setup-style.css">
    <script src="wifi-setup-theme.js"></script>
    <!--OTA_CAPS-->
    <style>
        .upload-section { margin-bottom: 1.5rem; }
        .upload-section:last-of-type { margin-bottom: 0; }
        .upload-section h2 {
            color: var(--accent);
            font-size: 0.9rem;
            font-weight: 500;
            text-transform: uppercase;
            letter-spacing: 0.5px;
            margin-bottom: 0.75rem;
        }
        input[type="file"] {
            width: 100%;
            padding: 0;
            background: var(--surface-sunken);
            border: 1px solid var(--surface-border);
            border-radius: var(--radius-button);
            color: var(--text-dim);
            font-size: 0.9rem;
            font-family: inherit;
            margin-bottom: 0.75rem;
            cursor: pointer;
            overflow: hidden;
            transition: border-color 0.18s, box-shadow 0.18s, background 0.18s;
        }
        input[type="file"]:hover {
            border-color: var(--accent);
            background: var(--surface-elevated);
        }
        input[type="file"]:focus {
            outline: none;
            border-color: var(--accent);
            box-shadow: 0 0 0 3px var(--accent-glow);
        }
        input[type="file"]::file-selector-button {
            background: linear-gradient(180deg, var(--accent), var(--accent-dark));
            color: white;
            border: 0;
            border-right: 1px solid rgba(255, 255, 255, 0.12);
            padding: 0.75rem 1.1rem;
            margin: 0 0.85rem 0 0;
            font-family: inherit;
            font-size: 0.9rem;
            font-weight: 500;
            cursor: pointer;
            transition: filter 0.18s, background 0.18s;
            box-shadow: inset 0 1px 0 rgba(255, 255, 255, 0.18);
        }
        input[type="file"]::file-selector-button:hover {
            filter: brightness(1.1);
        }
        .progress-container { display: none; margin-top: 1rem; }
        .progress-bar {
            width: 100%;
            height: 8px;
            background: var(--border);
            border-radius: 4px;
            overflow: hidden;
        }
        .progress-fill {
            height: 100%;
            background: var(--accent);
            width: 0%;
            transition: width 0.3s;
        }
        .progress-text {
            margin-top: 0.5rem;
            text-align: center;
            font-size: 0.85rem;
            color: var(--text-dim);
        }
        .status {
            padding: 0.75rem;
            border-radius: 8px;
            margin-top: 1rem;
            display: none;
            font-size: 0.85rem;
        }
        .status.success { background: #1a4d1a; color: #5fd35f; border: 1px solid #2a6d2a; }
        .status.error { background: #4d1a1a; color: #ff6b6b; border: 1px solid #6d2a2a; }
        .divider { border-top: 1px solid var(--border); margin: 1.5rem 0; padding-top: 1.5rem; }
    </style>
</head>
<body>
    <main>
        <div class="header">
            <h1>OTA Update</h1>
            <button type="button" class="theme-toggle" onclick="toggleTheme()"></button>
        </div>

        <div class="card">
            <div class="upload-section">
                <h2>Firmware Update</h2>
                <input type="file" id="firmwareFile" accept=".bin">
                <button onclick="uploadFirmware()">Upload Firmware</button>
            </div>

            <div class="divider" id="fsDivider" style="display:none"></div>

            <div class="upload-section" id="fsSection" style="display:none">
                <h2>Filesystem Update</h2>
                <input type="file" id="filesystemFile" accept=".bin">
                <button onclick="uploadFilesystem()">Upload Filesystem</button>
            </div>
            <script>
                // Hidden unless the server enables filesystem OTA; inline so it runs during parse.
                if (window.OTA_FS_ENABLED) {
                    document.getElementById('fsDivider').style.display = '';
                    document.getElementById('fsSection').style.display = '';
                }
            </script>

            <div class="progress-container" id="progressContainer">
                <div class="progress-bar">
                    <div class="progress-fill" id="progressFill"></div>
                </div>
                <div class="progress-text" id="progressText">0%</div>
            </div>

            <div class="status" id="status"></div>
        </div>

        <nav>
            <a href="/">Back</a>
        </nav>
    </main>

    <script>
        function uploadFirmware() { uploadFile('firmwareFile', 'firmware'); }
        function uploadFilesystem() { uploadFile('filesystemFile', 'filesystem'); }

        function uploadFile(inputId, type) {
            const fileInput = document.getElementById(inputId);
            const file = fileInput.files[0];
            if (!file) {
                showStatus('Please select a file', 'error');
                return;
            }

            const progressContainer = document.getElementById('progressContainer');
            const progressFill = document.getElementById('progressFill');
            const progressText = document.getElementById('progressText');
            const statusDiv = document.getElementById('status');
            const buttons = document.querySelectorAll('button:not(.theme-toggle)');

            buttons.forEach(btn => btn.disabled = true);
            progressContainer.style.display = 'block';
            statusDiv.style.display = 'none';
            progressFill.style.width = '0%';
            progressText.textContent = '0%';

            const xhr = new XMLHttpRequest();

            xhr.upload.addEventListener('progress', (e) => {
                if (e.lengthComputable) {
                    const percent = Math.round((e.loaded / e.total) * 100);
                    progressFill.style.width = percent + '%';
                    progressText.textContent = percent + '%';
                }
            });

            xhr.addEventListener('load', () => {
                buttons.forEach(btn => btn.disabled = false);
                if (xhr.status === 200) {
                    showStatus('Update successful! Device will reboot...', 'success');
                    setTimeout(() => location.reload(), 5000);
                } else {
                    showStatus('Update failed: ' + xhr.responseText, 'error');
                }
            });

            xhr.addEventListener('error', () => {
                buttons.forEach(btn => btn.disabled = false);
                showStatus('Upload failed - network error', 'error');
            });

            const formData = new FormData();
            formData.append('file', file);
            xhr.open('POST', '/ota/upload?type=' + type);
            xhr.send(formData);
        }

        function showStatus(message, type) {
            const statusDiv = document.getElementById('status');
            statusDiv.textContent = message;
            statusDiv.className = 'status ' + type;
            statusDiv.style.display = 'block';
        }
    </script>
</body>
</html>
)rawliteral";

#endif // OTA_HTML_H
