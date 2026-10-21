#!/usr/bin/env python3
"""Repeatable Python oracle probe for mailto: links and addresses.

The frozen Python browser runs with SDL's dummy video driver against the
127.0.0.1 server in tests/mailto_fixture.py. webbrowser.open is replaced by a
recorder, so each checkpoint reports the URLs the browser handed to the
external application next to the active tab's URL, history, heading, address
bar state and the requests the step sent. Dynamic ports become <PORT>.

    python3 tests/mailto_oracle_probe.py            # print the result
    python3 tests/mailto_oracle_probe.py --check    # compare with fixture
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
FIXTURE = ROOT / "tests" / "fixtures" / "mailto_oracle.json"

sys.path.insert(0, str(ROOT / "tests"))
import mailto_fixture  # noqa: E402


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
    descriptor, trace_path = tempfile.mkstemp(prefix="tai-mailto-oracle-",
                                              suffix=".trace")
    os.close(descriptor)
    os.environ["BROWSER_TRACE_FILE"] = trace_path

    source = REFERENCE / "browser.py"
    os.chdir(REFERENCE)
    spec = importlib.util.spec_from_file_location(
        "tai_mailto_fixed_reference", source)
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
                source="mailto-oracle-probe"):
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

    def heading(self):
        browser = self.browser
        if self.tab.nodes is None:
            return None
        for node in browser.tree_to_list(self.tab.nodes, []):
            if isinstance(node, browser.Element) and node.tag == "h1":
                return "".join(
                    item.text for item in browser.tree_to_list(node, [])
                    if isinstance(item, browser.Text)).strip()
        return None

    def wait_heading(self, heading):
        wait_for(lambda: self.heading() == heading,
                 "document {}".format(heading))
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
        """Center of the first layout object under an element select picks."""
        browser = self.browser
        tab = self.tab

        def locate():
            if tab.document is None:
                tab.run_animation_frame()
            for obj in browser.tree_to_list(tab.document, []):
                if getattr(obj, "width", None) is None or obj.width <= 0:
                    continue
                ancestor = getattr(obj, "node", None)
                while ancestor is not None:
                    if (isinstance(ancestor, browser.Element)
                            and select(ancestor)):
                        return (obj.x + obj.width / 2,
                                obj.y + obj.height / 2 - tab.scroll)
                    ancestor = ancestor.parent
            return None

        point = self.tab_call(tab, locate)
        if point is None:
            raise AssertionError("page element not found")
        return point[0], point[1] + self.window.chrome.bottom

    def click(self, select, navigates=False):
        x, y = self.page_point(select)
        self.window.handle_click(x, y)
        if navigates:
            # Tab.navigate clears queued tab tasks, so do not queue one.
            return
        self.tab_call(self.tab, lambda: None)
        self.commit()

    def state(self):
        normalize = self.server.normalize
        chrome = self.window.chrome
        tab = self.tab
        result = {
            "opened": list(self.opened),
            "url": normalize(str(tab.url)),
            "heading": self.heading(),
            "history": [normalize(str(url)) for url in tab.history],
            "history_index": tab.history_index,
            "address": normalize(chrome.address_bar_display_text()),
            "focused": chrome.focus == "address bar",
            "dirty": bool(chrome.address_bar_dirty),
            "requests": self.server.since(self.mark),
        }
        self.opened.clear()
        self.mark = self.server.mark()
        return result


def link(href):
    return lambda node: node.tag == "a" and node.attributes.get("href") == href


# ---------------------------------------------------------------------------
# Scenarios. Each receives a fresh Probe and returns named checkpoints.


def address(probe):
    """Enter in the address bar hands mailto: to the mail application."""
    probe.open("/a", "page-a")
    steps = {}
    for name, text in (
            ("query", "mailto:someone@example.com?subject=Hi"),
            ("padded_fragment", "  mailto:pad@example.com#part  "),
            ("bare", "mailto:")):
        probe.set_draft(text)
        probe.window.handle_enter()
        probe.tab_call(probe.tab, lambda: None)
        probe.commit()
        steps[name] = probe.state()
    return steps


def links(probe):
    """Clicking a mailto link opens it externally and keeps the page."""
    probe.open("/links", "links")
    steps = {}
    for name in ("query", "fragment"):
        probe.set_draft("typed draft")
        probe.click(link(mailto_fixture.LINKS[name]))
        steps[name] = probe.state()
    return steps


def cancelled(probe):
    """preventDefault in a click listener keeps the link from opening."""
    probe.open("/cancel", "cancel")
    probe.click(link("mailto:cancel@example.com"))
    return {"clicked": probe.state()}


def form(probe):
    """A form whose action is mailto: loads the fixed explanation page."""
    probe.open("/form", "form")
    probe.click(lambda node: node.tag == "button", navigates=True)
    probe.wait_heading("External mail link")
    return {"submitted": probe.state()}


SCENARIOS = (
    ("address", address),
    ("links", links),
    ("cancelled", cancelled),
    ("form", form),
)


def run_probe():
    original_cwd = pathlib.Path.cwd()
    server = trace_path = None
    try:
        browser, trace_path = import_reference()
        result = {}
        for name, scenario in SCENARIOS:
            server = mailto_fixture.MailtoServer()
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
                        help="compare the probe output with {}".format(FIXTURE))
    args = parser.parse_args()
    # The oracle prints diagnostics from its network and tab threads.
    with contextlib.redirect_stdout(io.StringIO()):
        result = run_probe()
    if args.check:
        expected = json.loads(FIXTURE.read_text(encoding="utf-8"))
        if result != expected:
            print(json.dumps(result, ensure_ascii=False, indent=2,
                             sort_keys=True), file=sys.stderr)
            raise SystemExit("Python mailto oracle differs from {}".format(
                FIXTURE))
        print("Python mailto oracle probe matches "
              "tests/fixtures/mailto_oracle.json")
    else:
        print(json.dumps(result, ensure_ascii=False, indent=2, sort_keys=True))


if __name__ == "__main__":
    main()
