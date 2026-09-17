#!/usr/bin/env python3
"""Compare page-scroll clamp and viewport hits with the frozen Python oracle."""
import contextlib
import json
import math
import os
import pathlib
import subprocess
import struct
import sys
import tempfile
import urllib.parse

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import oracle

CSS = ("html {display:block} body {display:block} div {display:block} "
       "section {display:block}")
HTML = ("<div style='height:30px;background-color:red'></div>"
        "<section style='height:30px;background-color:blue'></section>")
CASES = [
    (100.0, 0.0, 150.0, 20.0),
    (30.0, 30.0, 150.0, 20.0),
    (30.0, -10.0, 150.0, 20.0),
    (30.0, 1000.0, 150.0, 0.0),
    (30.0, 30.0, 187.0, 20.0),
]


class EmptyBrowserWindow:
    """Minimum observable browser state used by frozen Chrome.render()."""
    width = 800

    def active_tab_snapshot(self):
        return None

    def tabs_snapshot(self):
        return []

    def is_current_page_bookmarked(self):
        return False

    def active_is_secure(self):
        return False

    def active_url_string(self):
        return None

    def active_can_go_back(self):
        return False

    def active_can_go_forward(self):
        return False


def expected(browser, viewport_height, requested_scroll, x, y):
    nodes = browser.HTMLParser(HTML).parse()
    rules = browser.DEFAULT_STYLE_SHEET.copy()
    rules.extend(browser.CSSParser(CSS).parse())
    rules.sort(key=browser.cascade_priority)
    browser.style(nodes, rules)
    document = browser.DocumentLayout(nodes, 200)
    document.layout()
    display = []
    browser.paint_tree(document, display)
    maximum = max(document.height + 2 * browser.VSTEP - viewport_height, 0)
    scroll = max(0, min(requested_scroll, maximum))
    command = browser.hit_test_paint_commands(display, x, y + scroll)
    target = None
    if command is not None:
        target = oracle.target_path(command.layout_object.node,
                                    oracle.node_paths(nodes))
    return {"scroll": scroll, "max_scroll": maximum, "target": target}


previous = pathlib.Path.cwd()
os.chdir(oracle.REFERENCE)
try:
    with contextlib.redirect_stdout(sys.stderr):
        browser = oracle.load_reference()
        wanted = [expected(browser, *case) for case in CASES]
        chrome = browser.Chrome(EmptyBrowserWindow())
        wanted_viewport_height = math.ceil(browser.HEIGHT - chrome.bottom)
finally:
    os.chdir(previous)

url = "data:text/html," + urllib.parse.quote(HTML, safe="")
for index, (case, expected_result) in enumerate(zip(CASES, wanted)):
    actual = json.loads(subprocess.check_output(
        [sys.argv[1], url, *(str(value) for value in case)], text=True))
    assert actual["target"] == expected_result["target"], \
        (index, expected_result, actual)
    assert math.isclose(actual["scroll"], expected_result["scroll"],
                        rel_tol=0.0, abs_tol=0.0001), \
        (index, expected_result, actual)
    assert math.isclose(actual["max_scroll"], expected_result["max_scroll"],
                        rel_tol=0.0, abs_tol=0.0001), \
        (index, expected_result, actual)

with tempfile.TemporaryDirectory(prefix="tai-ci-page-scroll-") as directory:
    output = pathlib.Path(directory) / "viewport.png"
    subprocess.run([sys.argv[2], "--screenshot", str(output), url],
                   check=True)
    header = output.read_bytes()[:24]
    assert header[:8] == b"\x89PNG\r\n\x1a\n"
    actual_viewport_height = struct.unpack(">I", header[20:24])[0]
    assert actual_viewport_height == wanted_viewport_height, \
        (wanted_viewport_height, actual_viewport_height)

print(f"Page scroll differential: {len(CASES)} cases and viewport height passed")
