#!/usr/bin/env python3
"""Repeatable Python oracle probe for the JS DOM work item (slice 0).

Runs every case in tests/js_dom_cases.py against the frozen browser.py
JSContext (dukpy plus the frozen runtime.js). The JSContext is real; the Tab
it talks to is a minimal stand-in that owns the parsed document, the URL and
counters for set_needs_render() and set_needs_animation_frame(). Full-page
scenarios (loading, networking, headless RAF) are frozen later by the
integration tests.

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
import contextlib
import importlib.util
import io
import json
import os
import pathlib
import re
import sys

sys.dont_write_bytecode = True

ROOT = pathlib.Path(__file__).resolve().parents[1]
REFERENCE = ROOT / "tests" / "reference"
FIXTURE = ROOT / "tests" / "fixtures" / "js_dom_oracle.json"

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
        else:
            raise AssertionError("unknown step " + kind)
        return result

    def run_case(self, case):
        browser = self.browser
        browser.COOKIE_JAR.clear()
        for host, (cookie, params) in case.get("cookie_jar", {}).items():
            browser.COOKIE_JAR[host] = (cookie, dict(params))
        tab = StubTab(browser, case["html"], case.get("url"))

        self.output = []
        context = browser.JSContext(tab)
        created = {"invalidations": tab.invalidations,
                   "raf_requests": tab.raf_requests,
                   "output": self.output,
                   "dom": oracle.dom_value(tab.nodes)}

        dom = created["dom"]
        steps = []
        for step in case["steps"]:
            self.output = []
            before = (tab.invalidations, tab.raf_requests)
            result = self.run_step(context, tab, step)
            result["output"] = self.output
            result["invalidations"] = tab.invalidations - before[0]
            result["raf_requests"] = tab.raf_requests - before[1]
            after = oracle.dom_value(tab.nodes)
            if after != dom:
                result["dom"] = dom = after
            steps.append(result)
        self.output = None
        context.discard()
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
        if len(set(names)) != len(names):
            raise AssertionError("duplicate case names")
        return {case["name"]: runner.run_case(case)
                for case in js_dom_cases.CASES}
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
