#!/usr/bin/env python3
"""Convert HTML source files to C++ header files with PROGMEM strings."""

from pathlib import Path

SCRIPT_DIR = Path(__file__).parent
HTML_SOURCE_DIR = SCRIPT_DIR / "html_source"
OUTPUT_DIR = SCRIPT_DIR.parent / "src" / "html"

FILE_MAPPINGS = {
    "setup.html": ("setup_html.h", "SETUP_HTML", "SETUP_HTML_H"),
    "wifi-saved.html": ("wifi_saved_html.h", "WIFI_SAVED_HTML", "WIFI_SAVED_HTML_H"),
    "factory-reset.html": ("factory_reset_html.h", "FACTORY_RESET_HTML", "FACTORY_RESET_HTML_H"),
    "factory-reset-confirm.html": ("factory_reset_confirm_html.h", "FACTORY_RESET_CONFIRM_HTML", "FACTORY_RESET_CONFIRM_HTML_H"),
    "logs.html": ("logs_html.h", "LOGS_HTML", "LOGS_HTML_H"),
    "ota.html": ("ota_html.h", "OTA_HTML", "OTA_HTML_H"),
    "style.css": ("style_css.h", "STYLE_CSS", "STYLE_CSS_H"),
    "theme.js": ("theme_js.h", "THEME_JS", "THEME_JS_H"),
}

# `inline constexpr`, not `const`: a namespace-scope const has internal linkage, so each including TU gets its own copy in flash.
def convert_file(source_name, output_name, var_name, guard_name):
    source_path = HTML_SOURCE_DIR / source_name
    output_path = OUTPUT_DIR / output_name

    if not source_path.exists():
        print(f"  Skipping {source_name} (not found)")
        return False

    content = source_path.read_text(encoding='utf-8')

    # PROGMEM is Arduino-only; the empty fallback lets the generated header build under both frameworks.
    header = f'''#ifndef {guard_name}
#define {guard_name}

#ifndef PROGMEM
#define PROGMEM
#endif

inline constexpr char {var_name}[] PROGMEM = R"rawliteral({content})rawliteral";

#endif // {guard_name}
'''

    OUTPUT_DIR.mkdir(parents=True, exist_ok=True)
    output_path.write_text(header, encoding='utf-8')
    print(f"  {source_name} -> {output_name}")
    return True

def main():
    print("Converting HTML sources to C++ headers...")
    print(f"  Source: {HTML_SOURCE_DIR}")
    print(f"  Output: {OUTPUT_DIR}")
    print()

    count = 0
    for source, (output, var, guard) in FILE_MAPPINGS.items():
        if convert_file(source, output, var, guard):
            count += 1

    print(f"\nConverted {count} files.")

if __name__ == "__main__":
    main()
