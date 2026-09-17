#!/usr/bin/env python3
"""比較原生 headless navigation 的穩定 DOM/style/layout 與 Python oracle。"""
import json
import math
import pathlib
import subprocess
import sys
from urllib.parse import quote

ROOT = pathlib.Path(__file__).resolve().parents[1]
ORACLE = ROOT / "tests" / "oracle.py"
CASES = [
    "<p>Hello world</p>",
    "<style>p { color:red; }</style><p>Hello</p>",
    "<div>first</div><div>second<br>line</div>",
    "<pre>one\n\n  two\tthree</pre>",
    "<p style='text-align:center;font-size:150%'>center text</p>",
]

def layout_shape(node):
    keys = ("kind", "x", "y", "width", "height", "word")
    return {**{key: node[key] for key in keys if key in node},
            "children": [layout_shape(child) for child in node["children"]]}

def compare(expected, actual, path="root"):
    if isinstance(expected, dict):
        assert expected.keys() == actual.keys(), (path, expected.keys(), actual.keys())
        for key in expected:
            compare(expected[key], actual[key], path + "/" + key)
    elif isinstance(expected, list):
        assert len(expected) == len(actual), (path, len(expected), len(actual))
        for index, (left, right) in enumerate(zip(expected, actual)):
            compare(left, right, path + "/" + str(index))
    elif isinstance(expected, (float, int)):
        assert math.isclose(expected, actual, abs_tol=0.0001), (path, expected, actual)
    else:
        assert expected == actual, (path, expected, actual)

for index, html in enumerate(CASES):
    url = "data:text/html," + quote(html, safe="")
    actual = json.loads(subprocess.check_output([sys.argv[1], "--headless", url]))
    expected_dom = json.loads(subprocess.check_output(
        [sys.executable, str(ORACLE), "style", html]))
    expected_layout = json.loads(subprocess.check_output(
        [sys.executable, str(ORACLE), "layout", html]))["layout"]
    compare(expected_dom, actual["dom"], f"case {index}/dom")
    compare(layout_shape(expected_layout), layout_shape(actual["layout"]),
            f"case {index}/layout")
print(f"Browser differential: {len(CASES)} headless workflows passed")
