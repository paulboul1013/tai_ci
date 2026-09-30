#!/usr/bin/env python3
"""Compare whole-page native JS DOM behavior with tests/fixtures/js_page_oracle.json.

    js_dom_integration.py JS_PAGE_PROBE BROWSER_CSS

tests/js_page_probe.c loads each tests/js_page_fixture.py page from a
127.0.0.1 server at the oracle's Tab viewport size and replays the same
actions: load-time mutation, click listeners that change the DOM and the
title, a throwing listener, a fragment link whose listener moves the target
(the scroll uses the rebuilt layout) and a keydown listener that removes the
focused input; synchronous XHR with document.cookie (slice 5); and
requestAnimationFrame chains run to completion after load and after a click
(slice 6: frames:, one tai_page_run_animation_frame per frame). Every
scenario runs twice: headless (scripts use the network on the probe's
thread) and --tabset (load-time scripts on the loader thread, event-time
XHR queued to it from the probe's thread, as from the SDL thread).

The D5 scenarios (timers and asynchronous XHR; their oracle adds the 7d536e0^
scheduling runtime) wait with until:TITLE, which runs page tasks as the
window loop does. The asynchronous XHR ones run in --tabset only: the
headless path gives scripts no asynchronous network (the CLI never runs
page tasks), so their requests fail there.

Intentional differences applied, with the reasons:
* D8, focus.after_key.focus: removing the focused input blurs it (real
  browsers); Python keeps focus on the detached input, so native reports
  null where the oracle reports "field".
* D5, xhr.loaded: the frozen runtime rejects open(..., true); native
  accepts it, so the "async" record is "ok:undefined", not "error:other".
Scroll offsets compare within 0.001px (the oracle's float layout sums).
"""

import copy
import json
import math
import pathlib
import subprocess
import sys


ROOT = pathlib.Path(__file__).resolve().parents[1]
FIXTURE = ROOT / "tests" / "fixtures" / "js_page_oracle.json"

sys.path.insert(0, str(ROOT / "tests"))
import js_page_fixture  # noqa: E402

D8_FOCUS_CLEARED = {("focus", "after_key")}
D5_ASYNC_OPEN = ("xhr", "loaded", "async", "ok:undefined")
TABSET_ONLY = {"xhr_async", "xhr_async_csp"}


def set_record(node, label, value):
    """Sets data-v of the <p class=label> record under node."""
    if node.get("tag") == "p" and node["attributes"].get("class") == label:
        node["attributes"]["data-v"] = value
        return True
    return any(set_record(child, label, value)
               for child in node.get("children", ()))


def arguments(actions):
    for verb, argument in actions:
        yield "{}:{}".format(verb, argument)


def compare(label, want, got, failures):
    want, got = dict(want), dict(got)
    want_scroll, got_scroll = want.pop("scroll"), got.pop("scroll")
    if not math.isclose(want_scroll, got_scroll, rel_tol=0.0, abs_tol=0.001):
        failures.append("{}.scroll: oracle {!r}, native {!r}".format(
            label, want_scroll, got_scroll))
    for key in sorted(set(want) | set(got)):
        if want.get(key) != got.get(key):
            failures.append("{}.{}: oracle {}, native {}".format(
                label, key, json.dumps(want.get(key), sort_keys=True),
                json.dumps(got.get(key), sort_keys=True)))


def main():
    if len(sys.argv) != 3:
        raise SystemExit(__doc__)
    probe, css = sys.argv[1:]
    oracle = json.loads(FIXTURE.read_text(encoding="utf-8"))
    viewport = oracle["viewport"]
    failures = []
    checkpoints = 0
    server = js_page_fixture.JsPageServer()
    try:
        for mode, (name, path, _heading, actions) in (
                (mode, scenario) for mode in ((), ("--tabset",))
                for scenario in js_page_fixture.SCENARIOS):
            label = name + (mode[0] if mode else "")
            if not mode and name in TABSET_ONLY:
                continue
            result = subprocess.run(
                [probe, *mode, css, repr(viewport["width"]),
                 repr(viewport["height"]), server.url(path),
                 *arguments(actions)],
                capture_output=True, text=True, timeout=60)
            if result.returncode != 0:
                failures.append("{}: probe failed: {}".format(
                    label, result.stderr.strip()))
                continue
            native = json.loads(server.normalize(result.stdout))
            expected = copy.deepcopy(oracle["scenarios"][name])
            for scenario, step in D8_FOCUS_CLEARED:
                if scenario == name:
                    expected[step]["focus"] = None
            scenario, step, record, value = D5_ASYNC_OPEN
            if scenario == name and not set_record(expected[step]["dom"],
                                                   record, value):
                raise SystemExit("D5 record missing from the oracle")
            if set(native) != set(expected):
                failures.append("{}: checkpoints {} vs oracle {}".format(
                    label, sorted(native), sorted(expected)))
            for step in sorted(set(native) & set(expected)):
                compare("{}.{}".format(label, step), expected[step],
                        native[step], failures)
                checkpoints += 1
    finally:
        server.close()
    if failures:
        raise SystemExit("JS page differences:\n  " + "\n  ".join(failures))
    print("native JS pages match tests/fixtures/js_page_oracle.json "
          "({} scenarios, {} of them in 2 modes, {} checkpoints)".format(
              len(js_page_fixture.SCENARIOS),
              len(js_page_fixture.SCENARIOS) - len(TABSET_ONLY), checkpoints))


if __name__ == "__main__":
    main()
