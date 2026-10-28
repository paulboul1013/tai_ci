#!/usr/bin/env python3
"""Repeatable Python oracle probe for scrolling overflow:scroll containers.

The frozen Python browser runs with SDL's dummy video driver against the
127.0.0.1 server in tests/overflow_scroll_fixture.py. Clicks, the wheel and
the Up/Down keys go through BrowserWindow.handle_click, handle_mousewheel,
handle_down and handle_up, so they choose and use Tab.scroll_focus as the
real event loop does. Each checkpoint reports the page scroll offset and the
scroll_y of every container still in the document, by id.

    python3 tests/overflow_scroll_oracle_probe.py          # print the result
    python3 tests/overflow_scroll_oracle_probe.py --check  # compare
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
FIXTURE = ROOT / "tests" / "fixtures" / "overflow_scroll_oracle.json"

sys.path.insert(0, str(ROOT / "tests"))
import overflow_scroll_fixture  # noqa: E402

# Above the tab strip and the toolbar: a chrome click that hits nothing.
CHROME_POINT = (400, 2)
WHEEL_UP, WHEEL_DOWN = 1, -1


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
        prefix="tai-overflow-scroll-oracle-", suffix=".trace")
    os.close(descriptor)
    os.environ["BROWSER_TRACE_FILE"] = trace_path

    source = REFERENCE / "browser.py"
    os.chdir(REFERENCE)
    spec = importlib.util.spec_from_file_location(
        "tai_overflow_scroll_fixed_reference", source)
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
                source="overflow-scroll-oracle-probe"):
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

    def heading(self):
        browser = self.browser
        tab = self.tab
        if tab.nodes is None:
            return None
        for node in browser.tree_to_list(tab.nodes, []):
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
            raise AssertionError("page element not found")
        return point[0], point[1] + self.window.chrome.bottom

    def click(self, element_id, settle=True):
        self.window.handle_click(*self.page_point(element_id))
        if settle:
            self.settle()

    def click_chrome(self):
        self.window.handle_click(*CHROME_POINT)
        self.settle()

    def wheel(self, delta):
        self.window.handle_mousewheel(delta)
        self.settle()

    def down(self, times=1):
        for _ in range(times):
            self.window.handle_down()
            self.settle()

    def up(self):
        self.window.handle_up()
        self.settle()

    def state(self):
        browser = self.browser
        tab = self.tab

        def read():
            containers = {}
            for node in browser.tree_to_list(tab.nodes, []):
                if not isinstance(node, browser.Element):
                    continue
                name = node.attributes.get("id")
                if name in overflow_scroll_fixture.CONTAINERS:
                    containers[name] = getattr(node, "scroll_y", 0)
            return {"page": tab.scroll, "containers": containers}

        return self.tab_call(tab, read)


# ---------------------------------------------------------------------------
# Scenarios. Each receives a fresh Probe and returns named checkpoints.


def container(probe):
    """Wheel and keys scroll the clicked container, then stop at its end."""
    probe.open("/box", "box")
    steps = {}
    probe.click("box-text")
    steps["clicked"] = probe.state()
    probe.wheel(WHEEL_DOWN)
    steps["wheel_down"] = probe.state()
    probe.down()
    steps["key_down"] = probe.state()
    probe.wheel(WHEEL_UP)
    steps["wheel_up"] = probe.state()
    probe.up()
    steps["key_up"] = probe.state()
    probe.down(5)
    steps["limit"] = probe.state()
    probe.down()
    steps["past_limit"] = probe.state()
    return steps


def outside(probe):
    """Without a container click, or after one outside, the page scrolls."""
    probe.open("/box", "box")
    steps = {}
    probe.down()
    steps["unclicked"] = probe.state()
    probe.up()
    probe.click("box-text")
    probe.click("outside")
    probe.wheel(WHEEL_DOWN)
    steps["outside"] = probe.state()
    return steps


def nested(probe):
    """A click focuses the innermost container around it."""
    probe.open("/nested", "nested")
    steps = {}
    probe.click("inner-text")
    probe.down()
    steps["inner"] = probe.state()
    probe.click("outer-text")
    probe.down()
    steps["outer"] = probe.state()
    probe.down(4)
    steps["outer_limit"] = probe.state()
    return steps


def cancelled(probe):
    """A cancelled click clears the focus without choosing its container."""
    probe.open("/cancel", "cancel")
    steps = {}
    probe.click("box-text")
    probe.down()
    steps["box"] = probe.state()
    probe.click("cancel-text")
    probe.down()
    steps["cancelled"] = probe.state()
    return steps


def gone(probe):
    """A focused container removed from the document hands over to the
    page."""
    probe.open("/detach", "detach")
    probe.click("gone-text")
    probe.down()
    return {"removed": probe.state()}


def flat(probe):
    """A focused container that stops being scrollable does the same."""
    probe.open("/detach", "detach")
    probe.click("flat-text")
    probe.down()
    return {"restyled": probe.state()}


def fragment(probe):
    """A fragment link inside a container scrolls the page there and keeps
    the container focused."""
    probe.open("/box", "box")
    steps = {}
    probe.click("fragment")
    steps["clicked"] = probe.state()
    probe.down()
    steps["box"] = probe.state()
    return steps


def navigation(probe):
    """A link inside a container loads a page that scrolls itself."""
    probe.open("/box", "box")
    steps = {}
    probe.click("away", settle=False)
    probe.wait_heading("after")
    probe.down()
    steps["page"] = probe.state()
    return steps


def chrome(probe):
    """A chrome click clears the focus."""
    probe.open("/box", "box")
    steps = {}
    probe.click("box-text")
    probe.down()
    steps["box"] = probe.state()
    probe.click_chrome()
    probe.wheel(WHEEL_DOWN)
    steps["chrome"] = probe.state()
    return steps


SCENARIOS = (
    ("container", container),
    ("outside", outside),
    ("nested", nested),
    ("cancelled", cancelled),
    ("gone", gone),
    ("flat", flat),
    ("fragment", fragment),
    ("navigation", navigation),
    ("chrome", chrome),
)


def run_probe():
    original_cwd = pathlib.Path.cwd()
    server = trace_path = None
    try:
        browser, trace_path = import_reference()
        result = {}
        for name, scenario in SCENARIOS:
            server = overflow_scroll_fixture.OverflowScrollServer()
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
            raise SystemExit("Python overflow-scroll oracle differs from {}"
                             .format(FIXTURE))
        print("Python overflow-scroll oracle probe matches "
              "tests/fixtures/overflow_scroll_oracle.json")
    else:
        print(json.dumps(result, ensure_ascii=False, indent=2,
                         sort_keys=True))


if __name__ == "__main__":
    main()
