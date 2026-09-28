#!/usr/bin/env python3
"""Repeatable Python oracle probe for the chrome and history slice.

The frozen Python browser runs with SDL's dummy video driver against the
127.0.0.1 server in tests/history_fixture.py. Each scenario starts in a fresh
tab and records, after every step, the tab's URL, heading, history list and
index, Back/Forward availability, scroll, the address-bar display text and
draft state, and the requests the server received. Dynamic ports and
transport error text are omitted from the JSON result.

    python3 tests/history_oracle_probe.py            # print the result
    python3 tests/history_oracle_probe.py --check    # compare with the fixture
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
FIXTURE = ROOT / "tests" / "fixtures" / "history_oracle.json"

sys.path.insert(0, str(ROOT / "tests"))
import history_fixture  # noqa: E402


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
    descriptor, trace_path = tempfile.mkstemp(prefix="tai-history-oracle-",
                                              suffix=".trace")
    os.close(descriptor)
    os.environ["BROWSER_TRACE_FILE"] = trace_path

    source = REFERENCE / "browser.py"
    os.chdir(REFERENCE)
    spec = importlib.util.spec_from_file_location(
        "tai_history_fixed_reference", source)
    browser = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(browser)
    return browser, pathlib.Path(trace_path)


class Probe:
    def __init__(self, browser, window, server):
        self.browser = browser
        self.window = window
        self.server = server

    # -- tab-thread access -------------------------------------------------

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
                source="history-oracle-probe"):
            raise AssertionError("could not schedule tab-owned probe")
        wait_for(done.is_set, "tab task")
        if "error" in result:
            raise result["error"]
        return result.get("value")

    def commit(self, tab):
        """Run one frame so committed_states holds this tab's data, then let
        the browser thread apply draft discards the way a raster pass does."""
        self.tab_call(tab, tab.run_animation_frame)
        self.window.raster_and_draw()

    def settle(self, tab):
        """Drain queued tab work (for example a scheduled go_back)."""
        self.tab_call(tab, lambda: None)

    # -- waiting -------------------------------------------------------------

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

    def wait_new_document(self, tab, nodes):
        wait_for(lambda: tab.nodes is not None and tab.nodes is not nodes,
                 "replacement document")
        self.commit(tab)

    def load(self, tab, path, heading):
        self.window.schedule_load(self.browser.URL(self.server.url(path)),
                                  tab=tab)
        self.wait_heading(tab, heading)

    def new_tab(self, path, heading):
        tab = self.window.new_tab(self.browser.URL(self.server.url(path)))
        self.wait_heading(tab, heading)
        return tab

    # -- observation ---------------------------------------------------------

    def state(self, tab, mark=None):
        chrome = self.window.chrome
        normalize = self.server.normalize
        result = {
            "url": normalize(str(tab.url)) if tab.url is not None else None,
            "heading": self.heading(tab),
            "history": [normalize(str(url)) for url in tab.history],
            "history_index": tab.history_index,
            "can_go_back": bool(tab.can_go_back()),
            "can_go_forward": bool(tab.can_go_forward()),
            "scroll": round(float(tab.scroll), 3),
            "secure": bool(tab.secure),
        }
        if tab is self.window.active_tab_snapshot():
            result["address"] = normalize(chrome.address_bar_display_text())
            result["address_focused"] = chrome.focus == "address bar"
            result["address_dirty"] = bool(chrome.address_bar_dirty)
        if mark is not None:
            result["requests"] = self.requests(mark)
        return result

    def requests(self, mark):
        return [{"method": item["method"], "path": item["path"],
                 "has_body": bool(item["body"])}
                for item in self.server.since(mark)]

    # -- chrome and page input -------------------------------------------------

    def set_draft(self, focused):
        chrome = self.window.chrome
        chrome.focus = "address bar" if focused else None
        chrome.address_bar = "typed draft"
        chrome.address_bar_cursor = len(chrome.address_bar)
        chrome.address_bar_dirty = True

    def draft_state(self):
        chrome = self.window.chrome
        return {
            "address": self.server.normalize(chrome.address_bar_display_text()),
            "focused": chrome.focus == "address bar",
            "dirty": bool(chrome.address_bar_dirty),
        }

    def chrome_center(self, predicate):
        browser = self.browser
        chrome = self.window.chrome
        chrome.render()
        for obj in browser.tree_to_list(chrome.document, []):
            node = getattr(obj, "node", None)
            if getattr(obj, "width", None) is None or obj.width <= 0:
                continue
            ancestor = node
            while ancestor is not None:
                if isinstance(ancestor, browser.Element) and predicate(ancestor):
                    return (obj.x + obj.width / 2, obj.y + obj.height / 2)
                ancestor = ancestor.parent
        raise AssertionError("chrome element not found")

    def click_tab_label(self, index):
        href = "tab-{}".format(index)
        x, y = self.chrome_center(
            lambda node: node.tag == "a" and node.attributes.get("href") == href)
        self.window.handle_click(x, y)

    def click_button(self, button_id):
        x, y = self.chrome_center(
            lambda node: node.tag == "button"
            and node.attributes.get("id") == button_id)
        self.window.handle_click(x, y)

    def page_link_point(self, tab, href):
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

        point = self.tab_call(tab, locate)
        if point is None:
            raise AssertionError("page link {} not found".format(href))
        return point[0], point[1] + self.window.chrome.bottom

    def form_element(self, tab):
        browser = self.browser
        for node in browser.tree_to_list(tab.nodes, []):
            if isinstance(node, browser.Element) and node.tag == "form":
                return node
        raise AssertionError("form not found")


# ---------------------------------------------------------------------------
# Scenarios. Each returns a dict of named checkpoints.


def basic_truncation(probe):
    mark = probe.server.mark()
    tab = probe.new_tab("/a", "page-a")
    steps = {"a": probe.state(tab)}
    probe.load(tab, "/b", "page-b")
    steps["b"] = probe.state(tab)
    probe.load(tab, "/c", "page-c")
    steps["c"] = probe.state(tab)
    probe.window.schedule_go_back()
    probe.wait_heading(tab, "page-b")
    steps["back_b"] = probe.state(tab)
    probe.window.schedule_go_back()
    probe.wait_heading(tab, "page-a")
    steps["back_a"] = probe.state(tab)
    probe.window.schedule_go_forward()
    probe.wait_heading(tab, "page-b")
    steps["forward_b"] = probe.state(tab)
    probe.window.schedule_go_back()
    probe.wait_heading(tab, "page-a")
    probe.load(tab, "/d", "page-d")
    steps["truncated_d"] = probe.state(tab, mark)
    return steps


def pending(probe):
    server = probe.server
    tab = probe.new_tab("/a", "page-a")
    mark = server.mark()
    probe.window.schedule_load(probe.browser.URL(server.url("/delay-b")),
                               tab=tab)
    server.wait_seen("/delay-b", mark)
    probe.settle(tab)
    probe.commit(tab)
    steps = {"pending": probe.state(tab)}
    server.release("/delay-b")
    probe.wait_heading(tab, "delay-b")
    steps["loaded"] = probe.state(tab, mark)
    server.reset_gate("/delay-b")
    return steps


def pending_back(probe):
    """Decision 1: native keeps its behaviour; the oracle documents Python's."""
    server = probe.server
    tab = probe.new_tab("/a", "page-a")
    nodes = tab.nodes
    mark = server.mark()
    probe.window.schedule_load(probe.browser.URL(server.url("/delay-b")),
                               tab=tab)
    server.wait_seen("/delay-b", mark)
    probe.settle(tab)
    probe.window.schedule_go_back()
    probe.wait_new_document(tab, nodes)
    steps = {"back": probe.state(tab)}
    server.release("/delay-b")
    server.wait_seen("/a", mark)
    probe.commit(tab)
    steps["released"] = probe.state(tab, mark)
    server.reset_gate("/delay-b")
    return steps


def failures(probe):
    server = probe.server
    steps = {}

    tab = probe.new_tab("/a", "page-a")
    probe.load(tab, "/b", "page-b")
    probe.load(tab, "/fail", "Network Error")
    steps["after_b"] = probe.state(tab)
    probe.window.schedule_go_back()
    probe.wait_heading(tab, "page-b")
    steps["after_b_back"] = probe.state(tab)

    tab = probe.new_tab("/a", "page-a")
    probe.load(tab, "/b", "page-b")
    probe.load(tab, "/c", "page-c")
    probe.window.schedule_go_back()
    probe.wait_heading(tab, "page-b")
    probe.load(tab, "/fail", "Network Error")
    steps["from_middle"] = probe.state(tab)

    tab = probe.new_tab("/a", "page-a")
    probe.load(tab, "/e", "page-e")
    probe.window.schedule_go_back()
    probe.wait_heading(tab, "page-a")
    server.fail_paths.add("/e")
    probe.window.schedule_go_forward()
    probe.wait_heading(tab, "Network Error")
    steps["forward_to_failing"] = probe.state(tab)
    server.fail_paths.discard("/e")
    probe.window.schedule_go_back()
    probe.wait_heading(tab, "page-a")
    steps["back_from_error"] = probe.state(tab)
    server.fail_paths.add("/a")
    probe.window.schedule_go_forward()
    probe.wait_heading(tab, "page-e")
    probe.window.schedule_go_back()
    probe.wait_heading(tab, "Network Error")
    steps["back_to_failing"] = probe.state(tab)
    server.fail_paths.discard("/a")
    return steps


def same_page_fragment(probe):
    server = probe.server
    tab = probe.new_tab("/frag", "frag")
    mark = server.mark()
    x, y = probe.page_link_point(tab, "#target")
    probe.window.handle_click(x, y)
    probe.settle(tab)
    probe.commit(tab)
    steps = {"clicked": probe.state(tab, mark)}

    mark = server.mark()
    nodes = tab.nodes
    probe.window.schedule_go_back()
    probe.wait_new_document(tab, nodes)
    steps["back"] = probe.state(tab, mark)

    mark = server.mark()
    nodes = tab.nodes
    probe.window.schedule_go_forward()
    probe.wait_new_document(tab, nodes)
    probe.commit(tab)
    steps["forward"] = probe.state(tab, mark)
    return steps


def cross_page_fragment(probe):
    tab = probe.new_tab("/a", "page-a")
    probe.load(tab, "/frag#target", "frag")
    probe.commit(tab)
    steps = {"target": probe.state(tab)}
    probe.load(tab, "/b", "page-b")
    probe.load(tab, "/frag#missing", "frag")
    probe.commit(tab)
    steps["missing"] = probe.state(tab)
    return steps


def post_traversal(probe):
    server = probe.server
    tab = probe.new_tab("/form", "form")
    mark = server.mark()
    form = probe.form_element(tab)
    probe.tab_call(tab, tab.submit_form, form)
    probe.wait_heading(tab, "posted")
    steps = {"posted": probe.state(tab)}
    probe.load(tab, "/c", "page-c")
    probe.window.schedule_go_back()
    probe.wait_heading(tab, "posted")
    steps["back"] = probe.state(tab)
    probe.window.schedule_go_forward()
    probe.wait_heading(tab, "page-c")
    steps["forward"] = probe.state(tab, mark)
    return steps


def address_drafts(probe):
    server = probe.server
    window = probe.window
    steps = {}

    # (a) Switching tabs with a tab-label click.
    first = probe.new_tab("/a", "page-a")
    second = probe.new_tab("/b", "page-b")
    probe.set_draft(True)
    probe.click_tab_label(window.tabs_snapshot().index(first))
    probe.commit(first)
    steps["switch_tab"] = probe.draft_state()

    # (b) A page click that hits no link.
    probe.set_draft(True)
    window.handle_click(4, window.chrome.bottom + 4)
    probe.settle(first)
    probe.commit(first)
    steps["page_click"] = probe.draft_state()

    # (c) A same-document fragment link on the page.
    frag = probe.new_tab("/frag", "frag")
    probe.set_draft(True)
    x, y = probe.page_link_point(frag, "#target")
    window.handle_click(x, y)
    probe.settle(frag)
    probe.commit(frag)
    steps["fragment_link"] = probe.draft_state()

    # (d) The Back button.
    probe.set_draft(True)
    nodes = frag.nodes
    probe.click_button("back")
    probe.settle(frag)
    probe.commit(frag)
    steps["back_button_click"] = probe.draft_state()
    probe.wait_new_document(frag, nodes)
    steps["back_button_loaded"] = probe.draft_state()

    # (e) An inactive tab finishes loading.
    window.set_active_tab(second)
    probe.commit(second)
    mark = server.mark()
    window.schedule_load(probe.browser.URL(server.url("/delay-c")), tab=second)
    server.wait_seen("/delay-c", mark)
    probe.settle(second)
    probe.commit(second)
    window.set_active_tab(first)
    probe.commit(first)
    probe.set_draft(True)
    server.release("/delay-c")
    probe.wait_heading(second, "delay-c")
    probe.commit(first)
    steps["inactive_load"] = probe.draft_state()
    server.reset_gate("/delay-c")

    # (f) The active tab starts a pending navigation.
    probe.set_draft(True)
    mark = server.mark()
    window.schedule_load(probe.browser.URL(server.url("/delay-b")), tab=first)
    server.wait_seen("/delay-b", mark)
    probe.settle(first)
    probe.commit(first)
    steps["active_pending"] = probe.draft_state()
    server.release("/delay-b")
    probe.wait_heading(first, "delay-b")
    server.reset_gate("/delay-b")
    return steps


def cross_tab(probe):
    server = probe.server
    window = probe.window
    first = probe.new_tab("/a", "page-a")
    probe.load(first, "/b", "page-b")
    probe.load(first, "/c", "page-c")
    second = probe.new_tab("/d", "page-d")
    probe.load(second, "/e", "page-e")
    mark = server.mark()
    window.schedule_load(probe.browser.URL(server.url("/delay-c")), tab=second)
    server.wait_seen("/delay-c", mark)
    probe.settle(second)
    probe.commit(second)
    steps = {"second_pending": probe.state(second)}
    window.set_active_tab(first)
    probe.commit(first)
    steps["first_active"] = probe.state(first)
    window.set_active_tab(second)
    probe.commit(second)
    steps["second_again"] = probe.state(second)
    server.release("/delay-c")
    probe.wait_heading(second, "delay-c")
    probe.window.schedule_go_back()
    probe.wait_heading(second, "page-e")
    steps["second_back"] = probe.state(second)
    window.set_active_tab(first)
    probe.commit(first)
    steps["first_after_second_back"] = probe.state(first)
    server.reset_gate("/delay-c")
    return steps


SCENARIOS = (
    ("basic_truncation", basic_truncation),
    ("pending", pending),
    ("pending_back", pending_back),
    ("failures", failures),
    ("same_page_fragment", same_page_fragment),
    ("cross_page_fragment", cross_page_fragment),
    ("post_traversal", post_traversal),
    ("address_drafts", address_drafts),
    ("cross_tab", cross_tab),
)


def run_probe():
    original_cwd = pathlib.Path.cwd()
    browser = app = server = trace_path = None
    try:
        server = history_fixture.HistoryServer()
        browser, trace_path = import_reference()
        app = browser.BrowserApp()
        window = app.new_window(browser.URL(server.url("/other")))
        probe = Probe(browser, window, server)
        probe.wait_heading(window.tabs[0], "page-other")
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
            raise SystemExit("Python history oracle differs from {}".format(
                FIXTURE))
        print("Python history oracle probe matches "
              "tests/fixtures/history_oracle.json")
    else:
        print(json.dumps(result, ensure_ascii=False, indent=2, sort_keys=True))


if __name__ == "__main__":
    main()
