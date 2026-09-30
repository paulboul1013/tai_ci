#!/usr/bin/env python3
"""Repeatable Python oracle probe for whole-page JS DOM behavior.

The frozen Python browser (a real BrowserApp and Tab under SDL's dummy video
driver) loads the tests/js_page_fixture.py pages from a 127.0.0.1 server and
replays its scenarios: load-time mutation, click listeners that mutate the
DOM and the title, a throwing listener, a fragment link whose listener moves
the target, a keydown listener that removes the focused input, and
synchronous XHR with document.cookie (load time, in a click listener, under
CSP and Referrer-Policy, and against a server slower than 2 s), and
requestAnimationFrame chains started at load and by a click; and (D5)
timers and asynchronous XHR, for which the 7d536e0^ scheduling runtime is
evaluated right after each new JSContext's runtime.js, as it was there. Each
checkpoint is taken after a committed frame. The Tab viewport size is recorded
so the native side lays the pages out at the same size.

    python3 tests/js_page_oracle_probe.py            # print the result
    python3 tests/js_page_oracle_probe.py --check    # compare with fixture
"""

import argparse
import contextlib
import io
import json
import os
import pathlib
import sys


ROOT = pathlib.Path(__file__).resolve().parents[1]
FIXTURE = ROOT / "tests" / "fixtures" / "js_page_oracle.json"
SCHEDULING_JS = ROOT / "tests" / "fixtures" / "scheduling_runtime_7d536e0.js"

sys.path.insert(0, str(ROOT / "tests"))
import js_page_fixture  # noqa: E402
import oracle  # noqa: E402
import title_oracle_probe  # noqa: E402


def click_point(browser, tab, element):
    """The middle of the element's first text box (an input: its control
    box), in viewport coordinates truncated like the SDL event path."""
    for obj in layout_objects(tab.document):
        node = getattr(obj, "node", None)
        if element.tag == "input":
            hit = (node is element and
                   isinstance(obj, browser.InputLayout))
        else:
            hit = isinstance(node, browser.Text) and any(
                ancestor is element for ancestor in ancestors(node))
        if hit:
            return (int(obj.x + obj.width / 2),
                    int(obj.y + obj.height / 2 - tab.scroll))
    raise AssertionError("no layout box for #{}".format(
        element.attributes.get("id")))


def layout_objects(obj):
    """Pre-order walk; unlike tree_to_list it allows children None."""
    stack = [obj]
    while stack:
        current = stack.pop()
        yield current
        stack.extend(reversed(getattr(current, "children", None) or []))


def ancestors(node):
    while node is not None:
        yield node
        node = node.parent


def element_by_id(browser, tab, element_id):
    for node in browser.tree_to_list(tab.nodes, []):
        if (isinstance(node, browser.Element) and
                node.attributes.get("id") == element_id):
            return node
    raise AssertionError("no element #{}".format(element_id))


def checkpoint(probe, window, tab):
    browser = probe.browser
    title = probe.tab_call(window, tab, tab.get_title)
    focus = tab.focus.attributes.get("id") if tab.focus else None
    return {
        # dom_value shares the live attribute dicts: copy them now. XHR
        # results carry fixture URLs, so the port is normalized.
        "dom": json.loads(probe.server.normalize(
            json.dumps(oracle.dom_value(tab.nodes)))),
        "title": "" if title == "Tai Gar" else title,
        "scroll": tab.scroll,
        "url": probe.server.normalize(str(tab.url)),
        "focus": focus,
    }


def run_scenario(probe, path, heading, actions):
    window = probe.open_window(path, heading)
    tab = window.tabs[0]
    # The heading appears at parse time; the scripts run and the first layout
    # happens once the subresources arrive.
    def laid_out():
        probe.commit(window, tab)
        return tab.document is not None
    title_oracle_probe.wait_for(laid_out, "first layout of " + path)
    steps = {}
    for verb, argument in actions:
        if verb == "click":
            element = element_by_id(probe.browser, tab, argument)
            x, y = probe.tab_call(window, tab, click_point, probe.browser,
                                  tab, element)
            probe.tab_call(window, tab, tab.click, x, y)
        elif verb == "type":
            for char in argument:
                probe.tab_call(window, tab, tab.keypress, char)
        elif verb == "frames":
            # Each commit runs one Tab.run_animation_frame; the window's own
            # frame timers may run others in between.
            def settled():
                probe.commit(window, tab)
                return probe.tab_call(window, tab, tab.js.evaljs,
                                      "RAF_LISTENERS.length") == 0
            title_oracle_probe.wait_for(settled, "animation frames of " + path)
            continue
        elif verb == "until":
            def titled():
                probe.commit(window, tab)
                return probe.tab_call(window, tab, tab.get_title) == argument
            title_oracle_probe.wait_for(titled, "title " + argument)
            continue
        elif verb == "state":
            steps[argument] = checkpoint(probe, window, tab)
            continue
        else:
            raise AssertionError("unknown action {}".format(verb))
        probe.commit(window, tab)
    viewport = {"width": tab.width, "height": tab.tab_height}
    return steps, viewport


def run_probe():
    original_cwd = pathlib.Path.cwd()
    trace_path = None
    try:
        browser, trace_path = title_oracle_probe.import_reference()
        result = {"scenarios": {}}
        frozen_init = browser.JSContext.__init__
        scheduling = SCHEDULING_JS.read_text(encoding="utf-8")

        def scheduling_init(context, tab):
            frozen_init(context, tab)
            context.evaljs(scheduling)

        for name, path, heading, actions in js_page_fixture.SCENARIOS:
            server = js_page_fixture.JsPageServer()
            probe = title_oracle_probe.Probe(browser, server, None)
            if name in js_page_fixture.AUXILIARY_SCHEDULING:
                browser.JSContext.__init__ = scheduling_init
            try:
                steps, viewport = run_scenario(probe, path, heading, actions)
            finally:
                browser.JSContext.__init__ = frozen_init
                probe.close()
                server.close()
            result["scenarios"][name] = steps
            if result.setdefault("viewport", viewport) != viewport:
                raise AssertionError("viewport changed between scenarios")
        return result
    finally:
        os.chdir(original_cwd)
        if trace_path is not None:
            trace_path.unlink(missing_ok=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true",
                        help="compare the probe output with {}".format(FIXTURE))
    args = parser.parse_args()
    # The oracle prints hit-test and listener diagnostics from its threads.
    with contextlib.redirect_stdout(io.StringIO()):
        result = run_probe()
    if args.check:
        expected = json.loads(FIXTURE.read_text(encoding="utf-8"))
        if result != expected:
            print(json.dumps(result, ensure_ascii=False, indent=2,
                             sort_keys=True), file=sys.stderr)
            raise SystemExit("Python JS page oracle differs from {}".format(
                FIXTURE))
        print("Python JS page oracle probe matches "
              "tests/fixtures/js_page_oracle.json")
    else:
        print(json.dumps(result, ensure_ascii=False, indent=2, sort_keys=True))


if __name__ == "__main__":
    main()
