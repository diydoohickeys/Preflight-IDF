#ifndef FACTORY_RESET_HTML_H
#define FACTORY_RESET_HTML_H

#ifndef PROGMEM
#define PROGMEM
#endif

inline constexpr char FACTORY_RESET_HTML[] PROGMEM = R"rawliteral(<!DOCTYPE html>
<html>
<head>
    <meta charset="UTF-8">
    <title>Factory Reset</title>
    <meta name="viewport" content="width=device-width, initial-scale=1">
    <link rel="stylesheet" href="wifi-setup-style.css">
    <meta http-equiv="refresh" content="3">
</head>
<body class="status-page danger-page">
    <main>
        <h1>Factory Reset Complete</h1>

        <div class="card">
            <p>All settings have been cleared.</p>
            <p>Restarting in AP mode...</p>
        </div>
    </main>
</body>
</html>
)rawliteral";

#endif // FACTORY_RESET_HTML_H
