#!/usr/bin/env python3
"""Compare native tabbed-chrome row positions with the frozen Python oracle.

The toolbar rows (Back/Forward, bookmarks, address field) and the chrome
bottom move down one line when the tab strip wraps, and whether it wraps
depends on the tab labels, not only the window width: one tab wraps below
84px, two tabs below 125px. The oracle is measured live for one and two tabs,
every active index, and widths around each chrome breakpoint.

    python3 tests/tab_strip_differential.py PATH_TO_TAB_STRIP_PROBE
"""

import contextlib
import io
import json
import os
import pathlib
import subprocess
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import tabs_oracle_probe as reference  # noqa: E402

WIDTHS = (70, 78, 79, 83, 84, 93, 94, 100, 120, 124, 125, 127, 128, 200, 231,
          232, 800)
# Python lays bold and regular labels out on slightly different baselines, so
# a wrapped strip's height varies by up to 0.14px with the label mix. Native
# uses one line height; larger differences are real row errors.
TOLERANCE = 0.15


def measure_oracle():
    browser, trace_path = reference.import_reference()
    app = None
    results = {}
    try:
        with contextlib.redirect_stdout(io.StringIO()):
            app = browser.BrowserApp()
            window = app.new_window(browser.URL("about:blank"))
        tabs = [window.tabs[0]]
        for count in (1, 2):
            if count > len(tabs):
                with contextlib.redirect_stdout(io.StringIO()):
                    tabs.append(window.new_tab(browser.URL("about:blank")))
            for active in range(count):
                window.set_active_tab(tabs[active])
                for width in WIDTHS:
                    window.resize(width, 600)
                    reference.wait_for(
                        lambda: all(tab.width == width for tab in tabs),
                        "tab viewports resized")
                    chrome = window.chrome
                    chrome.render()
                    address = chrome.find_address_layout(
                        chrome.find_address_node())
                    back = chrome.find_button_layout("back")
                    results[(count, active, width)] = {
                        "chrome_bottom": round(float(chrome.bottom), 3),
                        "address_y": round(float(address.y), 3),
                        "back_y": round(float(back.y), 3),
                    }
    finally:
        if app is not None:
            for window in list(app.windows):
                window.close()
            app.network.set_needs_quit()
            app.network.join_thread(timeout=2)
            app.raster.set_needs_quit()
            app.raster.join_thread(timeout=2)
            app.measure.finish()
            browser.sdl2.SDL_Quit()
        trace_path.unlink(missing_ok=True)
    return results


def main():
    if len(sys.argv) != 2:
        raise SystemExit(__doc__)
    original_cwd = pathlib.Path.cwd()
    with contextlib.redirect_stdout(io.StringIO()):
        try:
            oracle = measure_oracle()
        finally:
            os.chdir(original_cwd)
    cases = ["{}:{}:{}".format(*key) for key in oracle]
    output = subprocess.run([sys.argv[1], *cases], check=True,
                            capture_output=True, text=True).stdout
    failures = []
    for line in output.splitlines():
        native = json.loads(line)
        key = (native["tabs"], native["active"], native["width"])
        expected = oracle[key]
        for field in ("chrome_bottom", "address_y", "back_y"):
            if abs(native[field] - expected[field]) > TOLERANCE:
                failures.append("{} {}: native {} oracle {}".format(
                    key, field, native[field], expected[field]))
    if len(output.splitlines()) != len(oracle):
        failures.append("native probe returned {} of {} cases".format(
            len(output.splitlines()), len(oracle)))
    if failures:
        raise SystemExit("tab strip rows differ:\n" + "\n".join(failures))
    print("Tab strip differential: {} one/two-tab chrome row cases match the "
          "oracle".format(len(oracle)))


if __name__ == "__main__":
    main()
