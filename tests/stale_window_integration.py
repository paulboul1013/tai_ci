#!/usr/bin/env python3
"""Real-window check that late load completions are dropped
(docs/stale-callback-plan.md), end to end through the SDL loop.

Serves tests/stale_fixture.py on 127.0.0.1 in this process, so the test owns
the gates, and drives `tai-browser --window` with
tests/tools/window_session.sh on a private Xvfb display. One browser session
per scenario:

* W1 CSS late: open /css-a, whose /slow.css is held. Once the server has seen
  /slow.css, type the /b URL into the address bar and press Return. The title
  and banner colour must turn to page-b, then stay so after /slow.css is
  released and served; the page-b heading must not turn red.
* W2 JS late: the same with /js-a and its held /slow.js. The late script must
  never run: no /stale-beacon request.
* W3 window close while loading: open the held /slow-a, close the window with
  a WM_DELETE_WINDOW request, release /slow-a. The browser must exit 0
  promptly.

Ordering comes from the gates; only "nothing changes after the late response"
is a fixed 0.5 s wait. Every session ends with a browser log free of errors
and failures: a superseded navigation that the tab accepted anyway logs
"tab navigation failed: navigation cancelled", although nothing on screen
changes, because the loader already aborted it.

Exits 77 (CTest skip) when Xvfb, xdotool, xwd or convert is missing. A
sanitizer build uses the LeakSanitizer suppressions of
tests/trace_window_integration.py. Set TAI_STALE_SHOTS to a directory to keep
the screenshots.

    python3 tests/stale_window_integration.py TAI_BROWSER
"""

import json
import os
import pathlib
import re
import shutil
import subprocess
import sys
import tempfile
import time


ROOT = pathlib.Path(__file__).resolve().parents[1]
TOOLS = ("Xvfb", "xdotool", "xwd", "convert")
SKIP = 77
QUIET = 0.5
EXIT_TIMEOUT = 5.0

sys.path.insert(0, str(ROOT / "tests"))
import e2e_window_integration as e2e  # noqa: E402
import stale_fixture  # noqa: E402
import trace_window_integration  # noqa: E402


def red_ink(color):
    """Red text (the late /slow.css rule)."""
    return color[0] - max(color[1], color[2]) >= 80


class Run:
    def __init__(self, browser, root, env):
        self.browser = browser
        self.root = root
        self.env = env
        self.shots = []
        self.session = None
        self.server = stale_fixture.StaleServer()
        try:
            raw = json.loads(subprocess.check_output(
                [str(browser), "--headless", self.server.url("/b")],
                timeout=60))
            fills = [command for command in raw["display"]
                     if command["kind"] == "fill_rect"
                     and command["rgba"] == stale_fixture.B_BANNER]
            assert len(fills) == 1, fills
            # css-a and js-a put their banner at the same place as page-b.
            self.banner = e2e.inset(fills[0])
            boxes = []
            stack = [raw["layout"]]
            while stack:
                box = stack.pop()
                boxes.append(box)
                stack.extend(reversed(box["children"]))
            heading = next(box for box in boxes
                           if box.get("word") == "page-b")
            self.heading = e2e.inset(heading, margin=0)
        except BaseException:
            self.server.close()
            raise

    def close(self):
        if self.session:
            self.session.abandon()
        self.server.close()

    def begin(self, name, path):
        self.session = trace_window_integration.Session(
            self.browser, self.root / name, self.root, self.env)
        self.mark = self.server.mark()
        self.session.start(self.server.url(path))
        self.server.wait_seen(path, self.mark)

    def title(self):
        output = self.session.run("title")
        return re.search(r"'(.*)'$", output.strip()).group(1)

    def wait_title(self, want, timeout=30):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if self.title() == want:
                return
            time.sleep(0.1)
        raise AssertionError("window title stayed {!r}, want {!r}".format(
            self.title(), want))

    def shot(self, name):
        output = self.session.run("shot", name)
        assert "BLANK?" not in output, output
        png = pathlib.Path(output.split()[0])
        self.shots.append(png)
        return png

    def check_b(self, step):
        title = self.title()
        assert title == "page-b", "{}: title {!r}, want 'page-b'".format(
            step, title)
        png = self.shot(step)
        color = e2e.average(png, self.banner)
        want = e2e.rgb(stale_fixture.B_BANNER)
        assert e2e.near(color, want), "{}: banner {} is {}, want {}".format(
            step, self.banner, tuple(round(value) for value in color), want)
        ink = e2e.ink(png, self.heading)
        assert e2e.black_ink(ink) and not red_ink(ink), (
            "{}: page-b heading {} has ink {}, want black".format(
                step, self.heading, tuple(round(value) for value in ink)))

    def paths(self):
        return [path.split("?", 1)[0]
                for path in self.server.since(self.mark)]

    def check_log(self, name):
        log = (self.root / name / "browser.log").read_text(errors="replace")
        errors = [line for line in log.splitlines()
                  if re.search(r"error|failed|badwindow", line,
                                   re.IGNORECASE)
                  and "SDL_EVENT" not in line]
        assert not errors, "{}: browser log errors:\n{}".format(
            name, "\n".join(errors[-10:]))

    def subresource_late(self, name, page, gated):
        """W1/W2: a newer navigation commits while the old page's
        subresource is held; the late subresource changes nothing."""
        self.begin(name, page)
        self.server.wait_seen(gated, self.mark)
        self.session.navigate(self.server.url("/b"))
        self.wait_title("page-b")
        self.check_b(name + "-committed")

        self.server.release(gated)
        self.server.wait_served(gated)
        time.sleep(QUIET)
        self.check_b(name + "-late")

        paths = self.paths()
        assert "/stale-beacon" not in paths, "{}: requests {}".format(
            name, paths)
        assert sorted(set(paths)) == sorted({page, gated, "/b"}), (
            "{}: requests {}".format(name, paths))
        events = self.session.run("events")
        assert re.search(r"windowid=0:0\b", events), events
        self.session.stop()
        self.check_log(name)

    def close_while_loading(self, name, page):
        """W3: closing the window while the only load is held."""
        self.begin(name, page)
        closed = time.monotonic()
        self.session.run("close")
        # The gate stays shut until the browser has exited, so a close that
        # waited for the held load would time out here.
        rc = self.root / name / "rc"
        while not rc.exists():
            if time.monotonic() - closed > EXIT_TIMEOUT:
                raise AssertionError("{}: browser still running {} s after "
                                     "close".format(name, EXIT_TIMEOUT))
            time.sleep(0.05)
        exited = time.monotonic() - closed
        self.server.release(page)
        status = rc.read_text().strip()
        result = self.session.run("stop", check=False)
        self.session.started = False
        assert status == "0" and "browser exit=0" in result.stdout, (
            "{}: exit {} ({}{})".format(name, status, result.stdout,
                                        result.stderr))
        self.check_log(name)
        return exited


def main(argv):
    if len(argv) != 2:
        raise SystemExit(__doc__)
    missing = [tool for tool in TOOLS if shutil.which(tool) is None]
    if missing:
        print("stale_window_integration skipped: missing {}".format(
            ", ".join(missing)))
        return SKIP
    browser = pathlib.Path(argv[1]).resolve()
    keep = os.environ.get("TAI_STALE_SHOTS")
    with tempfile.TemporaryDirectory(prefix="tai-stale-window-") as name:
        root = pathlib.Path(name)
        env = trace_window_integration.sanitizer_env(root)
        run = Run(browser, root, env)
        try:
            run.subresource_late("css_late", "/css-a", "/slow.css")
            print("W1 css_late passed")
            run.subresource_late("js_late", "/js-a", "/slow.js")
            print("W2 js_late passed")
            exited = run.close_while_loading("close_loading", "/slow-a")
            print("W3 close_loading passed: exit 0 {:.2f} s after "
                  "close".format(exited))
        finally:
            if keep:
                pathlib.Path(keep).mkdir(parents=True, exist_ok=True)
                for shot in run.shots:
                    if shot.exists():
                        shutil.copy(shot, keep)
            run.close()
    print("stale_window_integration passed: {} screenshots".format(
        len(run.shots)))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
