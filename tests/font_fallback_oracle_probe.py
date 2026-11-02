#!/usr/bin/env python3
"""Freeze what the Python oracle does with characters its font lacks.

get_font (browser.py:1516-1558) picks one skia.Typeface per family, and
DrawText calls canvas.drawString without a shaper, so a character the
typeface lacks maps to glyph 0 and nothing falls back. BlockLayout.recurse
splits text with str.split() (browser.py:4034), so a CJK run with no spaces
is one word that never wraps. Native differs on purpose (D-F1, D-F2 in
PORTING_PLAN.md); this probe keeps the oracle side of that record honest.

    python3 tests/font_fallback_oracle_probe.py            # print the result
    python3 tests/font_fallback_oracle_probe.py --check    # compare with fixture
"""

import contextlib
import io
import json
import os
import pathlib
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parents[1]
FIXTURE = ROOT / "tests" / "fixtures" / "font_fallback_oracle.json"
sys.path.insert(0, str(ROOT / "tests"))
import oracle  # noqa: E402

TEXTS = ["中文", "Tai中文!", "שלום"]
WRAP_HTML = '<div style="width:100px">瀏覽器會把文字排成一行一行，遇到視窗邊緣就換行。</div>'


def words(node, out):
    if node["kind"] == "TextLayout":
        out.append(node["word"])
    for child in node["children"]:
        words(child, out)
    return out


def lines(node, out):
    if node["kind"] == "LineLayout":
        out.append(words(node, []))
    for child in node["children"]:
        lines(child, out)
    return out


def probe():
    cwd = os.getcwd()
    os.chdir(oracle.REFERENCE)  # browser.py opens browser.css at import
    try:
        with contextlib.redirect_stdout(io.StringIO()):
            reference = oracle.load_reference()
    finally:
        os.chdir(cwd)
    fonts = {}
    for family in ("Times", "sans-serif"):
        font = reference.get_font(16, "normal", "normal", family)
        fonts[family] = {
            text: [glyph == 0 for glyph in font.textToGlyphs(text)]
            for text in TEXTS}
    layout = json.loads(subprocess.check_output(
        [sys.executable, str(ROOT / "tests" / "oracle.py"), "layout",
         WRAP_HTML, "--width", "800"]))["layout"]
    return {"missing_glyph": fonts, "wrap_lines": lines(layout, [])}


def main():
    result = probe()
    if "--check" in sys.argv:
        expected = json.loads(FIXTURE.read_text(encoding="utf-8"))
        if result != expected:
            raise SystemExit("font fallback oracle changed:\n{}".format(
                json.dumps(result, ensure_ascii=False, indent=1)))
        print("font fallback oracle matches the fixture")
    else:
        print(json.dumps(result, ensure_ascii=False, indent=1))


if __name__ == "__main__":
    main()
