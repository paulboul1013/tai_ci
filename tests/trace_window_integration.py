#!/usr/bin/env python3
"""Real-window check of the native trace (BROWSER_TRACE_FILE, D-T1..D-T8).

Drives `tai-browser --window` with tests/tools/window_session.sh on a private
Xvfb display against tests/fixtures/trace_window served from 127.0.0.1: the
page's timers, asynchronous XHR and animation frames run until it requests
/loaded.txt, a click on the heading sends a synchronous XHR, and closing the
window ends the browser. The trace must pass tests/trace_check.py, use only
the two native threads, use only names that test_trace --vocabulary lists and
that the frozen oracle trace (tests/fixtures/trace_oracle.json) emits, and
nest JavaScript inside the tasks that run it. Without BROWSER_TRACE_FILE, or
with it empty, no file appears; an unwritable path exits 1.

A sanitizer build of tai-browser leaks only through the known fontconfig and
cairo global caches and SDL's Xvfb XRandR probe, which are suppressed for
LeakSanitizer.

    python3 tests/trace_window_integration.py TAI_BROWSER TEST_TRACE
"""

import json
import os
import pathlib
import socket
import subprocess
import sys
import tempfile
import time


ROOT = pathlib.Path(__file__).resolve().parents[1]
SESSION = ROOT / "tests" / "tools" / "window_session.sh"
PAGE = ROOT / "tests" / "fixtures" / "trace_window"
ORACLE = ROOT / "tests" / "fixtures" / "trace_oracle.json"
# Window coordinates of the heading's text in the 800x600 window.
HEADING = (100, 101)

sys.path.insert(0, str(ROOT / "tests"))
import trace_check  # noqa: E402

REQUIRED = {
    "Loader thread": {
        "javascript", "render", "Task: Tab.load",
        "Task: Tab._finish_document_load", "Task: Tab._finish_page_resources",
        "Task: JSContext.run", "Network I/O: document",
        "resource_fetch[1]: style", "resource_fetch[2]: script",
        "Network I/O: asynchronous XHR",
    },
    "Browser thread": {
        "javascript", "render", "Task: Tab.run_animation_frame",
        "Task: JSContext.dispatch_settimeout",
        "Task: JSContext.dispatch_setinterval",
        "Task: JSContext.dispatch_xhr_onload", "Task: Tab.click",
        "Network I/O: synchronous XHR", "raster_and_draw", "sdl_present",
    },
}
# Each outer span must directly or indirectly contain the inner one.
CONTAINS = [
    ("Task: JSContext.dispatch_settimeout", "javascript"),
    ("Task: JSContext.dispatch_setinterval", "javascript"),
    ("Task: JSContext.dispatch_xhr_onload", "javascript"),
    ("Task: Tab.run_animation_frame", "javascript"),
    ("Task: JSContext.run", "javascript"),
    ("Task: Tab.click", "Network I/O: synchronous XHR"),
    ("Task: Tab._finish_page_resources", "Task: JSContext.run"),
]


def free_port():
    with socket.socket() as probe:
        probe.bind(("127.0.0.1", 0))
        return probe.getsockname()[1]


class Session:
    """One window_session.sh session in its own directory."""

    def __init__(self, browser, directory, cwd, env):
        self.browser = browser
        self.directory = directory
        self.cwd = cwd
        # A None value removes the variable from the inherited environment.
        merged = dict(os.environ, TAI_WS_DIR=str(directory), **env)
        self.env = {key: value for key, value in merged.items() if value is not None}
        self.started = False

    def run(self, *args, check=True):
        result = subprocess.run([str(SESSION), *args], cwd=self.cwd, env=self.env,
                                capture_output=True, text=True, timeout=120)
        if check and result.returncode != 0:
            raise AssertionError("window_session {} failed:\n{}{}".format(
                " ".join(args), result.stdout, result.stderr))
        return result.stdout

    def start(self):
        port = free_port()
        self.started = True
        self.run("start", "--xvfb", "--port", str(port), "--fixture", str(PAGE),
                 "--bin", str(self.browser),
                 "--url", "http://127.0.0.1:{}/index.html".format(port))

    def wait_request(self, path, timeout=60):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if '"GET {}" 200'.format(path) in self.run("requests"):
                return
            time.sleep(0.2)
        raise AssertionError("browser never requested " + path)

    def stop(self):
        """Close the only window, then collect the exit status."""
        self.run("close")
        output = self.run("stop", check=False)
        self.started = False
        if "browser exit=0" not in output:
            log = (self.directory / "browser.log").read_text(errors="replace")
            raise AssertionError("browser did not exit cleanly:\n{}\n{}".format(
                output, log[-4000:]))

    def abandon(self):
        if self.started:
            self.run("stop", check=False)


def sanitizer_env(directory):
    suppressions = directory / "lsan.supp"
    # SDL's XRandR probe on Xvfb leaks 8 bytes from a dlopen'd libXrandr
    # during SDL_InitVideo, before any browser code runs.
    suppressions.write_text("leak:libfontconfig.so\nleak:libcairo.so\n"
                            "leak:X11_InitModes_XRandR\n")
    return {"ASAN_OPTIONS": "detect_leaks=1",
            "LSAN_OPTIONS": "suppressions={}".format(suppressions)}


def span_children(events):
    """Per span name, the names of every span opened while it was open on the
    same thread (B/E only)."""
    inside = {}
    stacks = {}
    for event in events[1:]:
        if event["ph"] == "B":
            stack = stacks.setdefault(event["tid"], [])
            for outer in stack:
                inside.setdefault(outer, set()).add(event["name"])
            stack.append(event["name"])
        elif event["ph"] == "E":
            stacks[event["tid"]].pop()
    return inside


def check_trace(path, test_trace):
    events = trace_check.load(path)
    names = trace_check.check(events)
    assert sorted(names.values()) == ["Browser thread", "Loader thread"], names
    spans, fetches = trace_check.vocabulary(test_trace)
    oracle = {name for thread in json.loads(ORACLE.read_text())["threads"].values()
              for name in thread}
    seen = {}
    for event in events[1:]:
        if event["ph"] == "M":
            continue
        name = event["name"]
        if event["ph"] in "BE":
            assert name in spans, "span outside the vocabulary: " + name
        else:
            assert any(p.match(name) for p in fetches), "fetch outside the vocabulary: " + name
        assert name in oracle, "name the oracle never emits: " + name
        seen.setdefault(names[event["tid"]], set()).add(name)
    for thread, required in REQUIRED.items():
        missing = required - seen.get(thread, set())
        assert not missing, "{} lacks {}".format(thread, sorted(missing))
    inside = span_children(events)
    for outer, inner in CONTAINS:
        assert inner in inside.get(outer, set()), "{} never contains {}".format(outer, inner)
    return len(events)


def traced_run(browser, test_trace, root, env):
    directory = root / "traced"
    directory.mkdir()
    trace = directory / "t.json"
    session = Session(browser, directory, root, dict(env, BROWSER_TRACE_FILE=str(trace)))
    try:
        session.start()
        session.wait_request("/loaded.txt")
        session.run("click", *map(str, HEADING))
        session.wait_request("/sync.txt")
        session.stop()
    finally:
        session.abandon()
    return check_trace(trace, test_trace)


def untraced_run(browser, root, env, label, value):
    """The browser runs in an empty directory and must leave it empty."""
    directory = root / label
    cwd = root / (label + "-cwd")
    directory.mkdir()
    cwd.mkdir()
    session = Session(browser, directory, cwd, dict(env, BROWSER_TRACE_FILE=value))
    try:
        session.start()
        session.wait_request("/loaded.txt")
        session.stop()
    finally:
        session.abandon()
    leftovers = sorted(path.name for path in cwd.iterdir())
    assert not leftovers, "{} run wrote {}".format(label, leftovers)


def unwritable_run(browser, env):
    result = subprocess.run(
        [str(browser), "--window", "about:blank"], capture_output=True, text=True,
        timeout=60, env=dict(os.environ, **env,
                             BROWSER_TRACE_FILE="/nonexistent-tai-trace/t.json"))
    assert result.returncode == 1, (result.returncode, result.stderr)
    assert "trace failed: /nonexistent-tai-trace/t.json:" in result.stderr, result.stderr


def main(argv):
    if len(argv) != 3:
        raise SystemExit(__doc__)
    browser, test_trace = (pathlib.Path(arg).resolve() for arg in argv[1:])
    with tempfile.TemporaryDirectory(prefix="tai-trace-window-") as name:
        root = pathlib.Path(name)
        env = sanitizer_env(root)
        count = traced_run(browser, test_trace, root, env)
        untraced_run(browser, root, env, "unset", None)
        untraced_run(browser, root, env, "empty", "")
        unwritable_run(browser, env)
    print("trace_window_integration passed: {} events".format(count))


if __name__ == "__main__":
    main(sys.argv)
