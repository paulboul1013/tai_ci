#!/usr/bin/env python3
"""Real-window run of the full-pipeline E2E script (docs/e2e-plan.md, layer 2).

Drives `tai-browser --window` with tests/tools/window_session.sh on a private
Xvfb display against tests/e2e_fixture.py on 127.0.0.1, through real X11
input: open /, click the #bottom fragment link, click the form link, click
and type into the text and password inputs, click the checkbox and the
submit button, then press the chrome Back button twice. Then the address bar
opens /inline.html, whose inline script only native runs (D6).

Each step checks the server's request log against the same checkpoint in
tests/fixtures/e2e_oracle.json, the window title, and the average color of a
few screenshot regions (decision E2: no pixel comparison with Python).
Click points and color regions come from `tai-browser --headless` layout and
display JSON for the same pages, offset by the tabbed chrome. Clean exit and
an SDL event log without errors or windowID=0 key events end the run.

Exits 77 (CTest skip) when Xvfb, xdotool, xwd or convert is missing. A
sanitizer build uses the LeakSanitizer suppressions of
tests/trace_window_integration.py. Set TAI_E2E_SHOTS to a directory to keep
the screenshots.

    python3 tests/e2e_window_integration.py TAI_BROWSER
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
SESSION = ROOT / "tests" / "tools" / "window_session.sh"
FIXTURE = ROOT / "tests" / "fixtures" / "e2e_oracle.json"
TOOLS = ("Xvfb", "xdotool", "xwd", "convert")
SKIP = 77

# src/presentation_internal.h at 800px with one unwrapped tab row:
# tabs_chrome_bottom = chrome_bottom 68.34 + tabs_toolbar_offset 6.48, the
# Back button is pressed at (22, tabs_back_button_y + 12) like
# tests/test_history_window.c.
CHROME_BOTTOM = 68.34 + 6.48
BACK = (22, 36.0 + 6.48 + 12.0)

BANNER = 0xffcc00ff
BOTTOM = 0xddeeffff
INLINE = 0x00aa55ff
WHITE = (0xff, 0xff, 0xff)
TOLERANCE = 24

sys.path.insert(0, str(ROOT / "tests"))
import e2e_fixture  # noqa: E402
import e2e_shape  # noqa: E402
import trace_window_integration  # noqa: E402


class Page:
    """Native headless layout and display of one fixture page."""

    def __init__(self, browser, url):
        raw = json.loads(subprocess.check_output(
            [str(browser), "--headless", url], timeout=60))
        self.boxes = []
        self.walk(raw["layout"])
        self.fills = [command for command in raw["display"]
                      if command["kind"] == "fill_rect"]

    def walk(self, box):
        self.boxes.append(box)
        for child in box["children"]:
            self.walk(child)

    def word(self, text):
        matches = [box for box in self.boxes if box.get("word") == text]
        assert len(matches) == 1, "word {!r}: {} boxes".format(text,
                                                              len(matches))
        return matches[0]

    def line(self, *words):
        for box in self.boxes:
            if [child.get("word") for child in box["children"]] == list(words):
                return box
        raise AssertionError("no line {}".format(words))

    def inputs(self):
        return [box for box in self.boxes if box["kind"] == "InputLayout"]

    def fill(self, rgba):
        matches = [command for command in self.fills
                   if command["rgba"] == rgba]
        assert len(matches) == 1, "fill {:08x}: {}".format(rgba, len(matches))
        return matches[0]


def center(box, scroll=0.0):
    """Window point of a page box's centre."""
    return (round(box["x"] + box["width"] / 2),
            round(box["y"] + box["height"] / 2 - scroll + CHROME_BOTTOM))


def inset(rect, scroll=0.0, margin=4):
    """A window crop well inside a page rectangle, clear of its edges."""
    left = int(rect["x"]) + margin
    top = int(rect["y"] - scroll + CHROME_BOTTOM) + margin
    right = int(rect["x"] + rect["width"]) - margin
    bottom = int(rect["y"] + rect["height"] - scroll + CHROME_BOTTOM) - margin
    assert right > left and bottom > top, rect
    return left, top, right - left, bottom - top


def pixels(png, crop):
    left, top, width, height = crop
    text = subprocess.check_output(
        ["convert", str(png), "-crop",
         "{}x{}+{}+{}".format(width, height, left, top), "+repage",
         "-depth", "8", "txt:-"], text=True)
    found = [tuple(int(value) for value in match.groups())
             for match in re.finditer(r"\((\d+),(\d+),(\d+)", text)]
    assert found, "no pixels in {} {}".format(png, crop)
    return found


def average(png, crop):
    found = pixels(png, crop)
    return tuple(sum(pixel[index] for pixel in found) / len(found)
                 for index in range(3))


def ink(png, crop):
    """Average of the darkest 5% of a text box: its glyph colour. Glyphs are
    subpixel antialiased, so the ink carries colour fringes and is compared
    by hue, not to an exact colour."""
    found = sorted(pixels(png, crop), key=sum)
    darkest = found[:max(1, len(found) // 20)]
    return tuple(sum(pixel[index] for pixel in darkest) / len(darkest)
                 for index in range(3))


def blue_ink(color):
    """#0055aa link text."""
    return color[2] - color[0] >= 100


def black_ink(color):
    """Black text: dark and without a blue cast."""
    return max(color) <= 120 and abs(color[2] - color[0]) <= 40


def near(color, want):
    return all(abs(got - expected) <= TOLERANCE
               for got, expected in zip(color, want))


def rgb(rgba):
    return (rgba >> 24 & 0xff, rgba >> 16 & 0xff, rgba >> 8 & 0xff)


class Run:
    def __init__(self, browser, root, env):
        self.browser = browser
        self.root = root
        self.session = trace_window_integration.Session(
            browser, root / "session", root, env)
        self.shots = {}
        expected = json.loads(FIXTURE.read_text(encoding="utf-8"))
        self.oracle = {item["step"]: item["checkpoint"]
                       for item in expected["scenarios"]["pipeline"]}
        self.server = e2e_fixture.E2EServer()
        try:
            self.index = Page(browser, self.server.url("/index.html"))
            self.form = Page(browser, self.server.url("/form.html"))
            self.inline = Page(browser, self.server.url("/inline.html"))
        except BaseException:
            self.server.close()
            raise
        self.mark = self.server.mark()

    def close(self):
        self.session.abandon()
        self.server.close()

    def title(self):
        output = self.session.run("title")
        return re.search(r"'(.*)'$", output.strip()).group(1)

    def wait_title(self, want, timeout=30):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if self.title() == want:
                time.sleep(0.5)
                return
            time.sleep(0.2)
        raise AssertionError("window title stayed {!r}, want {!r}".format(
            self.title(), want))

    def requests(self, step, want=None):
        now = self.server.mark()
        seen = e2e_shape.requests_shape(self.server.since(self.mark))
        self.mark = now
        want = (self.oracle[step]["requests"] if want is None
                else e2e_shape.requests_shape(want))
        assert seen == want, "{}: requests {}, oracle {}".format(
            step, seen, want)

    def shot(self, name):
        output = self.session.run("shot", name)
        assert "BLANK?" not in output, output
        png = pathlib.Path(output.split()[0])
        self.shots[name] = png
        return png

    def click(self, point):
        self.session.run("click", *map(str, point))

    def check(self, step, png, crop, want):
        color = average(png, crop)
        assert near(color, want), "{}: region {} is {}, want {}".format(
            step, crop, tuple(round(value) for value in color), want)

    def check_ink(self, step, png, crop, accept):
        color = ink(png, crop)
        assert accept(color), "{}: text {} has ink {}, want {}".format(
            step, crop, tuple(round(value) for value in color),
            accept.__doc__.strip())

    def pipeline(self):
        index, form = self.index, self.form
        scroll = self.oracle["fragment"]["scroll"]
        bottom_line = index.line("bottom", "of", "page")
        assert abs(bottom_line["y"] - scroll) < 0.0001, bottom_line
        banner = inset(index.fill(BANNER))
        bottom = inset(index.fill(BOTTOM), scroll)
        open_form = index.word("open")
        link = inset(open_form, scroll, margin=0)

        self.session.start(self.server.url("/"))
        self.wait_title(e2e_fixture.READY_TITLE)
        self.requests("open")
        png = self.shot("open")
        self.check("open", png, banner, rgb(BANNER))

        self.click(center(index.word("jump")))
        time.sleep(0.5)
        self.requests("fragment")
        png = self.shot("fragment")
        self.check("fragment", png, bottom, rgb(BOTTOM))
        self.check_ink("fragment", png, link, blue_ink)

        self.click(center(open_form, scroll))
        self.wait_title(e2e_fixture.FORM_TITLE)
        self.requests("form")
        self.shot("form")

        name, secret, agree = form.inputs()[:3]
        self.click(center(name))
        self.session.run("type", e2e_fixture.TEXT_VALUE)
        self.click(center(secret))
        self.session.run("type", e2e_fixture.PASSWORD_VALUE)
        self.click(center(agree))
        self.requests("filled")
        self.shot("filled")

        self.click(center(form.word("Send")))
        self.wait_title(e2e_fixture.SUBMITTED_TITLE)
        self.requests("submit")
        png = self.shot("submit")
        self.check("submit", png, banner, WHITE)

        self.click(BACK)
        self.wait_title(e2e_fixture.FORM_TITLE)
        self.requests("back_form")

        self.click(BACK)
        self.wait_title(e2e_fixture.READY_TITLE)
        self.requests("back_index")
        png = self.shot("back_index")
        self.check("back_index", png, bottom, rgb(BOTTOM))
        # Now /form.html is visited: a:visited paints purple, which the
        # oracle's parse_color turns into black.
        self.check_ink("back_index", png, link, black_ink)

        url = self.server.url("/inline.html")
        self.session.navigate(url)
        self.wait_title(e2e_fixture.INLINE_TITLE)
        self.requests("inline", [{"method": "GET", "path": "/inline.html",
                                  "cookie": e2e_fixture.COOKIE,
                                  "body": None}])
        png = self.shot("inline")
        self.check("inline", png, inset(self.inline.fill(INLINE)),
                   rgb(INLINE))
        return url

    def finish(self, url):
        events = self.session.run("events")
        typed = re.search(r"^text: (.*)$", events, re.MULTILINE)
        assert typed and typed.group(1) == (e2e_fixture.TEXT_VALUE
                                          + e2e_fixture.PASSWORD_VALUE + url), events
        assert re.search(r"windowid=0:0\b", events), events
        self.session.stop()
        log = (self.root / "session" / "browser.log").read_text(
            errors="replace")
        errors = [line for line in log.splitlines()
                  if re.search(r"error|badwindow", line, re.IGNORECASE)
                  and "SDL_EVENT" not in line]
        assert not errors, "browser log errors:\n" + "\n".join(errors[-10:])


def main(argv):
    if len(argv) != 2:
        raise SystemExit(__doc__)
    missing = [tool for tool in TOOLS if shutil.which(tool) is None]
    if missing:
        print("e2e_window_integration skipped: missing {}".format(
            ", ".join(missing)))
        return SKIP
    browser = pathlib.Path(argv[1]).resolve()
    keep = os.environ.get("TAI_E2E_SHOTS")
    with tempfile.TemporaryDirectory(prefix="tai-e2e-window-") as name:
        root = pathlib.Path(name)
        env = trace_window_integration.sanitizer_env(root)
        run = Run(browser, root, env)
        try:
            url = run.pipeline()
            run.finish(url)
        finally:
            if keep:
                pathlib.Path(keep).mkdir(parents=True, exist_ok=True)
                for shot in run.shots.values():
                    if shot.exists():
                        shutil.copy(shot, keep)
            run.close()
    print("e2e_window_integration passed: {} screenshots".format(
        len(run.shots)))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
