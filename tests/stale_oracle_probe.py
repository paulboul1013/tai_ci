#!/usr/bin/env python3
"""Python oracle probe for stale load completions under a slow network.

The frozen Python browser runs with SDL's dummy video driver against the
127.0.0.1 server in tests/stale_fixture.py. Each scenario starts in a fresh
tab, holds a document or subresource at a gate, starts a newer navigation,
and then lets the late response arrive. After each step it records only what
a user can observe: the tab's URL, heading, history list and index,
Back/Forward availability, and the paths the server received. Dynamic ports
are normalized; no timing is recorded.

    python3 tests/stale_oracle_probe.py            # print the result
    python3 tests/stale_oracle_probe.py --check    # compare with the fixture
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
FIXTURE = ROOT / "tests" / "fixtures" / "stale_oracle.json"
# After a late response was written, the tab gets this long to (wrongly)
# apply it. Absence cannot be awaited, so this window is bounded, not ordered.
QUIET_SECONDS = 0.5

sys.path.insert(0, str(ROOT / "tests"))
import stale_fixture  # noqa: E402


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
    descriptor, trace_path = tempfile.mkstemp(prefix="tai-stale-oracle-",
                                              suffix=".trace")
    os.close(descriptor)
    os.environ["BROWSER_TRACE_FILE"] = trace_path

    source = REFERENCE / "browser.py"
    os.chdir(REFERENCE)
    spec = importlib.util.spec_from_file_location(
        "tai_stale_fixed_reference", source)
    browser = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(browser)
    return browser, pathlib.Path(trace_path)


class Probe:
    def __init__(self, browser, window, server):
        self.browser = browser
        self.window = window
        self.server = server

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
                source="stale-oracle-probe"):
            raise AssertionError("could not schedule tab-owned probe")
        wait_for(done.is_set, "tab task")
        if "error" in result:
            raise result["error"]
        return result.get("value")

    def commit(self, tab):
        self.tab_call(tab, tab.run_animation_frame)
        self.window.raster_and_draw()

    def settle(self, tab):
        self.tab_call(tab, lambda: None)

    def quiet(self, tab):
        """Let the tab run whatever the late response queued."""
        deadline = time.monotonic() + QUIET_SECONDS
        while time.monotonic() < deadline:
            self.settle(tab)
            time.sleep(0.01)
        self.commit(tab)

    def heading(self, tab):
        browser = self.browser
        if tab.nodes is None:
            return None
        for node in browser.tree_to_list(tab.nodes, []):
            if isinstance(node, browser.Element) and node.tag == "h1":
                return "".join(
                    item.text for item in browser.tree_to_list(node, [])
                    if isinstance(item, browser.Text)).strip()
        return None

    def wait_heading(self, tab, heading):
        wait_for(lambda: self.heading(tab) == heading,
                 "document {}".format(heading))
        self.commit(tab)

    def go(self, tab, path):
        self.window.schedule_load(self.browser.URL(self.server.url(path)),
                                  tab=tab)

    def go_pending(self, tab, path, mark):
        """Start a navigation and return once its request reached the server."""
        self.go(tab, path)
        self.server.wait_seen(path, mark)
        self.settle(tab)

    def new_tab(self):
        tab = self.window.new_tab(self.browser.URL(self.server.url("/start")))
        self.wait_heading(tab, "start")
        return tab

    def arrive_late(self, tab, path):
        self.server.release(path)
        self.server.wait_served(path)
        self.quiet(tab)

    def state(self, tab, mark=None):
        normalize = self.server.normalize
        result = {
            "url": normalize(str(tab.url)) if tab.url is not None else None,
            "heading": self.heading(tab),
            "history": [normalize(str(url)) for url in tab.history],
            "history_index": tab.history_index,
            "can_go_back": bool(tab.can_go_back()),
            "can_go_forward": bool(tab.can_go_forward()),
        }
        if mark is not None:
            result["requests"] = self.server.since(mark)
        return result


# ---------------------------------------------------------------------------
# Scenarios. Each returns a dict of named checkpoints.


def css_late(probe):
    """G1: the document arrived, its stylesheet is held, the user moves on."""
    server = probe.server
    tab = probe.new_tab()
    mark = server.mark()
    probe.go(tab, "/css-a")
    server.wait_seen("/slow.css", mark)
    probe.go(tab, "/b")
    probe.wait_heading(tab, "page-b")
    steps = {"b_committed": probe.state(tab, mark)}
    probe.arrive_late(tab, "/slow.css")
    steps["css_released"] = probe.state(tab, mark)
    server.reset_gate("/slow.css")
    return steps


def js_late(probe):
    """G2: as G1, but the held resource is a script with side effects."""
    server = probe.server
    tab = probe.new_tab()
    mark = server.mark()
    probe.go(tab, "/js-a")
    server.wait_seen("/slow.js", mark)
    probe.go(tab, "/b")
    probe.wait_heading(tab, "page-b")
    steps = {"b_committed": probe.state(tab, mark)}
    probe.arrive_late(tab, "/slow.js")
    steps["js_released"] = probe.state(tab, mark)
    server.reset_gate("/slow.js")
    return steps


def three_pending(probe):
    server = probe.server
    tab = probe.new_tab()
    mark = server.mark()
    for path in ("/slow-a", "/slow-b", "/slow-c"):
        probe.go_pending(tab, path, mark)
    probe.commit(tab)
    return tab, mark, {"pending": probe.state(tab)}


def out_of_order_oldest_first(probe):
    """G3a: the two superseded documents arrive before the newest one."""
    tab, mark, steps = three_pending(probe)
    probe.arrive_late(tab, "/slow-a")
    steps["a_released"] = probe.state(tab)
    probe.arrive_late(tab, "/slow-b")
    steps["b_released"] = probe.state(tab)
    probe.server.release("/slow-c")
    probe.wait_heading(tab, "slow-c")
    steps["c_committed"] = probe.state(tab, mark)
    for path in ("/slow-a", "/slow-b", "/slow-c"):
        probe.server.reset_gate(path)
    return steps


def out_of_order_newest_first(probe):
    """G3b: the newest document commits, then the older ones arrive."""
    tab, mark, steps = three_pending(probe)
    probe.server.release("/slow-c")
    probe.wait_heading(tab, "slow-c")
    steps["c_committed"] = probe.state(tab)
    probe.arrive_late(tab, "/slow-a")
    probe.arrive_late(tab, "/slow-b")
    steps["a_b_released"] = probe.state(tab, mark)
    for path in ("/slow-a", "/slow-b", "/slow-c"):
        probe.server.reset_gate(path)
    return steps


SCENARIOS = (
    ("css_late", css_late),
    ("js_late", js_late),
    ("out_of_order_oldest_first", out_of_order_oldest_first),
    ("out_of_order_newest_first", out_of_order_newest_first),
)


def run_probe():
    original_cwd = pathlib.Path.cwd()
    browser = app = server = trace_path = None
    try:
        server = stale_fixture.StaleServer()
        browser, trace_path = import_reference()
        app = browser.BrowserApp()
        window = app.new_window(browser.URL(server.url("/start")))
        probe = Probe(browser, window, server)
        probe.wait_heading(window.tabs[0], "start")
        result = {}
        for name, scenario in SCENARIOS:
            result[name] = scenario(probe)
        return result
    finally:
        if server is not None:
            for gate in server.gates.values():
                gate.set()
        if app is not None:
            for window in list(app.windows):
                window.close()
            if app.network is not None:
                wait_for(lambda: not app.network.active_workers,
                         "network workers to finish", timeout=8)
                app.network.set_needs_quit()
                app.network.join_thread(timeout=2)
            if app.raster is not None:
                app.raster.set_needs_quit()
                app.raster.join_thread(timeout=2)
            app.measure.finish()
            if browser is not None:
                browser.sdl2.SDL_Quit()
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
            raise SystemExit("Python stale oracle differs from {}".format(
                FIXTURE))
        print("Python stale oracle probe matches "
              "tests/fixtures/stale_oracle.json")
    else:
        print(json.dumps(result, ensure_ascii=False, indent=2, sort_keys=True))


if __name__ == "__main__":
    main()
