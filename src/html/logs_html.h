#ifndef LOGS_HTML_H
#define LOGS_HTML_H

#ifndef PROGMEM
#define PROGMEM
#endif

inline constexpr char LOGS_HTML[] PROGMEM = R"rawliteral(<!DOCTYPE html>
<html>
<head>
    <meta charset="UTF-8">
    <title>System Logs</title>
    <meta name="viewport" content="width=device-width, initial-scale=1">
    <link rel="stylesheet" href="wifi-setup-style.css">
    <script src="wifi-setup-theme.js"></script>
    <style>
        .log-container {
            background: var(--bg);
            border: 1px solid var(--border);
            border-radius: 8px;
            padding: 0.75rem;
            max-height: 60vh;
            overflow-y: auto;
            font-family: 'Courier New', monospace;
            font-size: 0.8rem;
        }
        .log-entry {
            padding: 0.5rem;
            border-bottom: 1px solid var(--border);
            display: flex;
            gap: 0.75rem;
        }
        .log-entry:last-child { border-bottom: none; }
        .timestamp { color: var(--text-dim); min-width: 70px; font-size: 0.7rem; }
        .level { min-width: 50px; font-weight: bold; font-size: 0.7rem; }
        .message { flex: 1; word-break: break-word; color: var(--text); }
        .top-bar {
            display: flex;
            justify-content: space-between;
            align-items: center;
            margin-bottom: 1rem;
            gap: 0.5rem;
            flex-wrap: wrap;
        }
        .controls { display: flex; gap: 0.5rem; }
        .controls button {
            padding: 0.5rem 0.75rem;
            margin: 0;
            width: auto;
            font-size: 0.8rem;
        }
        .stats { color: var(--text-dim); font-size: 0.85rem; }
        .stats strong { color: var(--accent); }
    </style>
</head>
<body>
    <main>
        <div class="header">
            <h1>System Logs</h1>
            <button type="button" class="theme-toggle" onclick="toggleTheme()"></button>
        </div>

        <div class="card">
            <div class="top-bar">
                <div class="controls">
                    <button onclick="location.reload()">Refresh</button>
                    <button onclick="clearLogs()">Clear</button>
                    <button id="copyBtn" onclick="copyLogs()">Copy all</button>
                    <button onclick="downloadLogs()">Download</button>
                </div>
                <div class="stats">Total: <strong id="count">%LOG_COUNT%</strong></div>
            </div>
            <div class="log-container" id="logs">
%LOG_ENTRIES%
            </div>
        </div>

        <nav>
            <a href="/">Back</a>
        </nav>
    </main>

    <script>
        function clearLogs() {
            if (confirm('Clear all logs?')) {
                fetch('/api/logs/clear', {method: 'POST'})
                    .then(() => location.reload());
            }
        }

        function downloadLogs() {
            fetch('/api/logs')
                .then(r => r.blob())
                .then(blob => {
                    const url = URL.createObjectURL(blob);
                    const a = document.createElement('a');
                    a.href = url;
                    a.download = 'logs_' + Date.now() + '.json';
                    a.click();
                    URL.revokeObjectURL(url);
                });
        }

        function copyLogs() {
            const entries = document.querySelectorAll('#logs .log-entry');
            const lines = [];
            entries.forEach(e => {
                const ts = e.querySelector('.timestamp');
                const lv = e.querySelector('.level');
                const msg = e.querySelector('.message');
                lines.push(
                    (ts ? ts.textContent : '') + '\t' +
                    (lv ? lv.textContent : '') + '\t' +
                    (msg ? msg.textContent : '')
                );
            });
            const text = lines.join('\n');
            const btn = document.getElementById('copyBtn');
            const orig = btn.textContent;
            const flash = (label) => {
                btn.textContent = label;
                setTimeout(() => { btn.textContent = orig; }, 1500);
            };
            // navigator.clipboard needs a secure context; the portal is plain HTTP.
            if (navigator.clipboard && window.isSecureContext) {
                navigator.clipboard.writeText(text).then(() => flash('Copied!'),
                                                          () => fallback());
            } else {
                fallback();
            }
            function fallback() {
                const ta = document.createElement('textarea');
                ta.value = text;
                ta.style.position = 'fixed';
                ta.style.left = '-9999px';
                document.body.appendChild(ta);
                ta.select();
                let ok = false;
                try { ok = document.execCommand('copy'); } catch (_) {}
                document.body.removeChild(ta);
                flash(ok ? 'Copied!' : 'Copy failed');
            }
        }

        // /ws is consumer-provided; the flag below is Logger's enableWebSocket.
        // Never spell a substitution marker in a comment - it gets substituted too.
        if (%LOG_WS%) {
        const ws = new WebSocket('ws://' + location.hostname + '/ws');
        ws.onmessage = function(e) {
            try {
                const data = JSON.parse(e.data);
                if (data.type === 'log') {
                    const logs = document.getElementById('logs');
                    const entry = document.createElement('div');
                    entry.className = 'log-entry';
                    entry.innerHTML = '<span class="timestamp">' + data.timestamp + 'ms</span>'
                        + '<span class="level" style="color:' + data.color + '">' + data.level + '</span>'
                        + '<span class="message">' + escapeHtml(data.message) + '</span>';
                    logs.appendChild(entry);
                    logs.scrollTop = logs.scrollHeight;
                    document.getElementById('count').textContent =
                        parseInt(document.getElementById('count').textContent) + 1;
                }
            } catch(e) {}
        };
        }

        function escapeHtml(text) {
            const div = document.createElement('div');
            div.textContent = text;
            return div.innerHTML;
        }

        window.addEventListener('DOMContentLoaded', function() {
            const logs = document.getElementById('logs');
            logs.scrollTop = logs.scrollHeight;
        });
    </script>
</body>
</html>
)rawliteral";

#endif // LOGS_HTML_H
