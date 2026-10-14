#!/usr/bin/env python3
"""Repeatable Python oracle probe for the trace vocabulary (MeasureTime).

The frozen Python browser runs its real BrowserApp.run() loop on an Xvfb
display with BROWSER_TRACE_FILE set, against tests/fixtures/trace_window
served from 127.0.0.1. The page uses an inline style, a stylesheet, an inline
script (which Python ignores, D6), an external script with setTimeout,
setInterval, asynchronous XHR and requestAnimationFrame (D5: the 7d536e0^
scheduling runtime is evaluated after runtime.js, as in js_page_oracle_probe),
and a click listener that sends a synchronous XHR. The probe waits for the
page's /loaded.txt marker, pushes an SDL click on the heading and then
SDL_QUIT, so MeasureTime.finish() closes the trace. The result is the set of
duration-event names per thread role; tid values, timestamps and instant
events (scheduler and frame-clock diagnostics, timing-dependent) are dropped.

    python3 tests/trace_oracle_probe.py                       # print the result
    python3 tests/trace_oracle_probe.py --check [TEST_TRACE]  # compare with fixture

With TEST_TRACE, --check also asserts that every native name printed by
`test_trace --vocabulary` occurs in the frozen oracle trace with the same
phase family, so one name never means two kinds of work.
"""

import argparse
import contextlib
import ctypes
import functools
import http.server
import importlib.util
import io
import json
import os
import pathlib
import re
import subprocess
import sys
import tempfile
import threading
import time


ROOT = pathlib.Path(__file__).resolve().parents[1]
REFERENCE = ROOT / "tests" / "reference"
FIXTURE = ROOT / "tests" / "fixtures" / "trace_oracle.json"
PAGE = ROOT / "tests" / "fixtures" / "trace_window"
SCHEDULING_JS = ROOT / "tests" / "fixtures" / "scheduling_runtime_7d536e0.js"

sys.path.insert(0, str(ROOT / "tests"))
import trace_check  # noqa: E402


def wait_for(predicate, label, timeout=15):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if predicate():
            return
        time.sleep(0.01)
    raise AssertionError("timed out waiting for {}".format(label))


class PageServer:
    """Serves the fixture directory on 127.0.0.1 and records request paths."""

    def __init__(self):
        self.paths = []
        server = self

        class Handler(http.server.SimpleHTTPRequestHandler):
            def log_message(self, *_args):
                server.paths.append(self.path)

        handler = functools.partial(Handler, directory=str(PAGE))
        self.httpd = http.server.ThreadingHTTPServer(("127.0.0.1", 0), handler)
        self.thread = threading.Thread(target=self.httpd.serve_forever, daemon=True)
        self.thread.start()

    def url(self, path):
        return "http://127.0.0.1:{}{}".format(self.httpd.server_address[1], path)

    def close(self):
        self.httpd.shutdown()
        self.httpd.server_close()


def display_ready(display):
    return subprocess.run(["xset", "-display", display, "q"],
                          stdout=subprocess.DEVNULL,
                          stderr=subprocess.DEVNULL).returncode == 0


@contextlib.contextmanager
def xvfb():
    """A private Xvfb. WSLg mounts /tmp/.X11-unix read-only, so Xvfb may
    listen only on the abstract socket; readiness is probed with xset."""
    for number in range(90, 200):
        if not (os.path.exists("/tmp/.X11-unix/X{}".format(number)) or
                os.path.exists("/tmp/.X{}-lock".format(number))):
            break
    else:
        raise AssertionError("no free X display")
    display = ":{}".format(number)
    process = subprocess.Popen(
        ["Xvfb", display, "-screen", "0", "1280x800x24", "-nolisten", "tcp"],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        wait_for(lambda: process.poll() is None and display_ready(display),
                 "Xvfb " + display, timeout=10)
        yield display
    finally:
        process.terminate()
        process.wait(timeout=10)


def import_reference(trace_path):
    import oracle

    oracle.verify_reference()
    os.environ["SDL_VIDEODRIVER"] = "x11"
    os.environ["BROWSER_RENDER_BACKEND"] = "cpu"
    os.environ["BROWSER_TRACE_FILE"] = str(trace_path)
    os.chdir(REFERENCE)
    spec = importlib.util.spec_from_file_location(
        "tai_trace_fixed_reference", REFERENCE / "browser.py")
    browser = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(browser)
    return browser


def heading_point(browser, window):
    """Window coordinates of the middle of the heading's first text box."""
    tab = window.tabs[0]
    result = {}
    done = threading.Event()

    def locate():
        try:
            stack = [tab.document]
            while stack:
                obj = stack.pop()
                node = getattr(obj, "node", None)
                if (isinstance(node, browser.Text) and node.parent is not None
                        and node.parent.attributes.get("id") == "sync"):
                    result["point"] = (int(obj.x + obj.width / 2),
                                       int(obj.y + obj.height / 2 - tab.scroll))
                    return
                stack.extend(getattr(obj, "children", None) or [])
        finally:
            done.set()

    window.schedule_tab_task(tab, locate, priority=browser.TaskPriority.INPUT,
                             source="trace-oracle-probe")
    wait_for(done.is_set, "heading layout")
    x, y = result["point"]
    return x, y + int(window.chrome.bottom)


def push_click(browser, window, x, y):
    sdl2 = browser.sdl2
    for kind in (sdl2.SDL_MOUSEBUTTONDOWN, sdl2.SDL_MOUSEBUTTONUP):
        event = sdl2.SDL_Event()
        event.type = kind
        event.button.windowID = window.window_id
        event.button.button = sdl2.SDL_BUTTON_LEFT
        event.button.x = x
        event.button.y = y
        if sdl2.SDL_PushEvent(ctypes.byref(event)) < 0:
            raise AssertionError("SDL_PushEvent failed")


def push_quit(browser):
    sdl2 = browser.sdl2
    event = sdl2.SDL_Event()
    event.type = sdl2.SDL_QUIT
    sdl2.SDL_PushEvent(ctypes.byref(event))


def run_oracle(trace_path):
    original_cwd = pathlib.Path.cwd()
    server = PageServer()
    try:
        with xvfb() as display:
            os.environ["DISPLAY"] = display
            browser = import_reference(trace_path)
            frozen_init = browser.JSContext.__init__
            scheduling = SCHEDULING_JS.read_text(encoding="utf-8")

            def scheduling_init(context, tab):
                frozen_init(context, tab)
                context.evaljs(scheduling)

            browser.JSContext.__init__ = scheduling_init
            app = browser.BrowserApp()
            window = app.new_window(browser.URL(server.url("/index.html")))
            failure = []

            def drive():
                try:
                    wait_for(lambda: "/loaded.txt" in server.paths, "/loaded.txt")
                    x, y = heading_point(browser, window)
                    push_click(browser, window, x, y)
                    wait_for(lambda: "/sync.txt" in server.paths, "/sync.txt")
                    time.sleep(0.5)
                except BaseException as error:
                    failure.append(error)
                finally:
                    push_quit(browser)

            driver = threading.Thread(target=drive, daemon=True)
            driver.start()
            app.run()
            driver.join(timeout=20)
            if failure:
                raise failure[0]
    finally:
        os.chdir(original_cwd)
        server.close()


ROLES = [
    (re.compile(r"Main thread - window \d+ tab \d+$"), "Main thread"),
    (re.compile(r"Network I/O worker \d+$"), "Network I/O worker"),
]
# The probe's own server thread and layout lookup task are not the browser's.
PROBE_THREAD = re.compile(r".*\(serve_forever\)$")
PROBE_TASK = "Task: heading_point.<locals>.locate"


def role(name):
    for pattern, replacement in ROLES:
        if pattern.match(name):
            return replacement
    return name


def summarize(events):
    names = trace_check.check(events)
    threads = {role(name): set() for name in names.values()
               if not PROBE_THREAD.match(name)}
    for event in events[1:]:
        if event["ph"] in ("B", "E") and event["name"] != PROBE_TASK:
            threads[role(names[event["tid"]])].add(event["name"])
    return {"threads": {thread: sorted(spans) for thread, spans in threads.items()}}


def run_probe():
    with tempfile.TemporaryDirectory(prefix="tai-trace-oracle-") as directory:
        trace_path = pathlib.Path(directory) / "oracle.trace"
        with contextlib.redirect_stdout(io.StringIO()):
            run_oracle(trace_path)
        return summarize(trace_check.load(trace_path))


def check_vocabulary(result, test_trace):
    """Every native name must name oracle duration work (D-T4 changes only
    the phase of fetch slices, not their meaning)."""
    oracle_names = {name for spans in result["threads"].values() for name in spans}
    spans, fetches = trace_check.vocabulary(test_trace)
    missing = sorted(spans - oracle_names)
    missing += [pattern.pattern for pattern in fetches
                if not any(pattern.match(name) for name in oracle_names)]
    if missing:
        raise SystemExit("native trace names the oracle never emits: {}".format(missing))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true",
                        help="compare the probe output with {}".format(FIXTURE))
    parser.add_argument("test_trace", nargs="?",
                        help="test_trace binary whose --vocabulary is checked")
    args = parser.parse_args()
    result = run_probe()
    if args.check:
        expected = json.loads(FIXTURE.read_text(encoding="utf-8"))
        if result != expected:
            print(json.dumps(result, indent=2, sort_keys=True), file=sys.stderr)
            raise SystemExit("Python trace oracle differs from {}".format(FIXTURE))
        if args.test_trace:
            check_vocabulary(expected, args.test_trace)
        print("Python trace oracle probe matches tests/fixtures/trace_oracle.json")
    else:
        print(json.dumps(result, indent=2, sort_keys=True))


if __name__ == "__main__":
    main()
