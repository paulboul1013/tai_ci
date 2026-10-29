"""Turn oracle and native page output into the E2E checkpoint shape.

A checkpoint is the one contract between the Python oracle probe and the
native E2E test (docs/e2e-plan.md, "設計：資料形狀與 seams"):

    {"url", "title", "history": {"back", "forward"}, "scroll",
     "dom", "layout", "display", "inputs", "requests"}

The oracle side reads live browser.py objects; the native side reads the JSON
object tai_page_json prints (the same one tai-browser --headless writes).
Both produce "dom", "layout" and "display" through the same reductions:
layout_shape here and the display leaves of tests/display_differential.py.
"""

import json
import re

import display_differential
import oracle


PORT_TOKEN = "PORT"
CHECKPOINT_KEYS = ("url", "title", "history", "scroll", "dom", "layout",
                   "display", "inputs", "requests")


def layout_shape(node):
    keys = ("kind", "x", "y", "width", "height", "word")
    return {**{key: node[key] for key in keys if key in node},
            "children": [layout_shape(child) for child in node["children"]]}


def normalize(value):
    """Replace every 127.0.0.1 origin's port with PORT_TOKEN, recursively."""
    if isinstance(value, str):
        return re.sub(r"http://127\.0\.0\.1:\d+",
                      "http://127.0.0.1:" + PORT_TOKEN, value)
    if isinstance(value, list):
        return [normalize(item) for item in value]
    if isinstance(value, dict):
        return {key: normalize(item) for key, item in value.items()}
    return value


def requests_shape(entries):
    """Server log entries as a sorted list: a page's stylesheets and scripts
    are fetched in parallel, so their arrival order is not part of the
    contract."""
    keys = ("method", "path", "cookie", "body")
    rows = [{key: entry[key] for key in keys} for entry in entries]
    return sorted(rows, key=lambda row: tuple(
        "" if row[key] is None else row[key] for key in keys))


def checkpoint(url, title, back, forward, scroll, page, inputs, requests):
    """page holds "dom", "layout" and "display" from oracle_page or
    native_page."""
    return normalize({
        "url": url, "title": title,
        "history": {"back": back, "forward": forward},
        "scroll": scroll,
        "dom": page["dom"], "layout": page["layout"],
        "display": page["display"],
        "inputs": inputs, "requests": requests_shape(requests)})


def _plain(value):
    return json.loads(json.dumps(value, allow_nan=False))


def oracle_page(browser, nodes, document):
    """dom, layout and display of a browser.py tab's committed page.

    The display list is painted again from document because a commit hands
    the tab's own list to the window."""
    paths = oracle.node_paths(nodes)
    commands = []
    browser.paint_tree(document, commands)
    return _plain({
        "dom": oracle.dom_value(nodes, True),
        "layout": layout_shape(oracle.layout_value(document, paths)),
        "display": display_differential.oracle_leaves(
            [oracle.display_value(command, paths) for command in commands])})


def oracle_inputs(browser, nodes):
    """Every <input> with an id: its value attribute and checkbox state."""
    inputs = {}
    for node in browser.tree_to_list(nodes, []):
        if (isinstance(node, browser.Element) and node.tag == "input"
                and "id" in node.attributes):
            inputs[node.attributes["id"]] = {
                "value": node.attributes.get("value", ""),
                "checked": bool(getattr(node, "is_checked", False))}
    return inputs


def _without_input_word(box):
    # Native json_box (src/layout.c) writes an InputLayout's painted value as
    # "word"; browser.py's InputLayout has no word. The value is compared
    # through the display text leaves and "inputs" instead.
    shaped = {key: value for key, value in box.items()
              if not (key == "word" and box.get("kind") == "InputLayout")}
    shaped["children"] = [_without_input_word(child)
                          for child in box["children"]]
    return shaped


def native_page(page_json):
    """dom, layout and display of a tai_page_json object."""
    return {"dom": page_json["dom"],
            "layout": _without_input_word(layout_shape(page_json["layout"])),
            "display": display_differential.native_leaves(
                page_json["display"])}
