#!/usr/bin/env python3
"""Repeatable Python oracle probe for the native tabs slice.

The probe uses the frozen Python browser with SDL's dummy video driver and a
local HTTP server whose barriers keep document and stylesheet requests pending
until the test explicitly releases them. Dynamic ports, transport error text,
and wall-clock timings are omitted from the JSON result.

Run from any directory with:

    python3 tests/tabs_oracle_probe.py
"""

import contextlib
import importlib.util
import io
import json
import math
import os
import pathlib
import re
import sys
import tempfile
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer


ROOT = pathlib.Path(__file__).resolve().parents[1]
REFERENCE = ROOT / "tests" / "reference"
PORT_TOKEN = "<PORT>"


class ControlledServer:
    def __init__(self):
        self.seen = {}
        self.gates = {
            "/initial-delay": threading.Event(),
            "/secondary-delay": threading.Event(),
            "/slow.css": threading.Event(),
        }
        self.lock = threading.Lock()
        owner = self

        class Handler(BaseHTTPRequestHandler):
            protocol_version = "HTTP/1.1"

            def log_message(self, *_args):
                pass

            def do_GET(self):
                path = self.path.split("?", 1)[0]
                with owner.lock:
                    owner.seen.setdefault(path, threading.Event()).set()

                gate = owner.gates.get(path)
                if gate is not None and not gate.wait(timeout=15):
                    self.close_connection = True
                    return

                if path in ("/initial-fail", "/secondary-fail"):
                    self.close_connection = True
                    try:
                        self.connection.shutdown(2)
                    except OSError:
                        pass
                    self.connection.close()
                    return

                if path == "/initial-delay":
                    label, paragraphs = "initial-loaded", 45
                elif path == "/secondary-delay":
                    label, paragraphs = "secondary-loaded", 70
                elif path == "/subresource-page":
                    label, paragraphs = "subresource-document", 10
                    body = (
                        "<!doctype html><html><head>"
                        '<link rel="stylesheet" href="/slow.css">'
                        "</head><body><h1>subresource-document</h1>"
                        "<p class='probe'>subresource pending</p></body></html>"
                    ).encode()
                    self._respond(body)
                    return
                elif path == "/slow.css":
                    body = b"p { color: red; }"
                    self._respond(body, content_type="text/css")
                    return
                else:
                    label, paragraphs = "unexpected", 0

                chunks = ["<!doctype html><html><body><h1>", label, "</h1>"]
                chunks.extend("<p>{} {}</p>".format(label, i)
                              for i in range(paragraphs))
                chunks.append("</body></html>")
                self._respond("".join(chunks).encode())

            def _respond(self, body, content_type="text/html; charset=utf-8"):
                self.send_response(200)
                self.send_header("Content-Type", content_type)
                self.send_header("Content-Length", str(len(body)))
                self.send_header("Connection", "close")
                self.end_headers()
                try:
                    self.wfile.write(body)
                except (BrokenPipeError, ConnectionResetError):
                    pass
                self.close_connection = True

        self.server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        self.server.daemon_threads = True
        self.port = self.server.server_port
        self.thread = threading.Thread(
            target=self.server.serve_forever,
            name="tabs-oracle-http-fixture",
            daemon=True,
        )
        self.thread.start()

    def url(self, path):
        return "http://127.0.0.1:{}{}".format(self.port, path)

    def wait_seen(self, path):
        with self.lock:
            event = self.seen.setdefault(path, threading.Event())
        wait_for(event.is_set, "HTTP request {}".format(path))

    def release(self, path):
        self.gates[path].set()

    def close(self):
        for gate in self.gates.values():
            gate.set()
        self.server.shutdown()
        self.server.server_close()
        self.thread.join(timeout=2)


def wait_for(predicate, label, timeout=8):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if predicate():
            return
        time.sleep(0.005)
    raise AssertionError("timed out waiting for {}".format(label))


def normalize(value):
    return re.sub(r"(http://127\.0\.0\.1):\d+", r"\1:" + PORT_TOKEN, value)


def import_reference():
    sys.path.insert(0, str(ROOT / "tests"))
    import oracle

    oracle.verify_reference()
    os.environ["SDL_VIDEODRIVER"] = "dummy"
    os.environ["BROWSER_RENDER_BACKEND"] = "cpu"
    os.environ["BROWSER_RASTER_MODE"] = "sync"
    descriptor, trace_path = tempfile.mkstemp(prefix="tai-tabs-oracle-", suffix=".trace")
    os.close(descriptor)
    os.environ["BROWSER_TRACE_FILE"] = trace_path

    source = REFERENCE / "browser.py"
    os.chdir(REFERENCE)
    spec = importlib.util.spec_from_file_location("tai_tabs_fixed_reference", source)
    browser = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(browser)
    return browser, pathlib.Path(trace_path)


def layout_rect(layout):
    return [
        round(float(layout.x), 3),
        round(float(layout.y), 3),
        round(float(layout.x + layout.width), 3),
        round(float(layout.y + layout.height), 3),
    ]


def descendants(node):
    return [node] + [child for sub in node.children for child in descendants(sub)]


def tab_anchor_info(browser, chrome, index):
    href = "tab-{}".format(index)
    anchor = next(
        node for node in browser.tree_to_list(chrome.nodes, [])
        if isinstance(node, browser.Element)
        and node.tag == "a"
        and node.attributes.get("href") == href
    )
    text_nodes = {
        id(node) for node in descendants(anchor)
        if isinstance(node, browser.Text)
    }
    fragments = [
        obj for obj in browser.tree_to_list(chrome.document, [])
        if id(getattr(obj, "node", None)) in text_nodes
        and getattr(obj, "x", None) is not None
        and getattr(obj, "width", None) is not None
    ]
    if not fragments:
        raise AssertionError("tab link has no text layout: {}".format(href))
    left = min(float(obj.x) for obj in fragments)
    top = min(float(obj.y) for obj in fragments)
    right = max(float(obj.x + obj.width) for obj in fragments)
    bottom = max(float(obj.y + obj.height) for obj in fragments)
    return {
        "href": href,
        "style": anchor.attributes.get("style", ""),
        "rect": [round(left, 3), round(top, 3), round(right, 3), round(bottom, 3)],
        "center": [round((left + right) / 2, 3), round((top + bottom) / 2, 3)],
    }


def target_at(browser, chrome, x, y):
    output = io.StringIO()
    with contextlib.redirect_stdout(output):
        layout = chrome.layout_object_at(x, y)
    node = getattr(layout, "node", None)
    while node is not None:
        if isinstance(node, browser.Element):
            if node.tag == "button":
                return "button:" + node.attributes.get("id", "")
            if node.tag == "a":
                return "link:" + node.attributes.get("href", "")
        node = node.parent
    return None


def h1_text(browser, tab):
    if tab.nodes is None:
        return None
    for node in browser.tree_to_list(tab.nodes, []):
        if isinstance(node, browser.Element) and node.tag == "h1":
            return "".join(
                item.text for item in browser.tree_to_list(node, [])
                if isinstance(item, browser.Text)
            ).strip()
    return None


def tab_snapshot(browser, tab):
    return {
        "url": normalize(str(tab.url)) if tab.url is not None else None,
        "history": [normalize(str(url)) for url in tab.history],
        "history_index": tab.history_index,
        "heading": h1_text(browser, tab),
    }


def tab_call(browser, window, tab, function, *args):
    done = threading.Event()
    result = {}

    def invoke():
        try:
            result["value"] = function(*args)
        except BaseException as exc:
            result["error"] = exc
        finally:
            done.set()

    scheduled = window.schedule_tab_task(
        tab,
        invoke,
        priority=browser.TaskPriority.INPUT,
        source="tabs-oracle-probe",
    )
    if not scheduled:
        raise AssertionError("could not schedule tab-owned probe")
    wait_for(done.is_set, "tab task")
    if "error" in result:
        raise result["error"]
    return result.get("value")


def render_tab(browser, window, tab):
    tab_call(browser, window, tab, tab.render)


def wait_loaded(browser, tab, heading):
    wait_for(lambda: h1_text(browser, tab) == heading, "document {}".format(heading))


def wait_resized(browser, window, tabs, width, height):
    expected_tab_height = max(1.0, float(height) - float(window.chrome.bottom))
    wait_for(
        lambda: all(
            tab.width == width
            and math.isclose(tab.tab_height, expected_tab_height, abs_tol=0.001)
            for tab in tabs
        ),
        "all tab viewports resized",
    )
    for tab in tabs:
        render_tab(browser, window, tab)
    return round(expected_tab_height, 3)


def run_probe():
    original_cwd = pathlib.Path.cwd()
    browser = None
    app = None
    http = None
    trace_path = None
    result = None
    try:
        browser, trace_path = import_reference()
        http = ControlledServer()
        app = browser.BrowserApp()

        initial_url = browser.URL(http.url("/initial-delay"))
        window = app.new_window(initial_url)
        initial_tab = window.tabs[0]
        http.wait_seen("/initial-delay")
        wait_for(lambda: bool(initial_tab.history), "initial history entry")
        assert initial_tab.nodes is None

        secondary_tab = window.new_tab(browser.URL(http.url("/secondary-delay")))
        http.wait_seen("/secondary-delay")
        assert len(window.tabs_snapshot()) == 2
        assert secondary_tab is window.active_tab_snapshot()

        chrome = window.chrome
        chrome.render()
        new_tab_button = chrome.find_button_layout("new-tab")
        if new_tab_button is None:
            raise AssertionError("missing New Tab button layout")
        new_rect = layout_rect(new_tab_button)
        new_center = [
            (new_rect[0] + new_rect[2]) / 2,
            (new_rect[1] + new_rect[3]) / 2,
        ]
        wide_tabs = [tab_anchor_info(browser, chrome, index) for index in range(2)]
        new_tab_boundary = {
            "left": target_at(browser, chrome, new_rect[0], new_center[1]),
            "right": target_at(browser, chrome, new_rect[2], new_center[1]),
        }
        tab_boundary = {
            "left": target_at(
                browser, chrome, wide_tabs[0]["rect"][0],
                wide_tabs[0]["center"][1],
            ),
            "right": target_at(
                browser, chrome, wide_tabs[0]["rect"][2],
                wide_tabs[0]["center"][1],
            ),
        }
        assert new_tab_boundary == {
            "left": "button:new-tab",
            "right": None,
        }
        assert tab_boundary == {"left": "link:tab-0", "right": None}

        requested_new_tab_urls = []
        original_new_tab = window.new_tab
        window.new_tab = lambda url: requested_new_tab_urls.append(str(url))
        chrome.address_bar = "https://draft.invalid/"
        chrome.address_bar_cursor = len(chrome.address_bar)
        chrome.address_bar_dirty = True
        with contextlib.redirect_stdout(io.StringIO()):
            chrome.click(*new_center)
        window.new_tab = original_new_tab
        assert requested_new_tab_urls == ["https://browser.engineering/"]
        draft_after_new_tab = {
            "address": chrome.address_bar,
            "dirty": chrome.address_bar_dirty,
            "focus": chrome.focus,
        }

        chrome.render()
        first_anchor = tab_anchor_info(browser, chrome, 0)
        chrome.address_bar = "https://dirty.invalid/"
        chrome.address_bar_cursor = len(chrome.address_bar)
        chrome.address_bar_dirty = True
        with contextlib.redirect_stdout(io.StringIO()):
            chrome.click(*first_anchor["center"])
        assert window.active_tab_snapshot() is initial_tab
        assert chrome.address_bar == "" and not chrome.address_bar_dirty
        draft_after_tab_switch = {
            "address": chrome.address_bar,
            "dirty": chrome.address_bar_dirty,
            "focus": chrome.focus,
        }

        chrome.render()
        active_styles = [
            tab_anchor_info(browser, chrome, index)["style"]
            for index in range(2)
        ]
        second_anchor = tab_anchor_info(browser, chrome, 1)
        with contextlib.redirect_stdout(io.StringIO()):
            chrome.click(*second_anchor["center"])
        assert window.active_tab_snapshot() is secondary_tab
        active_index_after_switch_back = window.tabs_snapshot().index(
            window.active_tab_snapshot()
        )

        # The narrow layout is an oracle measurement. Restore a usable width
        # before releasing either document request.
        window.resize(120, 1200)
        wait_resized(browser, window, [initial_tab, secondary_tab], 120, 1200)
        chrome.render()
        narrow_tabs = [tab_anchor_info(browser, chrome, index) for index in range(2)]
        narrow_geometry = {
            "chrome_bottom": round(float(chrome.bottom), 3),
            "tab_row_rects": [item["rect"] for item in narrow_tabs],
            "address_rect": layout_rect(
                chrome.find_address_layout(chrome.find_address_node())
            ),
        }
        window.resize(800, 600)
        wait_resized(browser, window, [initial_tab, secondary_tab], 800, 600)

        pending_before_release = {
            "window_exists": window.sdl_window is not None,
            "tab_count": len(window.tabs_snapshot()),
            "initial_tab": tab_snapshot(browser, initial_tab),
            "secondary_tab": tab_snapshot(browser, secondary_tab),
            "active_index": window.tabs_snapshot().index(window.active_tab_snapshot()),
        }

        # Both responses were held while the window remained usable. Release the
        # secondary first so the inactive initial tab completes later.
        http.release("/secondary-delay")
        wait_loaded(browser, secondary_tab, "secondary-loaded")
        render_tab(browser, window, secondary_tab)
        active_after_secondary = window.tabs_snapshot().index(window.active_tab_snapshot())
        http.release("/initial-delay")
        wait_loaded(browser, initial_tab, "initial-loaded")
        render_tab(browser, window, initial_tab)
        active_after_initial = window.tabs_snapshot().index(window.active_tab_snapshot())
        loaded_tab_snapshots = [
            tab_snapshot(browser, initial_tab),
            tab_snapshot(browser, secondary_tab),
        ]
        secondary_before_failure = loaded_tab_snapshots[1]

        # Scroll each tab independently, then grow the window so the old bottom
        # offsets exceed the new maximum if the oracle leaves them untouched.
        window.resize(800, 300)
        small_tab_height = wait_resized(
            browser, window, [initial_tab, secondary_tab], 800, 300
        )
        for tab in (initial_tab, secondary_tab):
            tab_call(browser, window, tab, tab.scroll_by, 100000)
        scroll_before = [round(float(tab.scroll), 3) for tab in (initial_tab, secondary_tab)]
        window.resize(1000, 900)
        large_tab_height = wait_resized(
            browser, window, [initial_tab, secondary_tab], 1000, 900
        )
        scroll_after = [round(float(tab.scroll), 3) for tab in (initial_tab, secondary_tab)]
        max_scroll_after = [
            round(max(float(tab.document.height + 2 * browser.VSTEP - tab.tab_height), 0), 3)
            for tab in (initial_tab, secondary_tab)
        ]

        # The document is parsed while its external stylesheet is still pending.
        subresource_url = browser.URL(http.url("/subresource-page"))
        window.schedule_load(subresource_url, tab=initial_tab)
        http.wait_seen("/subresource-page")
        http.wait_seen("/slow.css")
        wait_for(lambda: h1_text(browser, initial_tab) == "subresource-document",
                 "document parsed before stylesheet")
        window.set_active_tab(secondary_tab)
        window.resize(900, 700)
        subresource_tab_height = wait_resized(
            browser, window, [initial_tab, secondary_tab], 900, 700
        )
        css_pending = {
            "document_heading": h1_text(browser, initial_tab),
            "active_index": window.tabs_snapshot().index(window.active_tab_snapshot()),
            "tab_widths": [tab.width for tab in (initial_tab, secondary_tab)],
            "tab_heights": [
                round(float(tab.tab_height), 3)
                for tab in (initial_tab, secondary_tab)
            ],
        }
        http.release("/slow.css")
        wait_for(
            lambda: initial_tab.needs_render,
            "delayed stylesheet processed",
        )
        render_tab(browser, window, initial_tab)
        probe_paragraph = next(
            node for node in browser.tree_to_list(initial_tab.nodes, [])
            if isinstance(node, browser.Element)
            and node.tag == "p"
            and "probe" in node.attributes.get("class", "")
        )

        failed_initial_window = app.new_window(
            browser.URL(http.url("/initial-fail"))
        )
        failed_initial_tab = failed_initial_window.tabs[0]
        http.wait_seen("/initial-fail")
        wait_for(lambda: h1_text(browser, failed_initial_tab) == "Network Error",
                 "initial Network Error document")

        failed_secondary_url = browser.URL(http.url("/secondary-fail"))
        window.schedule_load(failed_secondary_url, tab=secondary_tab)
        http.wait_seen("/secondary-fail")
        wait_for(lambda: h1_text(browser, secondary_tab) == "Network Error",
                 "secondary Network Error document")
        secondary_after_failure = tab_snapshot(browser, secondary_tab)

        result = {
            "geometry": {
                "wide": {
                    "chrome_bottom": round(float(chrome.bottom), 3),
                    "new_tab_rect": new_rect,
                    "tab_rects": [item["rect"] for item in wide_tabs],
                    "new_tab_hit_boundary": new_tab_boundary,
                    "tab_hit_boundary": tab_boundary,
                },
                "narrow": narrow_geometry,
            },
            "new_tab": {
                "requested_url": requested_new_tab_urls[0],
            },
            "switching": {
                "active_styles_after_select": active_styles,
                "active_index_after_switch_back": active_index_after_switch_back,
                "draft_after_new_tab": draft_after_new_tab,
                "draft_after_tab_switch": draft_after_tab_switch,
            },
            "pending_documents": {
                "before_release": pending_before_release,
                "active_index_after_secondary_completion": active_after_secondary,
                "active_index_after_initial_completion": active_after_initial,
                "initial_tab": loaded_tab_snapshots[0],
                "secondary_tab": loaded_tab_snapshots[1],
            },
            "resize_and_scroll": {
                "small_tab_height": small_tab_height,
                "large_tab_height": large_tab_height,
                "tab_widths_after_resize": [tab.width for tab in (initial_tab, secondary_tab)],
                "tab_heights_after_resize": [round(float(tab.tab_height), 3) for tab in (initial_tab, secondary_tab)],
                "scroll_before": scroll_before,
                "scroll_after": scroll_after,
                "max_scroll_after": max_scroll_after,
            },
            "delayed_subresource": {
                "while_pending": css_pending,
                "computed_probe_color_after_release": probe_paragraph.style.get("color"),
            },
            "failures": {
                "initial_navigation": tab_snapshot(browser, failed_initial_tab),
                "secondary_navigation": {
                    "before_failure": secondary_before_failure,
                    "after_failure": secondary_after_failure,
                },
            },
        }
    finally:
        if http is not None:
            for gate in http.gates.values():
                gate.set()
        if app is not None:
            for window in list(app.windows):
                window.close()
            if app.network is not None:
                wait_for(
                    lambda: not app.network.active_workers,
                    "network workers to finish",
                    timeout=8,
                )
                app.network.set_needs_quit()
                app.network.join_thread(timeout=2)
            if app.raster is not None:
                app.raster.set_needs_quit()
                app.raster.join_thread(timeout=2)
            app.measure.finish()
            if browser is not None:
                browser.sdl2.SDL_Quit()
        if http is not None:
            http.close()
        os.chdir(original_cwd)
        if trace_path is not None:
            trace_path.unlink(missing_ok=True)

    return result


def main():
    import argparse

    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--check",
        action="store_true",
        help="compare the probe output with tests/fixtures/tabs_oracle.json",
    )
    args = parser.parse_args()
    result = run_probe()
    if args.check:
        expected_path = ROOT / "tests" / "fixtures" / "tabs_oracle.json"
        expected = json.loads(expected_path.read_text(encoding="utf-8"))
        if result != expected:
            print(json.dumps(result, ensure_ascii=False, indent=2, sort_keys=True),
                  file=sys.stderr)
            raise SystemExit("Python tabs oracle differs from {}".format(expected_path))
        print("Python tabs oracle probe matches tests/fixtures/tabs_oracle.json")
    else:
        print(json.dumps(result, ensure_ascii=False, indent=2, sort_keys=True))


if __name__ == "__main__":
    main()
