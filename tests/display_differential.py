#!/usr/bin/env python3
"""Compare the currently supported native display leaves with the oracle."""

import json
import math
import pathlib
import subprocess
import sys
from urllib.parse import quote


ROOT = pathlib.Path(__file__).resolve().parents[1]
ORACLE = ROOT / "tests" / "oracle.py"
CASES = [
    "<div style='background-color:#123456;color:#abcdef'>Hello</div>",
    "<div style='background-color:lightblue'>first</div>"
    "<div style='background-color:#00ff00;color:orange'>second</div>",
    "<div style='width:70px;background-color:#fedcba'>one two three four</div>",
    "<div style='background-color:#112233'>outer "
    "<span style='color:#445566'>nested words</span></div>",
    "<div style='background-color:#1234'>unsupported color fallback</div>",
]
SCROLL_CASES = [
    "<div style='height:20px;overflow:scroll;background-color:#112233'>"
    "<div style='background-color:#445566'>first</div>"
    "<div style='background-color:#778899'>second</div></div>",
    "<div style='height:20px;overflow:scroll;background-color:#112233'></div>",
    "<div style='height:30px;overflow:scroll;background-color:#112233'>"
    "<div style='height:20px;overflow:scroll;background-color:#445566'>"
    "<div style='background-color:#778899'>nested</div>"
    "<div style='background-color:#aabbcc'>content</div></div></div>",
]
ROUNDED_FILL_CASES = [
    "<div style='height:20px;border-radius:10px;background-color:#112233'></div>",
    "<div style='height:20px;border-radius:10.5px;background-color:#112233'></div>",
    "<div style='height:20px;border-radius:1e999px;background-color:#112233'></div>",
]


NAMED_COLORS = {
    "black": (0, 0, 0, 255),
    "white": (255, 255, 255, 255),
    "red": (255, 0, 0, 255),
    "green": (0, 128, 0, 255),
    "blue": (0, 0, 255, 255),
    "yellow": (255, 255, 0, 255),
    "gray": (128, 128, 128, 255),
    "grey": (128, 128, 128, 255),
    "lightgray": (211, 211, 211, 255),
    "lightgrey": (211, 211, 211, 255),
    "lightblue": (173, 216, 230, 255),
    "lightgreen": (144, 238, 144, 255),
    "orange": (255, 165, 0, 255),
    "orangered": (255, 69, 0, 255),
}


def rgba(value):
    value = value.strip().casefold()
    channels = NAMED_COLORS.get(value)
    if channels is None:
        raw = value.removeprefix("#")
        if len(raw) == 3:
            channels = tuple(int(char * 2, 16) for char in raw) + (255,)
        elif len(raw) == 6:
            channels = tuple(int(raw[index:index + 2], 16)
                             for index in (0, 2, 4)) + (255,)
        elif len(raw) == 8:
            channels = tuple(int(raw[index:index + 2], 16)
                             for index in (0, 2, 4, 6))
        else:
            channels = (0, 0, 0, 255)
    red, green, blue, alpha = channels
    return red << 24 | green << 16 | blue << 8 | alpha


def oracle_leaves(commands):
    leaves = []
    for command in commands:
        kind = command["kind"]
        if kind in ("DrawRect", "DrawText"):
            left, top, right, bottom = command["rect"]
            leaf = {
                "kind": "fill_rect" if kind == "DrawRect" else "text",
                "x": left,
                "y": top,
                "width": right - left,
                "height": bottom - top,
                "rgba": rgba(command["color"]),
            }
            if kind == "DrawText":
                leaf["text"] = command["text"]
            leaves.append(leaf)
        oracle_leaves_into = command.get("children")
        if oracle_leaves_into is not None:
            leaves.extend(oracle_leaves(oracle_leaves_into))
    return leaves


def native_leaves(commands):
    keys = ("kind", "x", "y", "width", "height", "rgba", "text")
    return [{key: command[key] for key in keys if key in command}
            for command in commands
            if command["kind"] in ("fill_rect", "text")]


def oracle_rounded_fills(commands):
    fills = []
    for command in commands:
        if command["kind"] in ("DrawRect", "DrawRRect"):
            left, top, right, bottom = command["rect"]
            fill = {"kind": "fill_rect", "x": left, "y": top,
                    "width": right - left, "height": bottom - top,
                    "rgba": rgba(command["color"])}
            if command["kind"] == "DrawRRect":
                fill["radius"] = command["radius"]
            fills.append(fill)
        if "children" in command:
            fills.extend(oracle_rounded_fills(command["children"]))
    return fills


def native_rounded_fills(commands):
    keys = ("kind", "x", "y", "width", "height", "rgba", "radius")
    return [{key: command[key] for key in keys if key in command}
            for command in commands if command["kind"] == "fill_rect"]


def oracle_scroll_structure(commands):
    output = []
    for command in commands:
        if command["kind"] == "DrawHitTest":
            left, top, right, bottom = command["rect"]
            output.append({"kind": "hit_test", "x": left, "y": top,
                           "width": right - left, "height": bottom - top})
        elif command["kind"] == "Scroll":
            left, top, right, bottom = command["clip_rect"]
            output.append({"kind": "push_clip_scroll", "x": left, "y": top,
                           "width": right - left, "height": bottom - top,
                           "scroll_y": command["scroll_y"]})
            output.extend(oracle_scroll_structure(command["children"]))
            output.append({"kind": "pop_clip_scroll"})
        elif command["kind"] in ("DrawRect", "DrawText"):
            output.extend(oracle_leaves([command]))
        elif "children" in command:
            output.extend(oracle_scroll_structure(command["children"]))
    return output


def native_scroll_structure(commands):
    leaf_keys = ("kind", "x", "y", "width", "height", "rgba", "text")
    output = []
    for command in commands:
        if command["kind"] in ("push_clip", "pop_clip"):
            continue
        if command["kind"] == "push_clip_scroll":
            output.append({key: command[key] for key in
                           ("kind", "x", "y", "width", "height", "scroll_y")})
        elif command["kind"] == "pop_clip_scroll":
            output.append({"kind": "pop_clip_scroll"})
        elif command["kind"] == "hit_test":
            output.append({key: command[key] for key in
                           ("kind", "x", "y", "width", "height")})
        elif command["kind"] in ("fill_rect", "text"):
            output.append({key: command[key] for key in leaf_keys if key in command})
    return output


def compare(expected, actual, path="display"):
    if isinstance(expected, dict):
        assert expected.keys() == actual.keys(), (path, expected, actual)
        for key in expected:
            compare(expected[key], actual[key], path + "/" + key)
    elif isinstance(expected, list):
        assert len(expected) == len(actual), (path, len(expected), len(actual))
        for index, (left, right) in enumerate(zip(expected, actual)):
            compare(left, right, path + "/" + str(index))
    elif isinstance(expected, (float, int)) and not isinstance(expected, bool):
        assert math.isclose(expected, actual, rel_tol=0.0, abs_tol=0.0001), \
            (path, expected, actual)
    else:
        assert expected == actual, (path, expected, actual)


for index, html in enumerate(CASES):
    expected_output = json.loads(subprocess.check_output(
        [sys.executable, str(ORACLE), "layout", html]))
    url = "data:text/html," + quote(html, safe="")
    actual_output = json.loads(subprocess.check_output(
        [sys.argv[1], "--headless", url]))
    compare(oracle_leaves(expected_output["display"]),
            native_leaves(actual_output["display"]), f"case {index}")

for index, html in enumerate(SCROLL_CASES):
    expected_output = json.loads(subprocess.check_output(
        [sys.executable, str(ORACLE), "layout", html]))
    url = "data:text/html," + quote(html, safe="")
    actual_output = json.loads(subprocess.check_output(
        [sys.argv[1], "--headless", url]))
    compare(oracle_scroll_structure(expected_output["display"]),
            native_scroll_structure(actual_output["display"]),
            f"scroll case {index}")

for index, html in enumerate(ROUNDED_FILL_CASES):
    expected_output = json.loads(subprocess.check_output(
        [sys.executable, str(ORACLE), "layout", html]))
    url = "data:text/html," + quote(html, safe="")
    actual_output = json.loads(subprocess.check_output(
        [sys.argv[1], "--headless", url]))
    compare(oracle_rounded_fills(expected_output["display"]),
            native_rounded_fills(actual_output["display"]),
            f"rounded fill case {index}")

print(f"Display differential: {len(CASES)} leaf and "
      f"{len(SCROLL_CASES)} scroll and {len(ROUNDED_FILL_CASES)} rounded "
      "fill cases passed")
