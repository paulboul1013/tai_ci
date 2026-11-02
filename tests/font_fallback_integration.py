#!/usr/bin/env python3
"""Check native font fallback (D-F1) and CJK line breaking (D-F2).

Runs `tai-browser --headless` on small pages and reads the layout and display
JSON. Assertions name no font: a fallback is right when fontconfig says the
family it drew with covers the character. Exits 77 (CTest SKIP) when no
installed font covers CJK.

    python3 tests/font_fallback_integration.py build/tai-browser
"""

import json
import pathlib
import subprocess
import sys
import tempfile

NO_LINE_START = "，。、！？）」』：；!),.:;?"
NO_LINE_END = "「『（("


def covers(family, char):
    """True when fontconfig resolves family to a font file that has char;
    an empty family asks whether any installed font has char."""
    pattern = ":charset={:x}".format(ord(char))
    if family:
        path = subprocess.check_output(
            ["fc-match", "-f", "%{file}", family], text=True)
        pattern = ":file={}{}".format(path, pattern)
    found = subprocess.check_output(["fc-list", pattern, "family"], text=True)
    return bool(found.strip())


def load(browser, tmp, body):
    path = pathlib.Path(tmp) / "page.html"
    path.write_text('<html><head><meta charset="utf-8"></head><body>{}'
                    "</body></html>".format(body), encoding="utf-8")
    return json.loads(subprocess.check_output(
        [browser, "--headless", path.as_uri()]))


def lines(node, out):
    if node["kind"] == "LineLayout":
        out.append([child for child in node["children"]
                    if child["kind"] == "TextLayout"])
    for child in node["children"]:
        lines(child, out)
    return out


def texts(page):
    return [c for c in page["display"] if c["kind"] == "text"]


def check_pieces(browser, tmp):
    page = load(browser, tmp, '<p style="font-family:serif">Tai中文! end</p>')
    [line] = lines(page["layout"], [])
    assert [b["word"] for b in line] == ["Tai", "中", "文", "!", "end"], line
    for left, right in zip(line[:3], line[1:4]):
        assert abs(left["x"] + left["width"] - right["x"]) < 1e-6, (left, right)
    assert line[4]["x"] > line[3]["x"] + line[3]["width"] + 1, line
    commands = {c["text"]: c for c in texts(page)}
    primary = commands["Tai"]["font_family"]
    assert commands["!"]["font_family"] == primary, commands
    for char in "中文":
        family = commands[char]["font_family"]
        assert family != primary and covers(family, char), commands[char]
        assert commands[char]["width"] > 0, commands[char]


def check_wrap(browser, tmp):
    text = "瀏覽器會把文字排成一行一行，遇到視窗邊緣就換行。「標點」不會出現在行首。"
    page = load(browser, tmp, '<div style="width:100px">{}</div>'.format(text))
    found = lines(page["layout"], [])
    assert len(found) > 3, [[b["word"] for b in line] for line in found]
    assert "".join(b["word"] for line in found for b in line) == text
    for line in found:
        words = "".join(b["word"] for b in line)
        left = line[0]["x"]
        right = line[-1]["x"] + line[-1]["width"]
        assert right - left <= 100 + 1e-6, (words, right - left)
        assert words[0] not in NO_LINE_START, words
        assert words[-1] not in NO_LINE_END, words


def check_kinsoku(browser, tmp):
    # Six 16px ideographs fill 100px, so a plain per-character break would
    # start line two with "，" and end line one with "「".
    for text, expected in (("一二三四五六，七", ["一二三四五", "六，七"]),
                           ("一二三四五「六」", ["一二三四五", "「六」"])):
        page = load(browser, tmp, '<div style="width:100px">{}</div>'.format(text))
        found = ["".join(b["word"] for b in line)
                 for line in lines(page["layout"], [])]
        assert found == expected, (text, found)


def check_marks(browser, tmp):
    # A combining mark or variation selector stays with the character
    # before it: no piece or line starts with one.
    for text, expected in (("中\u0301文", ["中\u0301", "文"]),
                           ("一\ufe00二", ["一\ufe00", "二"])):
        page = load(browser, tmp, "<p>{}</p>".format(text))
        [line] = lines(page["layout"], [])
        assert [b["word"] for b in line] == expected, (text, line)


def check_hebrew(browser, tmp):
    page = load(browser, tmp, '<p>שלום</p>')
    [command] = texts(page)
    assert covers(command["font_family"], "ש"), command


def check_missing(browser, tmp):
    # No installed font has U+E000, so the primary font draws .notdef as before.
    page = load(browser, tmp, '<p style="font-family:serif">ab</p>')
    assert [c["text"] for c in texts(page)] == ["ab"], texts(page)


def check_pre(browser, tmp):
    page = load(browser, tmp, '<div style="width:40px"><pre>中文中文中文</pre></div>')
    found = [line for line in lines(page["layout"], []) if line]
    assert len(found) == 1, found
    assert all(covers(c["font_family"], c["text"][0]) for c in texts(page))


def main():
    if not covers("", "中"):
        print("SKIP: no installed font covers CJK")
        return 77
    with tempfile.TemporaryDirectory() as tmp:
        for check in (check_pieces, check_wrap, check_kinsoku, check_marks,
                      check_hebrew, check_missing, check_pre):
            check(sys.argv[1], tmp)
    print("font fallback and CJK line breaking checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
