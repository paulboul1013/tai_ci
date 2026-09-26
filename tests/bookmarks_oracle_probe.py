#!/usr/bin/env python3
"""Exercise the frozen Python bookmark behavior against a local HTTP fixture.

The JSON separates observed Python behavior from the requested native two-star
UX. Run from any directory; --check compares a port-independent snapshot.
"""

import argparse
import contextlib
from html import escape
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import io
import json
import os
from pathlib import Path
import re
import sys
import tempfile
import threading
import time


ROOT = Path(__file__).resolve().parents[1]
PORT_TOKEN = "<PORT>"
SPECIAL_PATH = '/alpha?x=1&y=2#frag"\'<tag>&'


def wait_for(predicate, label, timeout=8):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if predicate():
            return
        time.sleep(0.005)
    raise AssertionError("timed out waiting for " + label)


class FixtureServer:
    def __init__(self):
        self.requests = []
        self.lock = threading.Lock()
        owner = self

        class Handler(BaseHTTPRequestHandler):
            def log_message(self, *_args):
                pass

            def do_GET(self):
                with owner.lock:
                    owner.requests.append(self.path)
                heading = "alpha" if self.path.startswith("/alpha?") else "zeta"
                body = ("<!doctype html><html><body><h1>" + heading +
                        "</h1></body></html>").encode()
                self.send_response(200)
                self.send_header("Content-Type", "text/html; charset=utf-8")
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)

        self.server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        self.server.daemon_threads = True
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()

    def url(self, path):
        return "http://127.0.0.1:{}{}".format(self.server.server_port, path)

    def count(self, path):
        with self.lock:
            return self.requests.count(path)

    def close(self):
        self.server.shutdown()
        self.server.server_close()
        self.thread.join(timeout=2)


def normalize(value):
    if isinstance(value, str):
        return re.sub(r"(http://127\.0\.0\.1):\d+", r"\1:" + PORT_TOKEN, value)
    if isinstance(value, list):
        return [normalize(item) for item in value]
    if isinstance(value, dict):
        return {key: normalize(item) for key, item in value.items()}
    return value


def import_reference():
    sys.path.insert(0, str(ROOT / "tests"))
    import oracle

    oracle.verify_reference()
    os.environ["SDL_VIDEODRIVER"] = "dummy"
    os.environ["BROWSER_RENDER_BACKEND"] = "cpu"
    os.environ["BROWSER_RASTER_MODE"] = "sync"
    descriptor, trace_name = tempfile.mkstemp(prefix="tai-bookmarks-oracle-", suffix=".trace")
    os.close(descriptor)
    os.environ["BROWSER_TRACE_FILE"] = trace_name
    return oracle.load_reference(), Path(trace_name)


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

    assert window.schedule_tab_task(
        tab, invoke, priority=browser.TaskPriority.INPUT,
        source="bookmarks-oracle-probe"
    )
    wait_for(done.is_set, "tab task")
    if "error" in result:
        raise result["error"]
    return result.get("value")


def heading(browser, tab):
    if tab.nodes is None:
        return None
    for node in browser.tree_to_list(tab.nodes, []):
        if isinstance(node, browser.Element) and node.tag == "h1":
            return "".join(child.text for child in browser.tree_to_list(node, [])
                           if isinstance(child, browser.Text)).strip()
    return None


def wait_page(browser, window, tab, expected):
    wait_for(lambda: heading(browser, tab) == expected, "page " + expected)
    tab_call(browser, window, tab, tab.run_animation_frame)
    wait_for(lambda: window.committed_states.get(tab) is not None and
             window.committed_states[tab].url_string == str(tab.url),
             "committed " + expected)


def bookmark_button_click(browser, window):
    chrome = window.chrome
    chrome.render()
    layout = chrome.find_button_layout("bookmark")
    assert layout is not None
    x = layout.x + layout.width / 2
    y = layout.y + layout.height / 2
    with contextlib.redirect_stdout(io.StringIO()):
        chrome.click(x, y)


def anchors(browser, tab):
    result = []
    for node in browser.tree_to_list(tab.nodes, []):
        if isinstance(node, browser.Element) and node.tag == "a":
            label = "".join(child.text for child in browser.tree_to_list(node, [])
                            if isinstance(child, browser.Text))
            result.append((node, node.attributes.get("href"), label))
    return result


def click_anchor(browser, window, tab, anchor):
    text_ids = {id(node) for node in browser.tree_to_list(anchor, [])
                if isinstance(node, browser.Text)}
    fragments = [layout for layout in browser.tree_to_list(tab.document, [])
                 if id(getattr(layout, "node", None)) in text_ids
                 and getattr(layout, "width", 0) > 0]
    assert fragments, "bookmark anchor has no clickable text layout"
    fragment = fragments[0]
    with contextlib.redirect_stdout(io.StringIO()):
        tab_call(browser, window, tab, tab.click,
                 fragment.x + fragment.width / 2,
                 fragment.y + fragment.height / 2)


def run_probe():
    previous_cwd = Path.cwd()
    browser = app = fixture = trace_path = None
    try:
        os.chdir(ROOT / "tests" / "reference")
        browser, trace_path = import_reference()
        fixture = FixtureServer()
        app = browser.BrowserApp()
        window = app.new_window(browser.URL("about:bookmarks"))
        first = window.tabs[0]
        wait_page(browser, window, first, "Bookmarks")
        empty_html = first.bookmarks_page()
        assert "<p>No bookmarks yet.</p>" in empty_html
        bookmark_button_click(browser, window)
        assert not app.bookmarks

        special_url = browser.URL(fixture.url(SPECIAL_PATH))
        zeta_url = browser.URL(fixture.url("/zeta"))
        window.schedule_load(zeta_url, tab=first)
        wait_page(browser, window, first, "zeta")
        window.chrome.address_bar = "http://draft.invalid/"
        window.chrome.address_bar_dirty = True
        assert window.chrome.chrome_html().count("id=bookmark") == 1
        bookmark_button_click(browser, window)
        assert app.bookmarks == {str(zeta_url)}
        assert not window.chrome.address_bar_dirty
        bookmark_button_click(browser, window)
        assert not app.bookmarks
        bookmark_button_click(browser, window)
        assert app.bookmarks == {str(zeta_url)}

        second = window.new_tab(special_url)
        wait_page(browser, window, second, "alpha")
        assert second.bookmarks is app.bookmarks
        bookmark_button_click(browser, window)
        assert app.bookmarks == {str(zeta_url), str(special_url)}
        assert window.is_current_page_bookmarked()
        window.set_active_tab(first)
        assert window.is_current_page_bookmarked()
        window.set_active_tab(second)

        window.schedule_load(browser.URL("about:bookmarks"), tab=second)
        wait_page(browser, window, second, "Bookmarks")
        assert not window.is_current_page_bookmarked()
        before_internal_toggle = set(app.bookmarks)
        bookmark_button_click(browser, window)
        assert app.bookmarks == before_internal_toggle
        page_html = second.bookmarks_page()
        ordered_urls = sorted(app.bookmarks)
        escaped_rows = [f'<li><a href="{escape(url, quote=True)}">{escape(url, quote=True)}</a></li>'
                        for url in ordered_urls]
        assert [line for line in page_html.splitlines() if line.startswith("<li>")] == escaped_rows
        parsed_anchors = anchors(browser, second)
        escaped_urls = [escape(url, quote=True) for url in ordered_urls]
        assert [(href, label) for _, href, label in parsed_anchors] == [
            (safe_url, url) for safe_url, url in zip(escaped_urls, ordered_urls)
        ], [(href, label) for _, href, label in parsed_anchors]
        assert "&amp;" in page_html and "&quot;" in page_html
        assert "&#x27;" in page_html and "&lt;tag&gt;" in page_html

        # The reference parser preserves HTML entities in attribute values.
        # Its generated link therefore navigates to a different query string.
        clicked_get_path = "/alpha?x=1&amp;y=2"
        alpha_before = fixture.count(clicked_get_path)
        click_anchor(browser, window, second, parsed_anchors[0][0])
        wait_for(lambda: fixture.count(clicked_get_path) == alpha_before + 1,
                 "bookmarked URL GET")
        wait_page(browser, window, second, "alpha")
        assert str(second.url) == parsed_anchors[0][1]
        assert not window.is_current_page_bookmarked()
        clicked_url = str(second.url)
        history_after_click = [str(url) for url in second.history]
        tab_call(browser, window, second, second.go_back)
        wait_page(browser, window, second, "Bookmarks")
        assert str(second.url) == "about:bookmarks"
        assert [str(url) for url in second.history] == history_after_click

        window.schedule_load(browser.URL("about:blank"), tab=second)
        wait_for(lambda: str(second.url) == "about:blank", "blank navigation")
        tab_call(browser, window, second, second.run_animation_frame)
        wait_for(lambda: window.active_url_string() == "about:blank",
                 "committed blank URL")
        assert window.current_url_string() is None
        blank_before = set(app.bookmarks)
        bookmark_button_click(browser, window)
        assert app.bookmarks == blank_before

        return normalize({
            "python_reference": {
                "toolbar_bookmark_buttons": 1,
                "empty_page_message": "No bookmarks yet.",
                "toggle_sequence_on_zeta": ["saved", "removed", "saved"],
                "address_draft_ignored_and_discarded": True,
                "shared_across_tabs": first.bookmarks is second.bookmarks is app.bookmarks,
                "sorted_urls": ordered_urls,
                "escaped_rows": escaped_rows,
                "parsed_links": [{"href": href, "text": label}
                                 for _, href, label in parsed_anchors],
                "link_click_get_path": clicked_get_path,
                "link_click_url": clicked_url,
                "link_click_matches_saved_url": False,
                "history_after_link_click": history_after_click,
                "back_url": "about:bookmarks",
                "internal_page_toggle_changed_bookmarks": False,
                "blank_page_toggle_changed_bookmarks": False,
            },
            "planned_native_ux": {
                "address_bar_inner_star": "toggle committed page without navigation",
                "left_star": "open about:bookmarks in active tab",
                "list_links": "navigate to the exact saved URL",
                "shared_collection": True,
                "persistence": "pending user decision",
            },
        })
    finally:
        if app is not None:
            for window in list(app.windows):
                window.close()
            if app.network is not None:
                wait_for(lambda: not app.network.active_workers,
                         "network workers to finish")
                app.network.set_needs_quit()
                app.network.join_thread(timeout=2)
            if app.raster is not None:
                app.raster.set_needs_quit()
                app.raster.join_thread(timeout=2)
            app.measure.finish()
            browser.sdl2.SDL_Quit()
        if fixture is not None:
            fixture.close()
        os.chdir(previous_cwd)
        if trace_path is not None:
            trace_path.unlink(missing_ok=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true",
                        help="compare output to tests/fixtures/bookmarks_oracle.json")
    args = parser.parse_args()
    result = run_probe()
    expected_path = ROOT / "tests" / "fixtures" / "bookmarks_oracle.json"
    if args.check:
        expected = json.loads(expected_path.read_text(encoding="utf-8"))
        if result != expected:
            print(json.dumps(result, ensure_ascii=False, indent=2, sort_keys=True), file=sys.stderr)
            raise SystemExit("Python bookmarks oracle differs from " + str(expected_path))
        print("Python bookmarks oracle probe matches tests/fixtures/bookmarks_oracle.json")
    else:
        print(json.dumps(result, ensure_ascii=False, indent=2, sort_keys=True))


if __name__ == "__main__":
    main()
