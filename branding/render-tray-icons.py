#!/usr/bin/env python3
"""Compose tray SVGs from the supplied logo and small vector status badges."""
import base64
from pathlib import Path


theme = Path(__file__).resolve().parent / "theme"
# Embed the existing logo unchanged so SVG loading never depends on external paths.
logo = base64.b64encode((theme / "colored/128-udrive-icon.png").read_bytes()).decode("ascii")
states = {
    "ok": ("#159765", '<path d="m21.5 25 2.3 2.3 4.6-5.1"/>'),
    "sync": ("#007F9C", '<path d="M21.5 24a3.5 3.5 0 0 1 5.6-1.7M27.6 21.2v2.7h-2.7M28.5 26a3.5 3.5 0 0 1-5.6 1.7M22.4 28.8v-2.7h2.7"/>'),
    "error": ("#D94152", '<path d="M25 21.5v4"/><circle cx="25" cy="28" r=".75" fill="{ink}" stroke="none"/>'),
    "offline": ("#66716B", '<path d="M21.6 25h6.8"/>'),
    "pause": ("#B66B0A", '<path d="M23.3 22v6M26.7 22v6"/>'),
    "information": ("#277EB6", '<circle cx="25" cy="22" r=".75" fill="{ink}" stroke="none"/><path d="M25 24.5v4"/>'),
}

# A compact outline of the logo's layered paper and wrapping folder, for template
# mode. Its interior stays transparent; macOS uses alpha rather than RGB colors.
outline = '''<g fill="none" stroke="{ink}" stroke-width="1.75" stroke-linecap="round" stroke-linejoin="round" clip-path="url(#logo-clip)">
  <path d="M6 12V6q0-1.8 1.8-2.3l6.6-1.8q1.7-.4 2.6 1l1.4 1.6q.7.8 1.8.5l5.1-1.3Q28 3 28 6v8"/>
  <path d="M10 17v-7l10.3-3q2.7-.8 2.7 2v2M13 19v-7l10.4-3q2.6-.7 2.6 1.9v3"/>
  <path d="M3 11.5v12q0 3.6 3.7 4.7l9.2 2.1q4.8.9 8.6-2.5Q28 25.2 28 21V12q0-3.1-2.6-2.1l-2.1.7Q21 11.3 21 14v8q0 4.3-3.6 5.1Q14 25.5 14 22v-3.7q0-2.5-2.4-3.6L3 11.5z"/>
</g>'''

for flavor in ("colored", "dark", "black", "white"):
    monochrome = flavor in ("black", "white")
    ink = "#FFFFFF" if flavor == "white" else "#253047"
    for state, (badge_color, glyph) in states.items():
        if monochrome:
            mark = '''<defs><clipPath id="logo-clip"><path d="M0 0h32v17h-7a8 8 0 0 0-8 8a8 8 0 0 0 8 8H0z"/></clipPath></defs>''' + outline.format(ink=ink)
            badge = f'<circle cx="25" cy="25" r="6" fill="none" stroke="{ink}" stroke-width="1.6"/>'
            glyph_ink = ink
        else:
            mark = f'<image x="0" y="0" width="30" height="30" xlink:href="data:image/png;base64,{logo}"/>'
            rim = "#292929" if flavor == "dark" else "#FFFFFF"
            badge = f'<circle cx="25" cy="25" r="6.2" fill="{badge_color}" stroke="{rim}" stroke-width="1.6"/>'
            glyph_ink = "#FFFFFF"
        content = (f'<svg xmlns="http://www.w3.org/2000/svg" xmlns:xlink="http://www.w3.org/1999/xlink" width="32" height="32" viewBox="0 0 32 32">\n'
            f'{mark}\n{badge}\n<g fill="none" stroke="{glyph_ink}" stroke-width="1.65" stroke-linecap="round" stroke-linejoin="round">'
            f'{glyph.format(ink=glyph_ink)}</g>\n</svg>\n')
        (theme / flavor / f"state-{state}.svg").write_text(content)
print("Rendered 24 logo-based tray SVGs (color and template modes).")
