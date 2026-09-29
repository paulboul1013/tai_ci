#!/usr/bin/env python3
"""Repeatable Python oracle probe for the new-window (Ctrl+N) work item.

The frozen Python browser runs with SDL's dummy video driver against the
127.0.0.1 server in tests/new_window_fixture.py. Every scenario starts a fresh
BrowserApp and drives it with synthetic SDL events through
BrowserApp.dispatch_event, the same routing the real event loop uses.
BrowserWindow.handle_new_window always asks for https://browser.engineering/;
the probe records that request and loads the fixture's /home page instead so
no scenario leaves the machine. Dynamic ports are replaced with <PORT>.

    python3 tests/new_window_oracle_probe.py            # print the result
    python3 tests/new_window_oracle_probe.py --check    # compare with fixture
"""

import contextlib
import ctypes
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
FIXTURE = ROOT / "tests" / "fixtures" / "new_window_oracle.json"

sys.path.insert(0, str(ROOT / "tests"))
import new_window_fixture  # noqa: E402


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
    descriptor, trace_path = tempfile.mkstemp(prefix="tai-new-window-oracle-",
                                              suffix=".trace")
    os.close(descriptor)
    os.environ["BROWSER_TRACE_FILE"] = trace_path

    source = REFERENCE / "browser.py"
    os.chdir(REFERENCE)
    spec = importlib.util.spec_from_file_location(
        "tai_new_window_fixed_reference", source)
    browser = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(browser)
    return browser, pathlib.Path(trace_path)


class Probe:
    """One BrowserApp whose new_window() loads the fixture home page."""

    def __init__(self, browser, server):
        self.browser = browser
        self.server = server
        browser.COOKIE_JAR.clear()
        self.app = browser.BrowserApp()
        self.requested_new_window_urls = []
        original = self.app.new_window

        def new_window(url=None):
            self.requested_new_window_urls.append(
                None if url is None else str(url))
            return original(browser.URL(server.url("/home")))

        self.app.new_window = new_window
        self.original_new_window = original
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

    # -- windows and tabs --------------------------------------------------

    def open_first(self, path, heading):
        window = self.original_new_window(
            self.browser.URL(self.server.url(path)))
        self.wait_heading(window, window.tabs[0], heading)
        return window

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
                source="new-window-oracle-probe"):
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

    def load(self, window, path, heading):
        tab = window.active_tab_snapshot()
        window.schedule_load(self.browser.URL(self.server.url(path)), tab=tab)
        self.wait_heading(window, tab, heading)

    def newest_window(self):
        return self.app.windows[-1]

    def wait_new_window(self, count):
        wait_for(lambda: len(self.app.windows) == count, "new window")
        window = self.newest_window()
        self.wait_heading(window, window.tabs[0], "home")
        return window

    # -- synthetic SDL events ----------------------------------------------

    def dispatch(self, event):
        self.app.dispatch_event(event)

    def key(self, window_id, sym, mod=0, repeat=0):
        sdl2 = self.browser.sdl2
        event = sdl2.SDL_Event()
        event.type = sdl2.SDL_KEYDOWN
        event.key.windowID = window_id
        event.key.repeat = repeat
        event.key.keysym.sym = sym
        event.key.keysym.mod = mod
        self.dispatch(event)

    def ctrl_n(self, window, mod=None, repeat=0):
        sdl2 = self.browser.sdl2
        self.key(window.window_id if hasattr(window, "window_id") else window,
                 sdl2.SDLK_n, sdl2.KMOD_LCTRL if mod is None else mod, repeat)

    def text(self, window, text):
        sdl2 = self.browser.sdl2
        event = sdl2.SDL_Event()
        event.type = sdl2.SDL_TEXTINPUT
        event.text.windowID = window.window_id
        encoded = text.encode()
        ctypes.memmove(ctypes.addressof(event.text) +
                       type(event.text).text.offset, encoded, len(encoded))
        self.dispatch(event)

    def wheel(self, window, y):
        sdl2 = self.browser.sdl2
        event = sdl2.SDL_Event()
        event.type = sdl2.SDL_MOUSEWHEEL
        event.wheel.windowID = window.window_id
        event.wheel.y = y
        self.dispatch(event)

    def click(self, window, x, y):
        sdl2 = self.browser.sdl2
        event = sdl2.SDL_Event()
        event.type = sdl2.SDL_MOUSEBUTTONUP
        event.button.windowID = window.window_id
        event.button.button = sdl2.SDL_BUTTON_LEFT
        event.button.x = int(x)
        event.button.y = int(y)
        self.dispatch(event)

    def close_event(self, window):
        sdl2 = self.browser.sdl2
        event = sdl2.SDL_Event()
        event.type = sdl2.SDL_WINDOWEVENT
        event.window.windowID = window.window_id
        event.window.event = sdl2.SDL_WINDOWEVENT_CLOSE
        self.dispatch(event)

    def quit_event(self):
        sdl2 = self.browser.sdl2
        event = sdl2.SDL_Event()
        event.type = sdl2.SDL_QUIT
        self.dispatch(event)

    # -- page geometry -------------------------------------------------------

    def page_link_point(self, window, tab, href):
        browser = self.browser

        def locate():
            for obj in browser.tree_to_list(tab.document, []):
                if getattr(obj, "width", None) is None or obj.width <= 0:
                    continue
                ancestor = getattr(obj, "node", None)
                while ancestor is not None:
                    if (isinstance(ancestor, browser.Element)
                            and ancestor.tag == "a"
                            and ancestor.attributes.get("href") == href):
                        return (obj.x + obj.width / 2,
                                obj.y + obj.height / 2 - tab.scroll)
                    ancestor = ancestor.parent
            return None

        point = self.tab_call(window, tab, locate)
        if point is None:
            raise AssertionError("page link {} not found".format(href))
        return point[0], point[1] + window.chrome.bottom

    # -- observation ---------------------------------------------------------

    def set_draft(self, window, text, focused):
        chrome = window.chrome
        chrome.focus = "address bar" if focused else None
        chrome.address_bar = text
        chrome.address_bar_cursor = len(text)
        chrome.address_bar_dirty = bool(text)

    def window_state(self, window):
        sdl2 = self.browser.sdl2
        normalize = self.server.normalize
        chrome = window.chrome
        tab = window.active_tab_snapshot()
        tabs = window.tabs_snapshot()
        width, height = ctypes.c_int(), ctypes.c_int()
        sdl2.SDL_GetWindowSize(window.sdl_window, ctypes.byref(width),
                               ctypes.byref(height))
        return {
            "title": sdl2.SDL_GetWindowTitle(window.sdl_window).decode(),
            "size": [width.value, height.value],
            "tab_count": len(tabs),
            "active_index": tabs.index(tab),
            "url": normalize(str(tab.url)),
            "heading": self.heading(tab),
            "history": [normalize(str(url)) for url in tab.history],
            "history_index": tab.history_index,
            "scroll": round(float(tab.scroll), 3),
            "address": normalize(chrome.address_bar_display_text()),
            "address_focused": chrome.focus == "address bar",
            "address_dirty": bool(chrome.address_bar_dirty),
            "bookmarked": bool(window.is_current_page_bookmarked()),
        }

    def app_state(self):
        return {
            "window_count": len(self.app.windows),
            "running": bool(self.app.running),
            "windows": [self.window_state(window)
                        for window in self.app.windows],
        }

    def requested(self):
        return list(self.requested_new_window_urls)


# ---------------------------------------------------------------------------
# Scenarios. Each receives a fresh Probe and returns named checkpoints.


def ctrl_n_basic(probe):
    first = probe.open_first("/a", "page-a")
    probe.ctrl_n(first)
    probe.wait_new_window(2)
    return {
        "requested_urls": probe.requested(),
        "after": probe.app_state(),
    }


def original_unaffected(probe):
    first = probe.open_first("/a", "page-a")
    probe.load(first, "/b", "page-b")
    tab = first.new_tab(probe.browser.URL(probe.server.url("/c")))
    probe.wait_heading(first, tab, "page-c")
    probe.set_draft(first, "typed draft", True)
    before = probe.window_state(first)
    probe.ctrl_n(first)
    probe.wait_new_window(2)
    probe.commit(first, first.active_tab_snapshot())
    return {"before": before, "after": probe.app_state()}


def modifiers(probe):
    sdl2 = probe.browser.sdl2
    first = probe.open_first("/a", "page-a")
    steps = {}
    probe.key(first.window_id, sdl2.SDLK_n, 0)
    steps["plain_n"] = len(probe.app.windows)
    cases = (
        ("right_ctrl", sdl2.KMOD_RCTRL, 0),
        ("ctrl_shift", sdl2.KMOD_LCTRL | sdl2.KMOD_LSHIFT, 0),
        ("ctrl_alt", sdl2.KMOD_LCTRL | sdl2.KMOD_LALT, 0),
        ("key_repeat", sdl2.KMOD_LCTRL, 1),
    )
    for name, mod, repeat in cases:
        count = len(probe.app.windows)
        probe.ctrl_n(first, mod, repeat)
        if len(probe.app.windows) != count:
            probe.wait_new_window(count + 1)
        steps[name] = len(probe.app.windows)
    probe.key(first.window_id, sdl2.SDLK_m, sdl2.KMOD_LCTRL)
    steps["ctrl_m"] = len(probe.app.windows)
    return steps


def routing(probe):
    sdl2 = probe.browser.sdl2
    server = probe.server
    first = probe.open_first("/links", "links")
    probe.set_draft(first, "first draft", True)
    probe.ctrl_n(first)
    second = probe.wait_new_window(2)
    steps = {}

    # Text and Enter go only to the window named by the event.
    probe.set_draft(second, "", True)
    probe.text(second, server.url("/b"))
    steps["typed"] = probe.app_state()
    probe.key(second.window_id, sdl2.SDLK_RETURN)
    probe.wait_heading(second, second.active_tab_snapshot(), "page-b")
    probe.commit(first, first.active_tab_snapshot())
    steps["entered"] = probe.app_state()

    # The wheel scrolls only the addressed window.
    tab = second.active_tab_snapshot()
    probe.wheel(second, -1)
    wait_for(lambda: tab.scroll > 0, "second window scroll")
    probe.commit(second, tab)
    steps["wheel"] = probe.app_state()

    # A page click in the first window navigates only the first window.
    probe.set_draft(first, "", False)
    first_tab = first.active_tab_snapshot()
    x, y = probe.page_link_point(first, first_tab, "/b")
    probe.click(first, x, y)
    probe.wait_heading(first, first_tab, "page-b")
    steps["clicked"] = probe.app_state()

    # Events for an unknown window are dropped.
    probe.ctrl_n(9999)
    steps["unknown_window_count"] = len(probe.app.windows)
    return steps


def shared_bookmarks(probe):
    first = probe.open_first("/a", "page-a")
    probe.ctrl_n(first)
    second = probe.wait_new_window(2)
    probe.load(second, "/c", "page-c")
    second.toggle_bookmark()
    steps = {"toggled": probe.app_state()}
    probe.load(first, "/c", "page-c")
    steps["first_on_c"] = probe.app_state()
    tab = first.active_tab_snapshot()
    listing = probe.tab_call(first, tab, tab.bookmarks_page)
    steps["first_list_has_c"] = probe.server.url("/c") in listing
    return steps


def shared_cookies(probe):
    server = probe.server
    first = probe.open_first("/cookie-set", "cookie-set")
    probe.ctrl_n(first)
    second = probe.wait_new_window(2)
    mark = server.mark()
    probe.load(second, "/cookie-check", "cookie-check")
    return {"second_requests": server.since(mark)}


def third_window(probe):
    first = probe.open_first("/a", "page-a")
    probe.ctrl_n(first)
    second = probe.wait_new_window(2)
    probe.load(second, "/b", "page-b")
    probe.ctrl_n(second)
    probe.wait_new_window(3)
    return {"requested_urls": probe.requested(),
            "after": probe.app_state()}


def close_one(probe):
    server = probe.server
    first = probe.open_first("/a", "page-a")
    probe.ctrl_n(first)
    second = probe.wait_new_window(2)
    mark = server.mark()
    first.schedule_load(probe.browser.URL(server.url("/delay")),
                        tab=first.active_tab_snapshot())
    server.wait_seen("/delay", mark)
    probe.close_event(first)
    steps = {"closed_first": probe.app_state()}
    server.release("/delay")
    probe.load(second, "/b", "page-b")
    steps["second_navigates"] = probe.app_state()
    probe.close_event(second)
    steps["closed_last"] = {"window_count": len(probe.app.windows),
                            "running": bool(probe.app.running)}
    return steps


def quit_all(probe):
    first = probe.open_first("/a", "page-a")
    probe.ctrl_n(first)
    probe.wait_new_window(2)
    probe.quit_event()
    # BrowserApp.run() then leaves its loop and closes every window.
    return {"window_count": len(probe.app.windows),
            "running": bool(probe.app.running)}


SCENARIOS = (
    ("ctrl_n_basic", ctrl_n_basic),
    ("original_unaffected", original_unaffected),
    ("modifiers", modifiers),
    ("routing", routing),
    ("shared_bookmarks", shared_bookmarks),
    ("shared_cookies", shared_cookies),
    ("third_window", third_window),
    ("close_one", close_one),
    ("quit_all", quit_all),
)


def run_probe():
    original_cwd = pathlib.Path.cwd()
    server = trace_path = None
    try:
        server = new_window_fixture.NewWindowServer()
        browser, trace_path = import_reference()
        result = {}
        for name, scenario in SCENARIOS:
            probe = Probe(browser, server)
            try:
                result[name] = scenario(probe)
            finally:
                probe.close()
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
            raise SystemExit("Python new-window oracle differs from {}".format(
                FIXTURE))
        print("Python new-window oracle probe matches "
              "tests/fixtures/new_window_oracle.json")
    else:
        print(json.dumps(result, ensure_ascii=False, indent=2, sort_keys=True))


if __name__ == "__main__":
    main()
