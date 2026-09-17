#!/usr/bin/env python3
"""Compare native display hit testing with the frozen Python oracle."""
import contextlib
import json
import math
import os
import pathlib
import subprocess
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import oracle

CSS = "html {display:block} body {display:block} main {display:block} div {display:block}"
CASES = [
    ("<div style='background-color:red'>hello</div>", 14, 22),
    ("<main style='height:20px;overflow:scroll'><div style='height:40px;background-color:red'>x</div></main>", 20, 38),
    ("<main data-scroll='20' style='height:20px;overflow:scroll'><div style='height:20px;background-color:red'>a</div><div style='height:20px;background-color:blue'>b</div></main>", 150, 20),
    ("<main data-scroll='10' style='height:25px;overflow:scroll'><div data-scroll='10' style='height:30px;overflow:scroll'><div style='height:20px;background-color:red'>a</div><div style='height:20px;background-color:blue'>b</div></div></main>", 150, 20),
    ("<main style='height:20px;overflow:scroll'></main>", 20, 20),
    ("<div style='height:20px;background-color:red'></div>", 13, 18),
    ("<div style='height:20px;background-color:red'></div>", 187, 18),
    ("<div style='height:20px;background-color:red'></div>", 20, 38),
    ("<div style='height:20px;border-radius:10px;background-color:red'></div>", 13, 18),
    ("<div style='height:20px;border-radius:10px;background-color:red'></div>", 100, 18),
    ("<div style='height:20px;border-radius:999px;background-color:red'></div>", 13, 18),
    ("<div style='height:20px;border-radius:999px;background-color:red'></div>", 100, 18),
    ("<div style='height:20px;border-radius:10.5px;background-color:red'></div>", 13, 18),
    ("<div style='height:20px;border-radius:1e999px;background-color:red'></div>", 13, 18),
    ("<div style='height:20px;border-radius:10px'>x</div>", 13, 21),
    ("<div style='height:20px;border-radius:10px'>x</div>", 17, 25),
    ("<main style='height:20px;overflow:clip;border-radius:10px'><div style='height:40px;background-color:blue'></div></main>", 13, 18),
    ("<main style='height:20px;overflow:clip;border-radius:10px'><div style='height:40px;background-color:blue'></div></main>", 100, 18),
]


def expected(browser, html, x, y):
    nodes = browser.HTMLParser(html).parse()
    rules = browser.DEFAULT_STYLE_SHEET.copy()
    rules.extend(browser.CSSParser(CSS).parse())
    rules.sort(key=browser.cascade_priority)
    browser.style(nodes, rules)
    for node in browser.tree_to_list(nodes, []):
        value = getattr(node, "attributes", {}).get("data-scroll")
        if value is not None:
            node.scroll_y = float(value)
    document = browser.DocumentLayout(nodes, 200)
    document.layout()
    display = []
    browser.paint_tree(document, display)
    command = browser.hit_test_paint_commands(display, x, y)
    if command is None:
        return None
    paths = oracle.node_paths(nodes)
    return {"target": oracle.target_path(command.layout_object.node, paths),
            "rect": list(command.rect)}


previous = pathlib.Path.cwd()
os.chdir(oracle.REFERENCE)
try:
    with contextlib.redirect_stdout(sys.stderr):
        browser = oracle.load_reference()
        results = [expected(browser, *case) for case in CASES]
finally:
    os.chdir(previous)

for index, ((html, x, y), wanted) in enumerate(zip(CASES, results)):
    actual = json.loads(subprocess.check_output(
        [sys.argv[1], html, str(x), str(y)], text=True))
    assert (wanted is None) == (actual is None), (index, wanted, actual)
    if wanted is not None:
        assert wanted["target"] == actual["target"], (index, wanted, actual)
        assert all(math.isclose(left, right, rel_tol=0.0, abs_tol=0.0001)
                   for left, right in zip(wanted["rect"], actual["rect"])), \
            (index, wanted, actual)

print(f"Hit differential: {len(CASES)} cases passed")
