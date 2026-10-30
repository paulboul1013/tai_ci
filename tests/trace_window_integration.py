#!/usr/bin/env python3
"""Real-window check of the native trace (BROWSER_TRACE_FILE, D-T1..D-T8).

Drives `tai-browser --window` with tests/tools/window_session.sh on a private
Xvfb display against tests/fixtures/trace_window served from 127.0.0.1: the
page's timers, asynchronous XHR and animation frames run until it requests
/loaded.txt, a click on the heading sends a synchronous XHR, and closing the
window ends the browser. The trace must pass tests/trace_check.py, use only
the two native threads, use only names that test_trace --vocabulary lists and
that the frozen oracle trace (tests/fixtures/trace_oracle.json) emits, and
nest JavaScript inside the tasks that run it. With BROWSER_TRACE_FILE empty no
file appears; unset, window_session.sh writes TAI_WS_DIR/trace.json and the
working directory stays empty; an unwritable path exits 1.

A second traced session bookmarks a page with the address-bar star, opens
about:bookmarks with the list button, follows its link back and types
about:blank (D-T10): about:bookmarks is built in memory, so it has a
Task: Tab.load and a commit but no document fetch and no
_finish_document_load; about:blank takes the document path.

A multi-window session (D-T11) starts with --home pointing at a page of the
same 127.0.0.1 fixture server: window 1 opens /a.html, Ctrl+N opens window 2
on the home page, window 2 navigates to /b.html and closes, and window 1 then
navigates to /c.html. The tab_window instants join each commit and render
(tab arg) to the raster_and_draw/sdl_present spans of its window (window
arg); no window-2 frame follows its close, and no request leaves 127.0.0.1.

Cancelled requests must still close their async slices. A local server
leaves /hang* requests unanswered until the browser has exited, so their `e`
events can only come from the cancel and close paths: navigating away from a
pending document, from a pending stylesheet and from a page with a pending
asynchronous XHR, then closing the window during a load and with an XHR
pending.

A sanitizer build of tai-browser leaks only through the known fontconfig and
cairo global caches and SDL's Xvfb XRandR probe, which are suppressed for
LeakSanitizer.

    python3 tests/trace_window_integration.py TAI_BROWSER TEST_TRACE
"""

import http.server
import json
import os
import pathlib
import socket
import subprocess
import sys
import tempfile
import threading
import time


ROOT = pathlib.Path(__file__).resolve().parents[1]
SESSION = ROOT / "tests" / "tools" / "window_session.sh"
PAGE = ROOT / "tests" / "fixtures" / "trace_window"
BOOKMARK_PAGE = ROOT / "tests" / "fixtures" / "bookmarks_window"
ORACLE = ROOT / "tests" / "fixtures" / "trace_oracle.json"
# Window coordinates of the heading's text in the 800x600 window.
HEADING = (100, 101)
ADDRESS = (400, 63)
STAR = (769, 63)
BOOKMARKS_BUTTON = (110, 54)
# The first link on about:bookmarks.
FIRST_BOOKMARK = (120, 121)

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
        "commit",
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
        return result.stdout if check else result

    def start(self, url=None, fixture=PAGE, page="index.html", home=None):
        """Serve fixture and open its page, or open url from another server.
        home names a fixture page for --home. Return the opened URL."""
        self.started = True
        if url:
            self.run("start", "--xvfb", "--bin", str(self.browser), "--url", url)
            return url
        port = free_port()
        base = "http://127.0.0.1:{}/".format(port)
        url = base + page
        home_args = ["--home", base + home] if home else []
        self.run("start", "--xvfb", "--port", str(port), "--fixture", str(fixture),
                 "--bin", str(self.browser), "--url", url, *home_args)
        return url

    def navigate(self, url):
        self.run("click", *map(str, ADDRESS))
        self.run("key", "End", "BackSpace*80")
        self.run("type", url)
        self.run("key", "Return")

    def wait_request(self, path, timeout=60, count=1):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if self.run("requests").count('"GET {}" 200'.format(path)) >= count:
                return
            time.sleep(0.2)
        raise AssertionError("browser never requested {} {} times".format(path, count))

    def wait_title(self, title, timeout=60):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if "'{}'".format(title) in self.run("title"):
                return
            time.sleep(0.2)
        raise AssertionError("window title never became " + title)

    def stop(self):
        """Close the only window, then collect the exit status."""
        self.run("close")
        result = self.run("stop", check=False)
        if "browser exit=0" not in result.stdout:
            log = (self.directory / "browser.log").read_text(errors="replace")
            raise AssertionError("browser did not exit cleanly (stop rc {}):\n{}{}\n{}".format(
                result.returncode, result.stdout, result.stderr, log[-4000:]))
        self.started = False

    def abandon(self):
        if self.started:
            self.run("stop", check=False)


HANG_PAGES = {
    "/hang.html": None,
    "/hang.css": None,
    "/hang.txt": None,
    "/stall.html": '<link rel="stylesheet" href="/hang.css"><p>stall</p>',
    "/xhr.html": "<p>xhr</p><script>var x = new XMLHttpRequest();"
                 "x.open('GET', '/hang.txt', true); x.send();</script>",
    "/done.html": "<p>done</p>",
}


class HangServer:
    """127.0.0.1 server whose None pages stay unanswered until release()."""

    def __init__(self):
        self.seen = []
        self.lock = threading.Lock()
        self.released = threading.Event()
        server = self

        class Handler(http.server.BaseHTTPRequestHandler):
            def do_GET(self):
                with server.lock:
                    server.seen.append(self.path)
                body = HANG_PAGES.get(self.path, "")
                if body is None:
                    server.released.wait(120)
                    body = ""
                data = body.encode()
                try:
                    self.send_response(200 if self.path in HANG_PAGES else 404)
                    self.send_header("Content-Length", str(len(data)))
                    self.end_headers()
                    self.wfile.write(data)
                except OSError:
                    pass

            def log_message(self, *args):
                pass

        self.httpd = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        self.httpd.daemon_threads = True
        self.thread = threading.Thread(target=self.httpd.serve_forever, daemon=True)
        self.thread.start()

    def url(self, path):
        return "http://127.0.0.1:{}{}".format(self.httpd.server_address[1], path)

    def wait(self, path, timeout=60):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            with self.lock:
                if path in self.seen:
                    return
            time.sleep(0.1)
        raise AssertionError("browser never requested " + path)

    def release(self):
        self.released.set()
        self.httpd.shutdown()
        self.httpd.server_close()


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
    vocab = trace_check.vocabulary(test_trace)
    oracle = {name for thread in json.loads(ORACLE.read_text())["threads"].values()
              for name in thread}
    seen = {}
    for event in events[1:]:
        if event["ph"] == "M":
            continue
        name = event["name"]
        assert trace_check.name_allowed(event, *vocab), "outside the vocabulary: " + name
        assert name in oracle or event["ph"] == "i", "name the oracle never emits: " + name
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


def loads_of(events, url):
    """The Task: Tab.load B events for url."""
    return [event for event in events if event["ph"] == "B"
            and event["name"] == "Task: Tab.load" and event["args"]["url"] == url]


def commit_after(events, load, label):
    """The first commit B for load's tab and generation that starts after it."""
    for event in events:
        if (event["ph"] == "B" and event["name"] == "commit" and event["ts"] >= load["ts"]
                and event["args"]["tab"] == load["args"]["tab"]
                and event["args"]["generation"] == load["args"]["generation"]):
            return event
    raise AssertionError("{}: no commit for {} generation {}".format(
        label, load["args"]["url"], load["args"]["generation"]))


def document_fetches(events, url):
    return [event for event in events if event["ph"] == "b"
            and event["name"] == "Network I/O: document"
            and event.get("args", {}).get("url") == url]


def check_about_trace(path, test_trace, page):
    """B1 about:bookmarks is built in memory: Tab.load and commit but no
    document fetch and no _finish_document_load. B2 its link really
    navigates back to page. B3 about:blank takes the document path."""
    events = trace_check.load(path)
    names = trace_check.check(events)
    vocab = trace_check.vocabulary(test_trace)
    for event in events[1:]:
        if event["ph"] != "M":
            assert trace_check.name_allowed(event, *vocab), \
                "outside the vocabulary: " + event["name"]
    loader = next(tid for tid, name in names.items() if name == "Loader thread")

    bookmarks = loads_of(events, "about:bookmarks")
    assert len(bookmarks) == 1, "B1: {} Tab.load for about:bookmarks".format(len(bookmarks))
    bookmarks = bookmarks[0]
    commit = commit_after(events, bookmarks, "B1")
    assert not document_fetches(events, "about:bookmarks"), \
        "B1: about:bookmarks has a Network I/O: document slice"
    finished = [event for event in events if event["ph"] == "B"
                and event["name"] == "Task: Tab._finish_document_load"
                and event["tid"] == loader
                and bookmarks["ts"] <= event["ts"] <= commit["ts"]]
    assert not finished, "B1: about:bookmarks ran Task: Tab._finish_document_load"

    back = [load for load in loads_of(events, page)
            if load["args"]["generation"] > bookmarks["args"]["generation"]]
    assert back, "B2: no Tab.load for {} after about:bookmarks".format(page)
    back_commit = commit_after(events, back[0], "B2")
    assert any(back[0]["ts"] <= fetch["ts"] <= back_commit["ts"]
               for fetch in document_fetches(events, page)), \
        "B2: no Network I/O: document for {} after about:bookmarks".format(page)

    blank = loads_of(events, "about:blank")
    assert len(blank) == 1, "B3: {} Tab.load for about:blank".format(len(blank))
    blank_commit = commit_after(events, blank[0], "B3")
    assert document_fetches(events, "about:blank"), \
        "B3: about:blank has no Network I/O: document slice"
    assert any(event["ph"] == "B" and event["name"] == "Task: Tab._finish_document_load"
               and blank[0]["ts"] <= event["ts"] <= blank_commit["ts"]
               for event in events), \
        "B3: about:blank never ran Task: Tab._finish_document_load"


def about_run(browser, test_trace, root, env):
    """Bookmark a page with the star, open about:bookmarks with the list
    button, follow its link back, then type about:blank."""
    directory = root / "about"
    directory.mkdir()
    trace = directory / "t.json"
    session = Session(browser, directory, root, dict(env, BROWSER_TRACE_FILE=str(trace)))
    try:
        page = session.start(fixture=BOOKMARK_PAGE)
        session.wait_title("Bookmark Home")
        session.run("click", *map(str, STAR))
        session.run("click", *map(str, BOOKMARKS_BUTTON))
        session.wait_title("Bookmarks")
        session.run("click", *map(str, FIRST_BOOKMARK))
        session.wait_request("/index.html", count=2)
        session.wait_title("Bookmark Home")
        session.navigate("about:blank")
        # about:blank has no <title>, so the window shows the browser name.
        session.wait_title("Tai Ci")
        session.stop()
    finally:
        session.abandon()
    check_about_trace(trace, test_trace, page)


MULTIWINDOW_PAGES = {"a": "Page A", "b": "Page B", "c": "Page C",
                     "home": "Home Page"}


def begins(events, name):
    return [event for event in events if event["ph"] == "B" and event["name"] == name]


def check_multiwindow_trace(path, test_trace, base):
    """M1 commits land on the tab of the expected window, M2 frames carry
    two distinct window IDs, M3 window 2 draws nothing once /c (typed
    after window 2 closed) starts loading while window 1 draws /c, M4 every render joins a window."""
    events = trace_check.load(path)
    trace_check.check(events)
    vocab = trace_check.vocabulary(test_trace)
    for event in events[1:]:
        if event["ph"] != "M":
            assert trace_check.name_allowed(event, *vocab), \
                "outside the vocabulary: " + event["name"]
    joins = [event["args"] for event in events
             if event["ph"] == "i" and event["name"] == "tab_window"]
    tab_window = {join["tab"]: join["window"] for join in joins}
    assert len(tab_window) == len(joins), "tab announced twice: {}".format(joins)
    window_tabs = {}
    for tab, window in tab_window.items():
        window_tabs.setdefault(window, []).append(tab)

    commits = {}
    for event in begins(events, "commit"):
        commits.setdefault(event["args"]["url"], []).append(event)
    url = {name: base + name + ".html" for name in MULTIWINDOW_PAGES}
    for name in MULTIWINDOW_PAGES:
        assert len(commits.get(url[name], [])) == 1, \
            "M1: commits of {}: {}".format(url[name], commits.get(url[name]))
    tab = {name: commits[url[name]][0]["args"]["tab"] for name in MULTIWINDOW_PAGES}
    for name in MULTIWINDOW_PAGES:
        assert tab[name] in tab_window, \
            "M1: tab {} of {} has no tab_window".format(tab[name], url[name])
    window1, window2 = tab_window[tab["a"]], tab_window[tab["home"]]
    assert window1 != window2, "M1: home opened in window 1"
    assert tab["c"] == tab["a"] and tab["b"] == tab["home"], \
        "M1: commits on tabs {}".format(tab)
    assert sorted(window_tabs) == sorted([window1, window2]) and all(
        len(tabs) == 1 for tabs in window_tabs.values()), \
        "M1: join table {}".format(tab_window)

    frames = begins(events, "raster_and_draw") + begins(events, "sdl_present")
    missing = [event["name"] for event in frames if "window" not in event.get("args", {})]
    assert not missing, "M2: {} frame spans without a window arg".format(len(missing))
    assert {event["args"]["window"] for event in frames} == {window1, window2}, \
        "M2: frame windows {}".format({event["args"]["window"] for event in frames})

    # /c.html is typed only after window 2 has gone, so its load marks the close.
    load_c = [event for event in begins(events, "Task: Tab.load")
              if event["args"]["url"] == url["c"]]
    assert len(load_c) == 1, "M3: {} loads of {}".format(len(load_c), url["c"])
    late = [event for event in frames if event["args"]["window"] == window2
            and event["ts"] >= load_c[0]["ts"]]
    assert not late, "M3: {} window-2 frames after its close".format(len(late))
    commit_c = commits[url["c"]][0]
    assert any(event["args"]["window"] == window1 and event["ts"] > commit_c["ts"]
               for event in begins(events, "sdl_present")), \
        "M3: window 1 never presented after committing /c.html"

    renders = begins(events, "render")
    untagged = [event for event in renders if "tab" not in event.get("args", {})]
    assert not untagged, "M4: {} render spans without a tab arg".format(len(untagged))
    unjoined = sorted({event["args"]["tab"] for event in renders} - set(tab_window))
    assert not unjoined, "M4: render tabs {} join no window".format(unjoined)
    assert {event["args"]["tab"] for event in renders} == set(tab_window), \
        "M4: not every tab rendered"

    fetched = [event["args"]["url"] for event in events
               if event["ph"] == "b" and "url" in event.get("args", {})]
    outside = [address for address in fetched if not address.startswith(base)]
    assert fetched and not outside, "requests left 127.0.0.1: {}".format(outside)


def multiwindow_run(browser, test_trace, root, env):
    """Window 1 opens /a.html; Ctrl+N opens window 2 on --home; window 2
    goes to /b.html and closes; window 1 goes to /c.html."""
    directory = root / "multiwindow"
    fixture = root / "multiwindow-pages"
    directory.mkdir()
    fixture.mkdir()
    for name, title in MULTIWINDOW_PAGES.items():
        (fixture / (name + ".html")).write_text(
            "<title>{0}</title><p>{0}</p>".format(title))
    trace = directory / "t.json"
    session = Session(browser, directory, root, dict(env, BROWSER_TRACE_FILE=str(trace)))
    try:
        first = session.start(fixture=fixture, page="a.html", home="home.html")
        base = first[:-len("a.html")]
        session.wait_title(MULTIWINDOW_PAGES["a"])
        session.run("key", "ctrl+n")
        session.run("await", "2")
        session.run("select", "2")
        session.wait_title(MULTIWINDOW_PAGES["home"])
        session.navigate(base + "b.html")
        session.wait_title(MULTIWINDOW_PAGES["b"])
        session.run("close")
        session.run("await", "1")
        session.run("select", "1")
        session.navigate(base + "c.html")
        session.wait_title(MULTIWINDOW_PAGES["c"])
        requests = session.run("requests")
        session.stop()
    finally:
        session.abandon()
    for name in MULTIWINDOW_PAGES:
        assert '"GET /{}.html" 200'.format(name) in requests, \
            "fixture server never served /{}.html:\n{}".format(name, requests)
    check_multiwindow_trace(trace, test_trace, base)


def async_pairs(path):
    """Validate the trace; return {async name: number of b/e pairs}."""
    events = trace_check.load(path)
    trace_check.check(events)
    pairs = {}
    for event in events[1:]:
        if event["ph"] == "e":
            pairs[event["name"]] = pairs.get(event["name"], 0) + 1
    return pairs


def cancel_run(browser, root, env, label, steps, expected):
    """Run steps against a HangServer, stop the browser before any /hang*
    request is answered, and compare the closed async slices."""
    directory = root / label
    directory.mkdir()
    trace = directory / "t.json"
    server = HangServer()
    session = Session(browser, directory, root, dict(env, BROWSER_TRACE_FILE=str(trace)))
    try:
        session.start(server.url(steps[0][0]))
        for page, waits in steps:
            if page != steps[0][0]:
                session.navigate(server.url(page))
            for path in waits:
                server.wait(path)
        session.stop()
    finally:
        session.abandon()
        server.release()
    pairs = async_pairs(trace)
    assert pairs == expected, "{}: closed async slices {} != {}".format(label, pairs, expected)


def cancel_runs(browser, root, env):
    cancel_run(browser, root, env, "navigate", [
        ("/hang.html", ["/hang.html"]),
        ("/xhr.html", ["/hang.txt"]),
        ("/stall.html", ["/hang.css"]),
        ("/done.html", ["/done.html"]),
    ], {"Network I/O: document": 4, "Network I/O: asynchronous XHR": 1,
        "resource_fetch[0]: style": 1})
    cancel_run(browser, root, env, "close-loading", [
        ("/stall.html", ["/hang.css"]),
    ], {"Network I/O: document": 1, "resource_fetch[0]: style": 1})
    cancel_run(browser, root, env, "close-xhr", [
        ("/xhr.html", ["/hang.txt"]),
    ], {"Network I/O: document": 1, "Network I/O: asynchronous XHR": 1})


def untraced_run(browser, root, env, label, value):
    """The browser runs in an empty directory and must leave it empty. Only
    an unset BROWSER_TRACE_FILE gets window_session.sh's default trace."""
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
    default = directory / "trace.json"
    if value is None:
        trace_check.check(trace_check.load(default))
    else:
        assert not default.exists(), "{} run wrote {}".format(label, default)


def unset_direct_run(browser, root, env):
    """D-T1 at the browser itself: window_session.sh always passes a trace
    path, so start the binary directly with the variable removed (dummy SDL
    video) and require that it writes nothing."""
    cwd = root / "unset-direct"
    cwd.mkdir()
    child_env = dict(os.environ, **env, SDL_VIDEO_DRIVER="dummy")
    child_env.pop("BROWSER_TRACE_FILE", None)
    process = subprocess.Popen(
        [str(browser), "--window", "about:blank"], cwd=cwd, env=child_env,
        stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True)
    try:
        time.sleep(2)
        assert process.poll() is None, process.stderr.read()
        process.terminate()
        stderr = process.communicate(timeout=20)[1]
    finally:
        if process.poll() is None:
            process.kill()
            process.wait()
    assert process.returncode == 0, (process.returncode, stderr)
    leftovers = sorted(path.name for path in cwd.iterdir())
    assert not leftovers, "unset direct run wrote {}".format(leftovers)


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
        about_run(browser, test_trace, root, env)
        multiwindow_run(browser, test_trace, root, env)
        cancel_runs(browser, root, env)
        untraced_run(browser, root, env, "unset", None)
        untraced_run(browser, root, env, "empty", "")
        unset_direct_run(browser, root, env)
        unwritable_run(browser, env)
    print("trace_window_integration passed: {} events".format(count))


if __name__ == "__main__":
    main(sys.argv)
