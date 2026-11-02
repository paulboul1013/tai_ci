#!/usr/bin/env python3
"""Real-window check of the trace's input and action records (D-T12).

Drives `tai-browser --window` with tests/tools/window_session.sh on a private
Xvfb display against tests/fixtures/trace_actions_window served from
127.0.0.1. The session clicks plain text, a text input and a password input,
types into both, scrolls, follows a link to a missing page, presses Back,
follows a link to an existing page, opens a new tab, resizes and closes.
It also presses keypad digits, Shift+A and Ctrl+A in the password input.

The trace must pass tests/trace_check.py; every input instant must be
followed by at least one action of its window before the next input (a page
click's action carries the tab instead); the typed words must not appear
anywhere in the trace, and only Ctrl/Alt shortcuts keep a key name; the document fetches must end with status 404 and 200;
and the resize must relayout inside a render span.

    python3 tests/trace_actions_window_integration.py TAI_BROWSER
"""

import json
import pathlib
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tests"))
import trace_check  # noqa: E402
import trace_window_integration as window  # noqa: E402

FIXTURE = ROOT / "tests" / "fixtures" / "trace_actions_window"
# Window coordinates in the 800x600 window: page layout y plus the 80px chrome.
NEXT_LINK = (25, 109)
PLAIN_TEXT = (25, 129)
TEXT_INPUT = (100, 149)
PASSWORD_INPUT = (100, 169)
MISSING_LINK = (30, 189)
BACK = (22, 54)
NEW_TAB = (15, 18)
TYPED = ("secretword", "hunter2")


def instants(events, name):
    return [e for e in events if e.get("ph") == "i" and e.get("name") == name]


def actions_after_inputs(events):
    """Pairs each input with the actions written before the next input."""
    pairs, current = [], None
    for event in events:
        if event.get("ph") != "i":
            continue
        if event["name"] == "input":
            current = (event["args"], [])
            pairs.append(current)
        elif event["name"] == "action" and current:
            current[1].append(event["args"])
    return pairs


def check_trace(path, typed):
    text = path.read_text(encoding="utf-8")
    for word in typed:
        assert word not in text, "typed text {!r} reached the trace".format(word)
    events = trace_check.load(path)
    trace_check.check(events)
    events.sort(key=lambda e: e.get("ts", 0))

    pairs = actions_after_inputs(events)
    kinds = [args["kind"] for args, _ in pairs]
    for kind in ("click", "text", "key", "wheel", "resize", "close"):
        assert kind in kinds, (kind, kinds)
    for args, actions in pairs:
        assert actions, "input without an action: {}".format(args)
        for action in actions:
            assert action.get("window") == args["window"] or "tab" in action, (
                args, action)
    keys = [args for args, _ in pairs if args["kind"] == "key"]
    assert sum(args["key"] == "char" for args in keys) >= sum(map(len, typed)), keys
    for args in keys:
        shortcut = "ctrl" in args["mod"] or "alt" in args["mod"]
        assert shortcut or len(args["key"]) != 1, "key name leaked: {}".format(args)
        assert "Keypad" not in args["key"], "keypad key named: {}".format(args)
    assert {"key": "char", "mod": "shift"} in [
        {k: a[k] for k in ("key", "mod")} for a in keys], keys
    assert any(a["key"] == "A" and a["mod"] == "ctrl" for a in keys), keys
    for args, _ in pairs:
        if args["kind"] == "text":
            assert set(args) == {"window", "kind", "length"}, args
            assert isinstance(args["length"], int), args

    acted = [a for _, actions in pairs for a in actions]
    page_clicks = [a for a in acted if a["action"] == "page_click"]
    results = [(a["target"], a["result"]) for a in page_clicks]
    assert ("p", "none") in results, results
    assert ("input", "input_focus") in results, results
    assert any(a["result"] == "navigate" and a["href"] == "missing.html"
               for a in page_clicks), page_clicks
    navigations = [a for a in acted if a["action"] == "navigate"]
    assert [a["url"].rsplit("/", 1)[1] for a in navigations] == [
        "missing.html", "next.html"], navigations
    assert all(a["method"] == "GET" and a["result"] == "started"
               for a in navigations), navigations
    chrome = [a["hit"] for a in acted if a["action"] == "chrome"]
    assert "back" in chrome and "new_tab" in chrome, chrome
    lengths = [a["length"] for a in acted if a["action"] == "page_text"]
    assert sum(lengths) >= sum(len(word) for word in typed), lengths
    assert any(a["action"] == "scroll" for a in acted), acted
    assert any(a["action"] == "resize" for a in acted), acted
    assert any(a["action"] == "close_window" for a in acted), acted

    ends = {}
    begins = {e["id"]: e["args"]["url"] for e in events
              if e.get("ph") == "b" and e["name"] == "Network I/O: document"}
    for event in events:
        if event.get("ph") == "e" and event.get("id") in begins:
            ends[begins[event["id"]].rsplit("/", 1)[1]] = event["args"]
    assert ends["missing.html"]["status"] == 404, ends
    assert ends["next.html"]["status"] == 200, ends
    assert ends["next.html"]["bytes"] > 0 and ends["next.html"]["error"] is None

    resize = next(e for e in instants(events, "action")
                  if e["args"]["action"] == "resize")
    renders = [e for e in events if e.get("ph") == "B" and e["name"] == "render"
               and e["tid"] == resize["tid"] and e["ts"] >= resize["ts"]]
    assert renders and "tab" in renders[0]["args"], renders


def main():
    browser = pathlib.Path(sys.argv[1]).resolve()
    with tempfile.TemporaryDirectory() as tmp:
        root = pathlib.Path(tmp)
        directory = root / "session"
        directory.mkdir()
        trace = directory / "trace.json"
        session = window.Session(browser, directory, root,
                                 dict(window.sanitizer_env(root),
                                      BROWSER_TRACE_FILE=str(trace)))
        try:
            session.start(fixture=FIXTURE)
            session.wait_title("Actions")
            session.run("click", *map(str, PLAIN_TEXT))
            session.run("click", *map(str, TEXT_INPUT))
            session.run("type", TYPED[0])
            session.run("click", *map(str, PASSWORD_INPUT))
            session.run("type", TYPED[1])
            # Keypad digits and Shift type characters; Ctrl+A is a shortcut.
            session.run("key", "KP_5", "KP_7", "shift+a", "ctrl+a")
            session.run("key", "Return")
            session.run("wheel", "down", "2")
            session.run("wheel", "up", "2")
            session.run("click", *map(str, MISSING_LINK))
            session.wait_title("Error response")
            session.run("click", *map(str, BACK))
            session.wait_title("Actions")
            session.run("click", *map(str, NEXT_LINK))
            session.wait_title("Next")
            session.run("click", *map(str, NEW_TAB))
            session.run("resize", "640", "480")
            session.stop()
        except BaseException:
            session.abandon()
            raise
        check_trace(trace, TYPED)
    print("trace action records passed")


if __name__ == "__main__":
    main()
