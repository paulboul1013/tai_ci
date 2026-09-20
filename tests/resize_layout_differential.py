#!/usr/bin/env python3
"""Compare a width-sensitive relayout case with the frozen Python oracle."""
import json
import math
import pathlib
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[1]
HTML = "<p>one two three four five six</p>"
WIDTH = 80


def normalize(node):
    keys = ("kind", "x", "y", "width", "height", "word")
    return {**{key: node[key] for key in keys if key in node},
            "children": [normalize(child) for child in node["children"]]}


def compare(expected, actual, path="root"):
    assert expected.keys() == actual.keys(), (path, expected.keys(), actual.keys())
    for key in expected:
        if isinstance(expected[key], dict):
            compare(expected[key], actual[key], path + "/" + key)
        elif isinstance(expected[key], list):
            assert len(expected[key]) == len(actual[key]), path + "/" + key
            for index, (left, right) in enumerate(zip(expected[key], actual[key])):
                compare(left, right, path + "/" + key + "/" + str(index))
        elif isinstance(expected[key], (float, int)):
            assert math.isclose(expected[key], actual[key], abs_tol=0.0001), (
                path + "/" + key, expected[key], actual[key])
        else:
            assert expected[key] == actual[key], (path + "/" + key,
                                                   expected[key], actual[key])


with tempfile.TemporaryDirectory() as directory:
    html_path = pathlib.Path(directory) / "input.html"
    html_path.write_text(HTML)
    expected = json.loads(subprocess.check_output(
        [sys.executable, str(ROOT / "tests/oracle.py"), "layout", HTML,
         "--width", str(WIDTH)]))["layout"]
    actual = json.loads(subprocess.check_output(
        [sys.argv[1], str(html_path), str(ROOT / "tests/reference/browser.css"),
         str(WIDTH)]))
    compare(normalize(expected), normalize(actual))

print("width-sensitive resize layout differential passed")
