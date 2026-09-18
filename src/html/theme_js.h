#ifndef THEME_JS_H
#define THEME_JS_H

#ifndef PROGMEM
#define PROGMEM
#endif

inline constexpr char THEME_JS[] PROGMEM = R"rawliteral(// Inline SVG: no CDN is reachable in AP / captive-portal mode.
var SUN_SVG  = '<svg width="18" height="18" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><circle cx="12" cy="12" r="4"/><line x1="12" y1="2" x2="12" y2="4"/><line x1="12" y1="20" x2="12" y2="22"/><line x1="4.93" y1="4.93" x2="6.34" y2="6.34"/><line x1="17.66" y1="17.66" x2="19.07" y2="19.07"/><line x1="2" y1="12" x2="4" y2="12"/><line x1="20" y1="12" x2="22" y2="12"/><line x1="4.93" y1="19.07" x2="6.34" y2="17.66"/><line x1="17.66" y1="6.34" x2="19.07" y2="4.93"/></svg>';
var MOON_SVG = '<svg width="18" height="18" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M21 12.79A9 9 0 1 1 11.21 3 7 7 0 0 0 21 12.79z"/></svg>';

function toggleTheme() {
    var body = document.body;
    var toggle = document.querySelector('.theme-toggle');
    body.classList.toggle('light');
    var isLight = body.classList.contains('light');
    if (toggle) toggle.innerHTML = isLight ? MOON_SVG : SUN_SVG;
    localStorage.setItem('theme', isLight ? 'light' : 'dark');
}

document.addEventListener('DOMContentLoaded', function() {
    var saved = localStorage.getItem('theme');
    var toggle = document.querySelector('.theme-toggle');
    if (saved === 'light') {
        document.body.classList.add('light');
        if (toggle) toggle.innerHTML = MOON_SVG;
    } else {
        if (toggle) toggle.innerHTML = SUN_SVG;
    }
});
)rawliteral";

#endif // THEME_JS_H
