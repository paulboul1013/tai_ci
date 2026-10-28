#!/usr/bin/env python3
"""Repeatable Python oracle probe for middle-clicking a link.

The frozen Python browser runs with SDL's dummy video driver against the
127.0.0.1 server in tests/middle_click_fixture.py. Middle clicks go through
BrowserWindow.handle_middle_click; webbrowser.open is replaced by a recorder.
Each checkpoint reports the opened external URLs, every tab's URL, heading
and history, the active tab, the address bar, whether the active page has a
focused input, and the requests the step sent. Dynamic ports become <PORT>.

    python3 tests/middle_click_oracle_probe.py          # print the result
    python3 tests/middle_click_oracle_probe.py --check  # compare with fixture
"""

import contextlib
import importlib.util
import io
import json
import os
import pathlib
import sys
import tempfile
import threading
import time


ROOT = pathlib.Path(__file__).resolve().parents[1]
REFERENCE = ROOT / "tests" / "reference"
FIXTURE = ROOT / "tests" / "fixtures" / "middle_click_oracle.json"

sys.path.insert(0, str(ROOT / "tests"))
import middle_click_fixture  # noqa: E402

# Any point above chrome.bottom: Python's middle click never hit-tests the
# chrome, so the point need not avoid its controls.
CHROME_POINT = (400, 2)


def wait_for(predicate, label, timeout=8):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if predicate():
            return
        time.sleep(0.005)
    raise AssertionError("timed out waiting for {}".format(label))


def import_reference():
    import oracle

    oracle.verify_reference()
    os.environ["SDL_VIDEODRIVER"] = "dummy"
    os.environ["BROWSER_RENDER_BACKEND"] = "cpu"
    os.environ["BROWSER_RASTER_MODE"] = "sync"
    descriptor, trace_path = tempfile.mkstemp(
        prefix="tai-middle-click-oracle-", suffix=".trace")
    os.close(descriptor)
    os.environ["BROWSER_TRACE_FILE"] = trace_path

    source = REFERENCE / "browser.py"
    os.chdir(REFERENCE)
    spec = importlib.util.spec_from_file_location(
        "tai_middle_click_fixed_reference", source)
    browser = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(browser)
    return browser, pathlib.Path(trace_path)


class Probe:
    """One BrowserApp window driven through BrowserWindow methods."""

    def __init__(self, browser, server):
        self.browser = browser
        self.server = server
        self.opened = []
        browser.webbrowser.open = self.record_open
        browser.COOKIE_JAR.clear()
        self.app = browser.BrowserApp()
        self.app.running = True
        self.window = None
        self.mark = 0

    def record_open(self, url):
        self.opened.append(url)
        return True

    def close(self):
        app = self.app
        for window in list(app.windows):
            window.close()
        wait_for(lambda: not app.network.active_workers,
                 "network workers to finish", timeout=8)
        app.network.set_needs_quit()
        app.network.join_thread(timeout=2)
        app.raster.set_needs_quit()
        app.raster.join_thread(timeout=2)
        app.measure.finish()
        self.browser.sdl2.SDL_Quit()

    def tab_call(self, tab, function, *args):
        done = threading.Event()
        result = {}

        def invoke():
            try:
                result["value"] = function(*args)
            except BaseException as exc:
                result["error"] = exc
            finally:
                done.set()

        if not self.window.schedule_tab_task(
                tab, invoke, priority=self.browser.TaskPriority.INPUT,
                source="middle-click-oracle-probe"):
            raise AssertionError("could not schedule tab-owned probe")
        wait_for(done.is_set, "tab task")
        if "error" in result:
            raise result["error"]
        return result.get("value")

    @property
    def tab(self):
        return self.window.active_tab_snapshot()

    def commit(self):
        self.tab_call(self.tab, self.tab.run_animation_frame)
        self.window.raster_and_draw()

    def settle(self):
        self.tab_call(self.tab, lambda: None)
        self.commit()

    def heading(self, tab=None):
        browser = self.browser
        tab = tab or self.tab
        if tab.nodes is None:
            return None
        for node in browser.tree_to_list(tab.nodes, []):
            if isinstance(node, browser.Element) and node.tag == "h1":
                return "".join(
                    item.text for item in browser.tree_to_list(node, [])
                    if isinstance(item, browser.Text)).strip()
        return None

    def wait_heading(self, heading, tab_count=1):
        wait_for(lambda: (len(self.window.tabs) == tab_count
                          and self.heading() == heading),
                 "tab {} document {}".format(tab_count, heading))
        self.commit()

    def open(self, path, heading):
        self.window = self.app.new_window(
            self.browser.URL(self.server.url(path)))
        self.wait_heading(heading)
        self.mark = self.server.mark()

    def set_draft(self, text):
        chrome = self.window.chrome
        chrome.focus = "address bar"
        chrome.address_bar = text
        chrome.address_bar_cursor = len(text)
        chrome.address_bar_dirty = True

    def page_point(self, select):
        """Center of the last layout object under an element select picks.

        The last one is the innermost: a block-level input or paragraph
        spans the whole line, its input box or text does not."""
        browser = self.browser
        tab = self.tab

        def locate():
            if tab.document is None:
                tab.run_animation_frame()
            point = None
            for obj in browser.tree_to_list(tab.document, []):
                if getattr(obj, "width", None) is None or obj.width <= 0:
                    continue
                ancestor = getattr(obj, "node", None)
                while ancestor is not None:
                    if (isinstance(ancestor, browser.Element)
                            and select(ancestor)):
                        point = (obj.x + obj.width / 2,
                                 obj.y + obj.height / 2 - tab.scroll)
                        break
                    ancestor = ancestor.parent
            return point

        point = self.tab_call(tab, locate)
        if point is None:
            raise AssertionError("page element not found")
        return point[0], point[1] + self.window.chrome.bottom

    def click(self, select):
        x, y = self.page_point(select)
        self.window.handle_click(x, y)
        self.settle()

    def middle_click(self, select):
        self.window.handle_middle_click(*self.page_point(select))

    def state(self):
        normalize = self.server.normalize
        chrome = self.window.chrome
        window = self.window
        active = window.active_tab_snapshot()
        result = {
            "opened": list(self.opened),
            "tab_count": len(window.tabs),
            "active": window.tabs.index(active),
            "tabs": [{
                "url": normalize(str(tab.url)),
                "heading": self.heading(tab),
                "history": [normalize(str(url)) for url in tab.history],
                "history_index": tab.history_index,
            } for tab in window.tabs],
            "address": normalize(chrome.address_bar_display_text()),
            "focused": chrome.focus == "address bar",
            "dirty": bool(chrome.address_bar_dirty),
            "input_focused": active.focus is not None,
            "requests": self.server.since(self.mark),
        }
        self.opened.clear()
        self.mark = self.server.mark()
        return result


def link(href):
    return lambda node: node.tag == "a" and node.attributes.get("href") == href


def tag(name):
    return lambda node: node.tag == name


def plain_text(node):
    return node.tag == "p" and node.attributes.get("id") == "part"


# ---------------------------------------------------------------------------
# Scenarios. Each receives a fresh Probe and returns named checkpoints.


def opens(name, heading):
    """Middle-clicking a link loads it in a new active tab."""
    def scenario(probe):
        probe.open("/links", "links")
        probe.middle_click(link(probe.server.href(name)))
        probe.wait_heading(heading, tab_count=2)
        return {"opened": probe.state()}
    scenario.__name__ = name
    return scenario


def mailto(probe):
    """A middle-clicked mailto link opens externally and adds no tab."""
    probe.open("/links", "links")
    probe.middle_click(link(probe.server.href("mailto")))
    probe.settle()
    return {"clicked": probe.state()}


def cancelled(probe):
    """preventDefault in a click listener does not stop a middle click."""
    probe.open("/cancel", "cancel")
    probe.middle_click(link(middle_click_fixture.CANCEL_HREF))
    probe.wait_heading("page-b", tab_count=2)
    return {"opened": probe.state()}


def draft(probe):
    """An address draft survives a middle click: unfocused, still dirty."""
    probe.open("/links", "links")
    steps = {}
    probe.set_draft("typed draft")
    probe.middle_click(plain_text)
    probe.settle()
    steps["text"] = probe.state()
    probe.set_draft("typed draft")
    probe.middle_click(link(probe.server.href("absolute")))
    probe.wait_heading("page-b", tab_count=2)
    steps["link"] = probe.state()
    return steps


def chrome(probe):
    """A content middle click keeps page focus; a chrome one blurs it."""
    probe.open("/links", "links")
    steps = {}
    probe.click(tag("input"))
    steps["focused"] = probe.state()
    probe.middle_click(plain_text)
    probe.settle()
    steps["text"] = probe.state()
    probe.set_draft("typed draft")
    probe.window.handle_middle_click(*CHROME_POINT)
    probe.settle()
    steps["chrome"] = probe.state()
    return steps


SCENARIOS = (
    ("absolute", opens("absolute", "page-b")),
    ("relative", opens("relative", "page-c")),
    ("fragment", opens("fragment", "links")),
    ("mailto", mailto),
    ("cancelled", cancelled),
    ("draft", draft),
    ("chrome", chrome),
)


def run_probe():
    original_cwd = pathlib.Path.cwd()
    server = trace_path = None
    try:
        browser, trace_path = import_reference()
        result = {}
        for name, scenario in SCENARIOS:
            server = middle_click_fixture.MiddleClickServer()
            probe = Probe(browser, server)
            try:
                result[name] = scenario(probe)
            finally:
                probe.close()
                server.close()
                server = None
        return result
    finally:
        if server is not None:
            server.close()
        os.chdir(original_cwd)
        if trace_path is not None:
            trace_path.unlink(missing_ok=True)


def main():
    import argparse

    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true",
                        help="compare the probe output with {}".format(
                            FIXTURE))
    args = parser.parse_args()
    # The oracle prints diagnostics from its network and tab threads.
    with contextlib.redirect_stdout(io.StringIO()):
        result = run_probe()
    if args.check:
        expected = json.loads(FIXTURE.read_text(encoding="utf-8"))
        if result != expected:
            print(json.dumps(result, ensure_ascii=False, indent=2,
                             sort_keys=True), file=sys.stderr)
            raise SystemExit("Python middle-click oracle differs from {}"
                             .format(FIXTURE))
        print("Python middle-click oracle probe matches "
              "tests/fixtures/middle_click_oracle.json")
    else:
        print(json.dumps(result, ensure_ascii=False, indent=2, sort_keys=True))


if __name__ == "__main__":
    main()
