#!/usr/bin/env python3
"""Repeatable Python oracle probe for the full-pipeline E2E script.

The frozen Python browser runs with SDL's dummy video driver against the
127.0.0.1 server in tests/e2e_fixture.py. Clicks, typing and Back go through
BrowserWindow.handle_click, handle_key and schedule_go_back, as the real event
loop does. After each step the probe commits a frame and records one
checkpoint in the shape tests/e2e_shape.py defines; "requests" holds the
server log entries since the previous checkpoint. "viewport" records the
Python tab's width and height. Native's tab is shorter (PORTING_PLAN.md "Tab
列換行時的 viewport"), which is why the fixture's #bottom sits below the
maximum scroll of both. Fixture ports are replaced with PORT.

    python3 tests/e2e_oracle_probe.py          # print the result
    python3 tests/e2e_oracle_probe.py --check  # compare
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
FIXTURE = ROOT / "tests" / "fixtures" / "e2e_oracle.json"

sys.path.insert(0, str(ROOT / "tests"))
import e2e_fixture  # noqa: E402
import e2e_shape  # noqa: E402


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
    descriptor, trace_path = tempfile.mkstemp(prefix="tai-e2e-oracle-",
                                              suffix=".trace")
    os.close(descriptor)
    os.environ["BROWSER_TRACE_FILE"] = trace_path

    source = REFERENCE / "browser.py"
    os.chdir(REFERENCE)
    spec = importlib.util.spec_from_file_location(
        "tai_e2e_fixed_reference", source)
    browser = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(browser)
    return browser, pathlib.Path(trace_path)


class Probe:
    """One BrowserApp window driven through BrowserWindow methods."""

    def __init__(self, browser, server):
        self.browser = browser
        self.server = server
        browser.COOKIE_JAR.clear()
        self.app = browser.BrowserApp()
        self.app.running = True
        self.window = None
        self.mark = 0

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
                source="e2e-oracle-probe"):
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

    def has_id(self, element_id):
        browser = self.browser
        tab = self.tab

        def find():
            if tab.nodes is None:
                return False
            return any(isinstance(node, browser.Element)
                       and node.attributes.get("id") == element_id
                       for node in browser.tree_to_list(tab.nodes, []))

        return self.tab_call(tab, find)

    def wait_loaded(self, title, element_id):
        """Wait until the page has the title and the element, so a script
        that sets either has run; then commit its frame."""
        wait_for(lambda: (self.tab_call(self.tab, self.tab.get_title) == title
                          and self.has_id(element_id)),
                 "page {}".format(title))
        self.commit()

    def open(self, path):
        self.window = self.app.new_window(
            self.browser.URL(self.server.url(path)))

    def page_point(self, element_id):
        """Center of the last layout object under the element with an id.

        The last one is the innermost: its text, not a block spanning the
        whole line."""
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
                            and ancestor.attributes.get("id") == element_id):
                        point = (obj.x + obj.width / 2,
                                 obj.y + obj.height / 2 - tab.scroll)
                        break
                    ancestor = ancestor.parent
            return point

        point = self.tab_call(tab, locate)
        if point is None:
            raise AssertionError("page element {} not found".format(
                element_id))
        return point[0], point[1] + self.window.chrome.bottom

    def click(self, element_id):
        self.window.handle_click(*self.page_point(element_id))
        self.settle()

    def type(self, text):
        for char in text:
            self.window.handle_key(char)
            self.settle()

    def navigating(self, start):
        """Run start, then wait until the tab has begun the navigation it
        triggers. Tab.navigate drops the tab's pending tasks, so a probe
        task queued before that point would never run."""
        tab = self.tab
        generation = tab.navigation_generation
        start()
        wait_for(lambda: tab.navigation_generation > generation,
                 "navigation to start")

    def click_away(self, element_id):
        point = self.page_point(element_id)
        self.navigating(lambda: self.window.handle_click(*point))

    def back(self):
        self.navigating(self.window.schedule_go_back)

    def viewport(self):
        tab = self.tab
        return {"width": tab.width, "height": tab.tab_height}

    def checkpoint(self):
        browser = self.browser
        tab = self.tab

        def read():
            return {
                "url": str(tab.url), "title": tab.get_title(),
                "back": tab.can_go_back(), "forward": tab.can_go_forward(),
                "scroll": tab.scroll,
                "page": e2e_shape.oracle_page(browser, tab.nodes,
                                              tab.document),
                "inputs": e2e_shape.oracle_inputs(browser, tab.nodes)}

        state = self.tab_call(tab, read)
        mark = self.server.mark()
        requests = self.server.since(self.mark)
        self.mark = mark
        return e2e_shape.checkpoint(
            state["url"], state["title"], state["back"], state["forward"],
            state["scroll"], state["page"], state["inputs"], requests)


# ---------------------------------------------------------------------------
# The script. Each step returns after its frame commits.


def pipeline(probe):
    """Open, jump to a fragment, fill and post a form, then go Back twice."""
    steps = []

    def record(name):
        steps.append({"step": name, "checkpoint": probe.checkpoint()})

    probe.open("/")
    probe.wait_loaded(e2e_fixture.READY_TITLE, "added")
    record("open")

    probe.click("to-bottom")
    record("fragment")

    probe.click_away("to-form")
    probe.wait_loaded(e2e_fixture.FORM_TITLE, "name")
    record("form")

    probe.click("name")
    probe.type(e2e_fixture.TEXT_VALUE)
    probe.click("secret")
    probe.type(e2e_fixture.PASSWORD_VALUE)
    probe.click("agree")
    record("filled")

    probe.click_away("send")
    probe.wait_loaded(e2e_fixture.SUBMITTED_TITLE, "field-name")
    record("submit")

    probe.back()
    probe.wait_loaded(e2e_fixture.FORM_TITLE, "name")
    record("back_form")

    probe.back()
    probe.wait_loaded(e2e_fixture.READY_TITLE, "added")
    record("back_index")
    return steps


SCENARIOS = (
    ("pipeline", pipeline),
)


def run_probe():
    original_cwd = pathlib.Path.cwd()
    server = trace_path = None
    try:
        browser, trace_path = import_reference()
        result = {"scenarios": {}}
        for name, scenario in SCENARIOS:
            server = e2e_fixture.E2EServer()
            probe = Probe(browser, server)
            try:
                result["scenarios"][name] = scenario(probe)
                result["viewport"] = probe.viewport()
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
            raise SystemExit("Python E2E oracle differs from {}"
                             .format(FIXTURE))
        print("Python E2E oracle probe matches tests/fixtures/e2e_oracle.json")
    else:
        print(json.dumps(result, ensure_ascii=False, indent=2,
                         sort_keys=True))


if __name__ == "__main__":
    main()
