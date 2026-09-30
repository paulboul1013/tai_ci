#!/usr/bin/env python3
"""Repeatable Python oracle probe for the JS DOM work item (slice 0).

Runs every case in tests/js_dom_cases.py against the frozen browser.py
JSContext (dukpy plus the frozen runtime.js). The JSContext is real; the Tab
it talks to is a minimal stand-in that owns the parsed document, the URL and
counters for set_needs_render() and set_needs_animation_frame(). Full-page
scenarios (loading, networking, headless RAF) are frozen later by the
integration tests.

The "scheduling" section (D5) comes from an auxiliary oracle: the same
JSContext plus the original project's SCHEDULING_RUNTIME_JS
(tests/fixtures/scheduling_runtime_7d536e0.js), which the frozen runtime lost.
Its Python half runs on real threading.Timer and interval threads; here they
run on a virtual clock (VirtualTime) so every run is identical, and the tab's
task runner and network are stand-ins driven by the steps.

Per step the probe records:
  value / error    JS completion value, or the thrown error normalized so
                   engine-specific messages are dropped (see normalize_error)
  output           log() values and "crashed" reports, in order
  invalidations    set_needs_render() calls during the step
  raf_requests     set_needs_animation_frame() calls during the step
  prevented        (dispatch) Python's dispatch_event() return value
  dom              the document tree after the step, present only when it
                   differs from the tree before the step

    python3 tests/js_dom_oracle_probe.py            # print the result
    python3 tests/js_dom_oracle_probe.py --check    # compare with fixture
"""

import ast
import collections
import contextlib
import heapq
import importlib.util
import io
import json
import os
import pathlib
import re
import sys
import threading
import time

sys.dont_write_bytecode = True

ROOT = pathlib.Path(__file__).resolve().parents[1]
REFERENCE = ROOT / "tests" / "reference"
FIXTURE = ROOT / "tests" / "fixtures" / "js_dom_oracle.json"
SCHEDULING_JS = ROOT / "tests" / "fixtures" / "scheduling_runtime_7d536e0.js"
SCHEDULING_SOURCE = ("frozen browser.py JSContext + SCHEDULING_RUNTIME_JS of "
                     "the original project at 7d536e0^ "
                     "(tests/fixtures/scheduling_runtime_7d536e0.js), "
                     "virtual clock")
TASK_PRINTS = ("setTimeout callback crashed", "setInterval callback crashed",
               "XMLHttpRequest onload crashed")

sys.path.insert(0, str(ROOT / "tests"))
import js_dom_cases  # noqa: E402
import oracle  # noqa: E402

BRIDGE_ERROR = re.compile(
    r"^Error while calling Python Function \((\w+)\): (\w+)\((.*)\)$", re.S)


def import_reference():
    oracle.verify_reference()
    os.environ["SDL_VIDEODRIVER"] = "dummy"
    spec = importlib.util.spec_from_file_location(
        "tai_js_dom_fixed_reference", REFERENCE / "browser.py")
    browser = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(browser)
    return browser


def normalize_error(name, message):
    """Keep only the parts of an error that do not depend on the JS engine.

    Python exceptions raised by a bridge function surface in dukpy as
    EvalError("Error while calling Python Function (fn): Exc('msg')"). The
    Python-authored message of a plain Exception is kept; other Python types
    (KeyError for an unknown handle, ...) keep only their type. JS errors keep
    their name, plus the message when runtime.js or the page threw a plain
    Error, since engine TypeError/ReferenceError texts differ.
    """
    match = BRIDGE_ERROR.match(message) if name == "EvalError" else None
    if match:
        function, python_type, argument = match.groups()
        out = {"type": "bridge", "function": function}
        if python_type == "Exception":
            out["message"] = ast.literal_eval(argument)
        else:
            out["python"] = python_type
        return out
    out = {"type": "js", "name": name}
    if name == "Error":
        out["message"] = message
    return out


def error_head(error):
    """First line of a dukpy error, reduced by normalize_error."""
    head = str(error).split("\n", 1)[0]
    name, _, message = head.partition(": ")
    return normalize_error(name, message)


def dom_snapshot(nodes):
    # oracle.dom_value shares each Element's live attribute dict; copy it so a
    # later mutation cannot rewrite an earlier snapshot.
    return json.loads(json.dumps(oracle.dom_value(nodes)))


class StubMeasure:
    def time(self, _name):
        pass

    def stop(self, _name):
        pass


class StubBrowser:
    def __init__(self, tab):
        self.measure = StubMeasure()
        self.tab = tab

    def set_needs_animation_frame(self, tab):
        assert tab is self.tab
        tab.raf_requests += 1


class StubTab:
    """The subset of Tab that JSContext touches for these cases."""

    def __init__(self, browser, html, url):
        self.browser = StubBrowser(self)
        self.nodes = browser.HTMLParser(html).parse()
        self.url = browser.URL(url) if url else None
        self.invalidations = 0
        self.raf_requests = 0

    def set_needs_render(self):
        self.invalidations += 1


class VirtualTime:
    """Stands in for the threading and time modules JSContext timers use.

    threading.Timer callbacks and the setInterval worker's Event.wait()
    become events on a virtual clock. Workers are real threads, but only one
    runs at a time: the driver resumes a parked worker and waits until it
    parks again or exits. Events at the same time run in the order they were
    scheduled. Each event's resulting tasks run before the next event.
    """

    def __init__(self, drain):
        self.clock = 0.0
        self.sequence = 0
        self.events = []
        self.drain = drain
        self.parked = threading.Semaphore(0)
        virtual = self

        class Timer:
            def __init__(self, interval, function):
                self.interval = interval
                self.function = function
                self.daemon = False

            def start(self):
                virtual.push(virtual.clock + self.interval, self.function)

        class Waiter:
            def __init__(self, event):
                self.event = event
                self.resume = threading.Semaphore(0)
                self.result = False
                self.cancelled = False

        class Event:
            def __init__(self):
                self.flag = False
                self.waiter = None

            def is_set(self):
                return self.flag

            def set(self):
                self.flag = True
                waiter, self.waiter = self.waiter, None
                if waiter is not None:
                    waiter.cancelled = True
                    virtual.resume(waiter, True)

            def wait(self, timeout=None):
                if self.flag:
                    return True
                waiter = Waiter(self)
                self.waiter = waiter
                virtual.push(virtual.clock + timeout, waiter)
                virtual.parked.release()
                waiter.resume.acquire()
                return waiter.result

        class Thread:
            def __init__(self, target, name=None, daemon=None):
                self.target = target
                self.name = name
                self.daemon = daemon

            def start(self):
                def run():
                    try:
                        self.target()
                    finally:
                        virtual.parked.release()
                threading.Thread(target=run, daemon=True).start()
                virtual.parked.acquire()

        class ThreadingShim:
            def __getattr__(self, name):
                return getattr(threading, name)

        class TimeShim:
            def __getattr__(self, name):
                return getattr(time, name)

            def perf_counter(self):
                return virtual.clock

        self.threading = ThreadingShim()
        self.threading.Timer = Timer
        self.threading.Event = Event
        self.threading.Thread = Thread
        self.time = TimeShim()

    def push(self, due, payload):
        heapq.heappush(self.events, (due, self.sequence, payload))
        self.sequence += 1

    def resume(self, waiter, result):
        waiter.result = result
        waiter.resume.release()
        self.parked.acquire()

    def advance(self, seconds):
        target = self.clock + seconds
        while self.events and self.events[0][0] <= target:
            due, _, payload = heapq.heappop(self.events)
            if getattr(payload, "cancelled", False):
                continue
            self.clock = max(self.clock, due)
            if callable(payload):
                payload()
            else:
                payload.event.waiter = None
                self.resume(payload, False)
            self.drain()
        self.clock = target


class StubRunner:
    def __init__(self):
        self.queue = collections.deque()

    def schedule_task(self, task):
        self.queue.append(task)
        return True

    def drain(self):
        while self.queue:
            self.queue.popleft().run()


class StubNetwork:
    """Records asynchronous XHR submissions; steps complete them."""

    def __init__(self, runner):
        self.runner = runner
        self.completions = []

    def submit(self, work, on_complete=None, trace_name=None, source=None):
        self.runner.output.append({"xhr": len(self.completions)})
        self.completions.append(on_complete)


class Runner:
    def __init__(self, browser):
        self.browser = browser
        self.output = None
        # JSContext exports log=print and reports crashes through print; a
        # module-level print lets the probe record those calls structurally.
        browser.print = self.record_print

    def record_print(self, *args):
        if self.output is None:
            raise AssertionError("print outside a step: {!r}".format(args))
        if len(args) == 1:
            self.output.append({"log": args[0]})
        elif len(args) == 4 and args[0] == "Event" and args[2] == "crashed":
            self.output.append({"crash": "event", "event": args[1],
                                "error": error_head(args[3])})
        elif len(args) == 2 and args[0] == "requestAnimationFrame callback crashed":
            self.output.append({"crash": "raf", "error": error_head(args[1])})
        elif len(args) == 2 and args[0] in TASK_PRINTS:
            self.output.append({"crash": args[0], "error": error_head(args[1])})
        elif len(args) == 2 and args[0] == "Async XMLHttpRequest failed":
            self.output.append({"crash": args[0], "message": str(args[1])})
        else:
            raise AssertionError("unexpected print: {!r}".format(args))

    def first_match(self, tab, selector_text):
        browser = self.browser
        selector = browser.CSSParser(selector_text).selector()
        for node in browser.tree_to_list(tab.nodes, []):
            if isinstance(node, browser.Element) and selector.matches(node):
                return node
        raise AssertionError("no element matches " + selector_text)

    def run_step(self, context, tab, step):
        kind = step[0]
        result = {}
        if kind == "js":
            encoded = context.evaljs(js_dom_cases.step_source(step[1]))
            outcome = json.loads(encoded)
            if "error" in outcome:
                result["error"] = normalize_error(outcome["error"]["name"],
                                                  outcome["error"]["message"])
            else:
                result["value"] = outcome["value"]
        elif kind == "dispatch":
            target = self.first_match(tab, step[2])
            result["prevented"] = context.dispatch_event(step[1], target)
        elif kind == "raf":
            # The RAF part of Tab.run_animation_frame, including its handler.
            try:
                context.evaljs(self.browser.RAF_JS)
            except self.browser.dukpy.JSRuntimeError as error:
                self.browser.print("requestAnimationFrame callback crashed",
                                   error)
        elif kind == "tick":
            self.virtual.advance(step[1] / 1000.0)
        elif kind == "xhr_done":
            tab.browser.app.network.completions[step[1]](step[2], None)
        elif kind == "xhr_fail":
            tab.browser.app.network.completions[step[1]](
                None, Exception(step[2]))
        else:
            raise AssertionError("unknown step " + kind)
        if self.virtual is not None:
            tab.task_runner.drain()
        return result

    def run_case(self, case, scheduling=False):
        browser = self.browser
        browser.COOKIE_JAR.clear()
        for host, (cookie, params) in case.get("cookie_jar", {}).items():
            browser.COOKIE_JAR[host] = (cookie, dict(params))
        tab = StubTab(browser, case["html"], case.get("url"))
        self.virtual = None
        if scheduling:
            tab.task_runner = StubRunner()
            tab.task_runner.output = None
            tab.referrer_policy = None
            tab.allowed_request = lambda url: True
            tab.browser.app = type("StubApp", (), {})()
            tab.browser.app.network = StubNetwork(self)
            self.virtual = VirtualTime(tab.task_runner.drain)
            real = (browser.threading, browser.time)
            browser.threading = self.virtual.threading
            browser.time = self.virtual.time
            try:
                return self.run_context(case, tab, scheduling)
            finally:
                browser.threading, browser.time = real
        return self.run_context(case, tab, scheduling)

    def run_context(self, case, tab, scheduling):
        browser = self.browser
        self.output = []
        context = browser.JSContext(tab)
        if scheduling:
            # As the 7d536e0^ JSContext did right after RUNTIME_JS.
            context.evaljs(SCHEDULING_JS.read_text(encoding="utf-8"))
        created = {"invalidations": tab.invalidations,
                   "raf_requests": tab.raf_requests,
                   "output": self.output,
                   "dom": dom_snapshot(tab.nodes)}

        dom = created["dom"]
        steps = []
        for step in case["steps"]:
            self.output = []
            before = (tab.invalidations, tab.raf_requests)
            result = self.run_step(context, tab, step)
            result["output"] = self.output
            result["invalidations"] = tab.invalidations - before[0]
            result["raf_requests"] = tab.raf_requests - before[1]
            after = dom_snapshot(tab.nodes)
            if after != dom:
                result["dom"] = dom = after
            steps.append(result)
        context.discard()
        self.output = None
        return {"created": created, "steps": steps,
                "cookie_jar": {host: [cookie, params] for host, (cookie, params)
                               in sorted(browser.COOKIE_JAR.items())}}


def run_probe():
    original_cwd = pathlib.Path.cwd()
    try:
        os.chdir(REFERENCE)
        with contextlib.redirect_stdout(io.StringIO()):
            browser = import_reference()
        runner = Runner(browser)
        names = [case["name"] for case in js_dom_cases.CASES]
        names += [case["name"] for case in js_dom_cases.SCHEDULING_CASES]
        if len(set(names)) != len(names) or "scheduling" in names:
            raise AssertionError("duplicate case names")
        result = {case["name"]: runner.run_case(case)
                  for case in js_dom_cases.CASES}
        result["scheduling"] = {
            "source": SCHEDULING_SOURCE,
            "cases": {case["name"]: runner.run_case(case, scheduling=True)
                      for case in js_dom_cases.SCHEDULING_CASES}}
        return result
    finally:
        os.chdir(original_cwd)


def main():
    import argparse

    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true",
                        help="compare the probe output with {}".format(FIXTURE))
    args = parser.parse_args()
    result = run_probe()
    if args.check:
        expected = json.loads(FIXTURE.read_text(encoding="utf-8"))
        if result != expected:
            print(json.dumps(result, ensure_ascii=False, indent=2,
                             sort_keys=True), file=sys.stderr)
            raise SystemExit("Python JS DOM oracle differs from {}".format(
                FIXTURE))
        print("Python JS DOM oracle probe matches "
              "tests/fixtures/js_dom_oracle.json")
    else:
        print(json.dumps(result, ensure_ascii=False, indent=2, sort_keys=True))


if __name__ == "__main__":
    main()
