#!/usr/bin/env python3
"""Repeatable Python oracle probe for the HTTPS lock and address-field slice.

The frozen Python browser runs with SDL's dummy video driver against three
127.0.0.1 servers: plain HTTP, HTTPS signed by a per-run test CA that
SSL_CERT_FILE makes trusted, and HTTPS signed by an untrusted CA. A barrier
holds one HTTPS document so the pending state can be observed. Dynamic ports
and transport error text are omitted from the JSON result.

    python3 tests/https_oracle_probe.py            # print the result
    python3 tests/https_oracle_probe.py --check    # compare with the fixture
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
FIXTURE = ROOT / "tests" / "fixtures" / "https_oracle.json"

sys.path.insert(0, str(ROOT / "tests"))
import https_fixture  # noqa: E402


def wait_for(predicate, label, timeout=8):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if predicate():
            return
        time.sleep(0.005)
    raise AssertionError("timed out waiting for {}".format(label))


def import_reference():
    sys.path.insert(0, str(ROOT / "tests"))
    import oracle

    oracle.verify_reference()
    os.environ["SDL_VIDEODRIVER"] = "dummy"
    os.environ["BROWSER_RENDER_BACKEND"] = "cpu"
    os.environ["BROWSER_RASTER_MODE"] = "sync"
    descriptor, trace_path = tempfile.mkstemp(prefix="tai-https-oracle-", suffix=".trace")
    os.close(descriptor)
    os.environ["BROWSER_TRACE_FILE"] = trace_path

    source = REFERENCE / "browser.py"
    os.chdir(REFERENCE)
    spec = importlib.util.spec_from_file_location("tai_https_fixed_reference", source)
    browser = importlib.util.module_from_spec(spec)
    with contextlib.redirect_stdout(io.StringIO()):
        spec.loader.exec_module(browser)
    return browser, pathlib.Path(trace_path)


def rect(left, top, right, bottom):
    return [round(float(v), 3) for v in (left, top, right, bottom)]


def layout_rect(layout):
    return rect(layout.x, layout.y, layout.x + layout.width,
                layout.y + layout.height)


def h1_text(browser, tab):
    if tab.nodes is None:
        return None
    for node in browser.tree_to_list(tab.nodes, []):
        if isinstance(node, browser.Element) and node.tag == "h1":
            return "".join(
                item.text for item in browser.tree_to_list(node, [])
                if isinstance(item, browser.Text)
            ).strip()
    return None


def tab_call(browser, window, tab, function, *args):
    done = threading.Event()
    result = {}

    def invoke():
        try:
            result["value"] = function(*args)
        except BaseException as exc:
            result["error"] = exc
        finally:
            done.set()

    if not window.schedule_tab_task(tab, invoke,
                                    priority=browser.TaskPriority.INPUT,
                                    source="https-oracle-probe"):
        raise AssertionError("could not schedule tab-owned probe")
    wait_for(done.is_set, "tab task")
    if "error" in result:
        raise result["error"]
    return result.get("value")


def commit(browser, window, tab):
    """Run one frame so BrowserWindow.committed_states holds this tab's data."""
    with contextlib.redirect_stdout(io.StringIO()):
        tab_call(browser, window, tab, tab.run_animation_frame)


def wait_heading(browser, window, tab, heading):
    wait_for(lambda: h1_text(browser, tab) == heading,
             "document {}".format(heading))
    commit(browser, window, tab)


def resize(browser, window, width, height):
    window.resize(width, height)
    tabs = window.tabs_snapshot()
    wait_for(lambda: all(tab.width == width for tab in tabs),
             "tab viewports resized")
    for tab in tabs:
        commit(browser, window, tab)


def target_at(browser, chrome, x, y):
    with contextlib.redirect_stdout(io.StringIO()):
        layout = chrome.layout_object_at(x, y)
    node = getattr(layout, "node", None)
    while node is not None:
        if isinstance(node, browser.Element):
            if node.tag == "button":
                return "button:" + node.attributes.get("id", "")
            if node.tag == "input":
                return "input:" + node.attributes.get("id", "")
            if node.tag == "a":
                return "link:" + node.attributes.get("href", "")
        node = node.parent
    return None


def chrome_geometry(browser, window):
    chrome = window.chrome
    chrome.render()
    address = chrome.find_address_layout(chrome.find_address_node())
    bookmark = chrome.find_button_layout("bookmark")
    lock = chrome.security_icon_rect
    address_rect = layout_rect(address)
    middle_y = (address_rect[1] + address_rect[3]) / 2
    slot_left = address_rect[0] - (browser.SECURITY_ICON_SLOT if lock else 0)
    hits = {
        "slot_left": target_at(browser, chrome, slot_left, middle_y),
        "slot_last": target_at(browser, chrome, address_rect[0] - 1, middle_y),
        "address_left": target_at(browser, chrome, address_rect[0], middle_y),
    }

    # Clicking the lock slot must not focus the address field.
    chrome.focus = None
    chrome.address_bar = "https://draft.invalid/"
    chrome.address_bar_cursor = len(chrome.address_bar)
    chrome.address_bar_dirty = True
    with contextlib.redirect_stdout(io.StringIO()):
        chrome.click(slot_left + 1, middle_y)
    click_slot = {
        "focus": chrome.focus,
        "address": chrome.address_bar,
        "dirty": chrome.address_bar_dirty,
    }
    chrome.focus = None
    chrome.render()
    return {
        "secure": bool(window.active_is_secure()),
        "window_width": window.width,
        "address_rect": address_rect,
        "bookmark_rect": layout_rect(bookmark),
        "lock_rect": rect(lock.left(), lock.top(), lock.right(), lock.bottom())
        if lock is not None else None,
        "hits": hits,
        "click_slot": click_slot,
    }


GEOMETRY_WIDTHS = (800, 232, 231, 120, 70)


def measure_widths(browser, window, geometry, label):
    """Chrome breakpoints: wide, first/last two-row widths, narrow, tiny."""
    for width in GEOMETRY_WIDTHS:
        resize(browser, window, width, 600)
        geometry["{}_{}".format(label, width)] = chrome_geometry(browser, window)
    resize(browser, window, 800, 600)


def secure_state(browser, window, servers, tab):
    return {
        "secure": bool(window.active_is_secure()),
        "tab_secure": bool(tab.secure),
        "heading": h1_text(browser, tab),
        "url": servers.normalize(str(tab.url)),
    }


def run_probe():
    original_cwd = pathlib.Path.cwd()
    browser = app = servers = trace_path = None
    saved_cert_file = os.environ.get("SSL_CERT_FILE")
    with tempfile.TemporaryDirectory(prefix="tai-https-oracle-") as directory:
        try:
            material = https_fixture.make_material(pathlib.Path(directory))
            # Only the per-run test CA is trusted; the untrusted server fails
            # verification the same way an invalid public certificate would.
            os.environ["SSL_CERT_FILE"] = str(material["trusted"]["ca"])
            servers = https_fixture.FixtureServers(material)
            browser, trace_path = import_reference()
            with contextlib.redirect_stdout(io.StringIO()):
                app = browser.BrowserApp()
                window = app.new_window(browser.URL(servers.https_url("/secure-home")))
            tab = window.tabs[0]
            wait_heading(browser, window, tab, "secure-home")
            result = {"transitions": {}, "geometry": {}, "redirects": {},
                      "history": {}, "tabs": {}}
            transitions = result["transitions"]
            transitions["https_success"] = secure_state(browser, window, servers, tab)

            measure_widths(browser, window, result["geometry"], "secure")

            # Pending HTTPS navigation from a secure page.
            window.schedule_load(browser.URL(servers.https_url("/secure-delay")), tab=tab)
            servers.wait_seen("/secure-delay")
            wait_for(lambda: tab.url is not None and "secure-delay" in str(tab.url),
                     "pending navigation started")
            commit(browser, window, tab)
            transitions["https_pending"] = secure_state(browser, window, servers, tab)
            servers.gates["/secure-delay"].set()
            wait_heading(browser, window, tab, "secure-delayed")
            transitions["https_after_pending"] = secure_state(browser, window, servers, tab)

            # Certificate error from a secure page.
            window.schedule_load(browser.URL(servers.untrusted_url("/secure-home")), tab=tab)
            wait_heading(browser, window, tab, "Certificate Error")
            transitions["certificate_error"] = secure_state(browser, window, servers, tab)

            # Other transport failure over HTTPS, again from a secure page.
            window.schedule_load(browser.URL(servers.https_url("/secure-next")), tab=tab)
            wait_heading(browser, window, tab, "secure-next")
            window.schedule_load(browser.URL(servers.https_url("/secure-fail")), tab=tab)
            servers.wait_seen("/secure-fail")
            wait_heading(browser, window, tab, "Network Error")
            transitions["network_error"] = secure_state(browser, window, servers, tab)

            # Plain HTTP and internal pages.
            window.schedule_load(browser.URL(servers.http_url("/plain-home")), tab=tab)
            wait_heading(browser, window, tab, "plain-home")
            transitions["http_success"] = secure_state(browser, window, servers, tab)
            measure_widths(browser, window, result["geometry"], "insecure")
            window.schedule_load(browser.URL(servers.https_url("/secure-home")), tab=tab)
            wait_heading(browser, window, tab, "secure-home")
            window.schedule_load(browser.URL("about:bookmarks"), tab=tab)
            wait_for(lambda: str(tab.url) == "about:bookmarks", "about:bookmarks")
            commit(browser, window, tab)
            transitions["about_bookmarks"] = {
                "secure": bool(window.active_is_secure()),
                "tab_secure": bool(tab.secure),
            }

            # Redirects: secure follows the requested URL.
            window.schedule_load(browser.URL(servers.http_url("/to-https")), tab=tab)
            servers.wait_seen("/to-https")
            wait_heading(browser, window, tab, "redirected-secure")
            result["redirects"]["http_to_https"] = secure_state(browser, window, servers, tab)
            window.schedule_load(browser.URL(servers.https_url("/to-http")), tab=tab)
            servers.wait_seen("/to-http")
            wait_heading(browser, window, tab, "redirected-plain")
            result["redirects"]["https_to_http"] = secure_state(browser, window, servers, tab)

            # Back/Forward between a secure and an insecure page.
            window.schedule_load(browser.URL(servers.https_url("/secure-home")), tab=tab)
            wait_heading(browser, window, tab, "secure-home")
            window.schedule_load(browser.URL(servers.http_url("/plain-home")), tab=tab)
            wait_heading(browser, window, tab, "plain-home")
            tab_call(browser, window, tab, tab.go_back)
            wait_heading(browser, window, tab, "secure-home")
            result["history"]["back_to_https"] = secure_state(browser, window, servers, tab)
            tab_call(browser, window, tab, tab.go_forward)
            wait_heading(browser, window, tab, "plain-home")
            result["history"]["forward_to_http"] = secure_state(browser, window, servers, tab)
            tab_call(browser, window, tab, tab.go_back)
            wait_heading(browser, window, tab, "secure-home")

            # Two tabs: the lock follows the active tab.
            with contextlib.redirect_stdout(io.StringIO()):
                second = window.new_tab(browser.URL(servers.http_url("/plain-home")))
            wait_heading(browser, window, second, "plain-home")
            tabs = result["tabs"]
            tabs["second_http_active"] = {
                "active_index": window.tabs_snapshot().index(window.active_tab_snapshot()),
                "secure": bool(window.active_is_secure()),
            }
            window.set_active_tab(tab)
            commit(browser, window, tab)
            tabs["first_https_active"] = {
                "active_index": window.tabs_snapshot().index(window.active_tab_snapshot()),
                "secure": bool(window.active_is_secure()),
            }
            window.set_active_tab(second)
            commit(browser, window, second)
            tabs["back_to_second"] = {
                "active_index": window.tabs_snapshot().index(window.active_tab_snapshot()),
                "secure": bool(window.active_is_secure()),
            }
        finally:
            if servers is not None:
                for gate in servers.gates.values():
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
            if servers is not None:
                servers.close()
            os.chdir(original_cwd)
            if trace_path is not None:
                trace_path.unlink(missing_ok=True)
            if saved_cert_file is None:
                os.environ.pop("SSL_CERT_FILE", None)
            else:
                os.environ["SSL_CERT_FILE"] = saved_cert_file
    return result


def main():
    import argparse

    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true",
                        help="compare the probe output with {}".format(FIXTURE))
    args = parser.parse_args()
    # The oracle prints redirect diagnostics from its network threads.
    with contextlib.redirect_stdout(io.StringIO()):
        result = run_probe()
    if args.check:
        expected = json.loads(FIXTURE.read_text(encoding="utf-8"))
        if result != expected:
            print(json.dumps(result, ensure_ascii=False, indent=2, sort_keys=True),
                  file=sys.stderr)
            raise SystemExit("Python HTTPS oracle differs from {}".format(FIXTURE))
        print("Python HTTPS oracle probe matches tests/fixtures/https_oracle.json")
    else:
        print(json.dumps(result, ensure_ascii=False, indent=2, sort_keys=True))


if __name__ == "__main__":
    main()
