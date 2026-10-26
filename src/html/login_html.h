#ifndef LOGIN_HTML_H
#define LOGIN_HTML_H

#ifndef PROGMEM
#define PROGMEM
#endif

inline constexpr char LOGIN_HTML[] PROGMEM = R"rawliteral(<!DOCTYPE html>
<html>
<head>
    <meta charset="UTF-8">
    <title>Sign In</title>
    <meta name="viewport" content="width=device-width, initial-scale=1">
    <link rel="stylesheet" href="wifi-setup-style.css">
    <script src="wifi-setup-theme.js"></script>
</head>
<body>
    <main>
        <div class="header">
            <h1>Sign In</h1>
            <button type="button" class="theme-toggle" onclick="toggleTheme()"></button>
        </div>

        <div class="card">
            <form id="login">
                <label for="password">Device password</label>
                <input type="password" id="password" name="password" autocomplete="current-password" required autofocus>
                <p class="hint" id="message" style="display:none"></p>
                <button type="submit" id="submit">Sign in</button>
            </form>
        </div>
    </main>

    <script>
        // Only a same-site path: "next" comes from the URL, so anything else is an open redirect.
        function nextPath() {
            const next = new URLSearchParams(location.search).get('next') || '/';
            return next.startsWith('/') && !next.startsWith('//') && !next.startsWith('/\\') ? next : '/';
        }

        function show(text) {
            const message = document.getElementById('message');
            message.textContent = text;
            message.style.display = text ? 'block' : 'none';
        }

        document.getElementById('login').addEventListener('submit', async (event) => {
            event.preventDefault();
            const button = document.getElementById('submit');
            button.disabled = true;
            show('');
            try {
                const res = await fetch('/login', {
                    method: 'POST',
                    body: new URLSearchParams({ password: document.getElementById('password').value }),
                });
                if (res.ok) {
                    location.replace(nextPath());
                    return;
                }
                const data = await res.json().catch(() => ({}));
                show(res.status === 429
                    ? `Too many attempts - try again in ${data.retryAfter || 30} s`
                    : (data.error || 'Sign in failed'));
            } catch (e) {
                show('Device unreachable');
            }
            button.disabled = false;
        });
    </script>
</body>
</html>
)rawliteral";

#endif // LOGIN_HTML_H
