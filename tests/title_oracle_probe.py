#!/usr/bin/env python3
"""Repeatable Python oracle probe for the window-title work item.

The frozen Python browser runs with SDL's dummy video driver against the
127.0.0.1 server in tests/title_fixture.py, plus an HTTPS server signed by an
untrusted per-run CA (tests/https_fixture.py) for the Certificate Error page.
Each checkpoint records the active tab's Tab.get_title(), the committed
state's title and SDL_GetWindowTitle() after a present. Dynamic ports are
replaced with <PORT>.

    python3 tests/title_oracle_probe.py            # print the result
    python3 tests/title_oracle_probe.py --check    # compare with fixture
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
FIXTURE = ROOT / "tests" / "fixtures" / "title_oracle.json"

sys.path.insert(0, str(ROOT / "tests"))
import https_fixture  # noqa: E402
import title_fixture  # noqa: E402


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
    descriptor, trace_path = tempfile.mkstemp(prefix="tai-title-oracle-",
                                              suffix=".trace")
    os.close(descriptor)
    os.environ["BROWSER_TRACE_FILE"] = trace_path

    source = REFERENCE / "browser.py"
    os.chdir(REFERENCE)
    spec = importlib.util.spec_from_file_location(
        "tai_title_fixed_reference", source)
    browser = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(browser)
    return browser, pathlib.Path(trace_path)


class Probe:
    """One BrowserApp driven directly through BrowserWindow methods."""

    def __init__(self, browser, server, https):
        self.browser = browser
        self.server = server
        self.https = https
        browser.COOKIE_JAR.clear()
        self.app = browser.BrowserApp()
        self.app.running = True

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

    def url(self, path):
        return self.browser.URL(self.server.url(path))

    def tab_call(self, window, tab, function, *args):
        done = threading.Event()
        result = {}

        def invoke():
            try:
                result["value"] = function(*args)
            except BaseException as exc:
                result["error"] = exc
            finally:
                done.set()

        if not window.schedule_tab_task(
                tab, invoke, priority=self.browser.TaskPriority.INPUT,
                source="title-oracle-probe"):
            raise AssertionError("could not schedule tab-owned probe")
        wait_for(done.is_set, "tab task")
        if "error" in result:
            raise result["error"]
        return result.get("value")

    def commit(self, window, tab):
        self.tab_call(window, tab, tab.run_animation_frame)
        window.raster_and_draw()

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

    def wait_heading(self, window, tab, heading):
        wait_for(lambda: self.heading(tab) == heading,
                 "document {}".format(heading))
        self.commit(window, tab)

    def open_window(self, path, heading):
        window = self.app.new_window(self.url(path))
        self.wait_heading(window, window.tabs[0], heading)
        return window

    def load(self, window, tab, url, heading):
        window.schedule_load(url, tab=tab)
        self.wait_heading(window, tab, heading)

    def sdl_title(self, window):
        sdl2 = self.browser.sdl2
        return sdl2.SDL_GetWindowTitle(window.sdl_window).decode()

    def state(self, window):
        tab = window.active_tab_snapshot()
        committed = window.committed_states.get(tab)
        return {
            "get_title": self.tab_call(window, tab, tab.get_title),
            "committed_title": committed.title if committed else None,
            "sdl_title": self.sdl_title(window),
            "heading": self.heading(tab),
            "url": self.server.normalize(str(tab.url)),
        }


# ---------------------------------------------------------------------------
# Scenarios. Each receives a fresh Probe and returns named checkpoints.


def markup(probe):
    """Questions 1-5: which <title> wins and how its text is cleaned."""
    window = probe.open_window("/home", "home")
    tab = window.tabs[0]
    steps = {}
    for path, (_markup, heading) in title_fixture.MARKUP_PAGES.items():
        probe.load(window, tab, probe.url(path), heading)
        steps[path] = probe.state(window)
    return steps


def error_pages(probe):
    """Question 6: Network Error has no <title>; Certificate Error has one."""
    window = probe.open_window("/a", "page-a")
    tab = window.tabs[0]
    probe.load(window, tab, probe.url("/fail"), "Network Error")
    steps = {"network_error": probe.state(window)}
    probe.load(window, tab, probe.url("/a"), "page-a")
    probe.load(window, tab,
               probe.browser.URL(probe.https.untrusted_url("/secure-home")),
               "Certificate Error")
    state = probe.state(window)
    state["url"] = probe.https.normalize(state["url"])
    steps["certificate_error"] = state
    return steps


def bookmarks(probe):
    """Question 7: the internal bookmarks page."""
    window = probe.open_window("/a", "page-a")
    tab = window.tabs[0]
    probe.load(window, tab, probe.browser.URL("about:bookmarks"), "Bookmarks")
    return {"about_bookmarks": probe.state(window)}


def pending(probe):
    """Question 8: the old title stays until the pending page commits."""
    server = probe.server
    window = probe.open_window("/a", "page-a")
    tab = window.tabs[0]
    window.schedule_load(probe.url("/delay"), tab=tab)
    server.wait_seen("/delay")
    probe.commit(window, tab)
    steps = {"pending": probe.state(window)}
    server.release("/delay")
    probe.wait_heading(window, tab, "delay")
    steps["released"] = probe.state(window)
    return steps


def tabs(probe):
    """Question 9: the window shows the active tab's title only."""
    server = probe.server
    window = probe.open_window("/a", "page-a")
    first = window.tabs[0]
    second = window.new_tab(probe.url("/b"))
    probe.wait_heading(window, second, "page-b")
    steps = {"second_active": probe.state(window)}
    window.set_active_tab(first)
    probe.commit(window, first)
    steps["first_active"] = probe.state(window)
    window.set_active_tab(second)
    probe.commit(window, second)
    steps["second_again"] = probe.state(window)

    # A background load finishing in the inactive first tab.
    window.schedule_load(probe.url("/delay"), tab=first)
    server.wait_seen("/delay")
    server.release("/delay")
    wait_for(lambda: probe.heading(first) == "delay", "background load")
    probe.commit(window, first)
    probe.commit(window, second)
    state = probe.state(window)
    state["inactive_committed_title"] = window.committed_states[first].title
    steps["inactive_loaded"] = state
    window.set_active_tab(first)
    probe.commit(window, first)
    steps["switched_to_loaded"] = probe.state(window)
    return steps


def windows(probe):
    """Question 10: each window follows its own active tab."""
    first = probe.open_window("/a", "page-a")
    second = probe.open_window("/b", "page-b")
    steps = {"opened": [probe.state(first), probe.state(second)]}
    probe.load(second, second.tabs[0], probe.url("/c"), "page-c")
    probe.commit(first, first.tabs[0])
    steps["second_navigated"] = [probe.state(first), probe.state(second)]
    return steps


def fresh_window(probe):
    """Question 11: the title before the first page commits."""
    server = probe.server
    window = probe.app.new_window(probe.url("/delay"))
    server.wait_seen("/delay")
    steps = {"created": {"sdl_title": probe.sdl_title(window),
                         "committed": len(window.committed_states)}}
    window.raster_and_draw()
    steps["presented_before_commit"] = {
        "sdl_title": probe.sdl_title(window),
        "committed": len(window.committed_states)}
    server.release("/delay")
    probe.wait_heading(window, window.tabs[0], "delay")
    steps["committed"] = probe.state(window)
    return steps


def dom_change(probe):
    """Question 12: a script rewrites the title text through innerHTML."""
    window = probe.open_window("/a", "page-a")
    tab = window.tabs[0]
    probe.load(window, tab, probe.url("/retitle"), "retitle")
    wait_for(lambda: probe.tab_call(window, tab, tab.get_title)
             == "After script", "script title", timeout=4)
    probe.commit(window, tab)
    return {"after_script": probe.state(window)}


SCENARIOS = (
    ("markup", markup),
    ("error_pages", error_pages),
    ("bookmarks", bookmarks),
    ("pending", pending),
    ("tabs", tabs),
    ("windows", windows),
    ("fresh_window", fresh_window),
    ("dom_change", dom_change),
)


def run_probe():
    original_cwd = pathlib.Path.cwd()
    server = https = trace_path = None
    with tempfile.TemporaryDirectory(prefix="tai-title-oracle-") as directory:
        try:
            material = https_fixture.make_material(pathlib.Path(directory))
            https = https_fixture.FixtureServers(material)
            browser, trace_path = import_reference()
            result = {}
            for name, scenario in SCENARIOS:
                server = title_fixture.TitleServer()
                probe = Probe(browser, server, https)
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
            if https is not None:
                https.close()
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
            raise SystemExit("Python title oracle differs from {}".format(
                FIXTURE))
        print("Python title oracle probe matches "
              "tests/fixtures/title_oracle.json")
    else:
        print(json.dumps(result, ensure_ascii=False, indent=2, sort_keys=True))


if __name__ == "__main__":
    main()
