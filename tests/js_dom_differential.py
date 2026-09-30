#!/usr/bin/env python3
"""Compare native JS/DOM behavior with the frozen Python oracle.

Runs every case in tests/js_dom_cases.py through the native js_probe and
compares it with tests/fixtures/js_dom_oracle.json (written by
tests/js_dom_oracle_probe.py).

Comparison rules:
  * Errors: a Python bridge exception with its own message must be a native
    Error with the same message; one without (KeyError for an unknown handle)
    must be a native Error. JS errors must match by name, and by message when
    the oracle kept one (a plain Error thrown by runtime.js or the page).
  * Intentional differences D7 (a throwing listener is isolated) and D10 (a
    throwing animation frame callback is isolated): the steps in
    INTENTIONAL replace the oracle's answer; see PORTING_PLAN.md.
  * PENDING cases need bridge operations of a later slice and are skipped.
  Everything else must be equal.

    python3 tests/js_dom_differential.py build/js_probe
"""
import json
import pathlib
import subprocess
import sys
import tempfile

sys.dont_write_bytecode = True
ROOT = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tests"))
import js_dom_cases  # noqa: E402
from js_dom_oracle_probe import error_head, normalize_error  # noqa: E402

FIXTURE = ROOT / "tests" / "fixtures" / "js_dom_oracle.json"

# case -> the later plan slice that provides what it needs.
PENDING = {}


def js_error(name, message=None):
    out = {"type": "js", "name": name}
    if message is not None:
        out["message"] = message
    return out


# D7: native keeps running the other listeners and bubbling after a throw, and
# an earlier preventDefault stays in effect. Only output/prevented differ.
D7_EXPECTED = {
    "listener_throws": {
        1: {"output": [{"log": "first"},
                       {"crash": "event", "event": "click",
                        "error": js_error("Error", "boom")},
                       {"log": "third"}, {"log": "outer"}],
            "prevented": True},
        3: {"output": [{"crash": "event", "event": "keydown",
                        "error": js_error("ReferenceError")},
                       {"log": "after reference error"}],
            "prevented": False},
    },
}


# D10: native runs the rest of the batch after a throwing callback, so
# 'dropped' is logged in the same frame.
D10_EXPECTED = {
    "raf_throws": {
        1: {"output": [{"log": "one"},
                       {"crash": "raf", "error": js_error("Error", "raf boom")},
                       {"log": "dropped"}]},
    },
}

INTENTIONAL = {**D7_EXPECTED, **D10_EXPECTED}


def native_error(raw):
    return {"name": raw["name"], "message": raw["message"]}


def error_matches(expected, actual):
    """expected: oracle-normalized error; actual: raw native name/message."""
    if expected["type"] == "bridge":
        if actual["name"] != "Error":
            return False
        return "message" not in expected or actual["message"] == expected["message"]
    if actual["name"] != expected["name"]:
        return False
    return "message" not in expected or actual["message"] == expected["message"]


def normalize_output(entries):
    out = []
    for entry in entries:
        if "log" in entry:
            out.append({"log": entry["log"]})
        elif entry["crash"] == "event":
            out.append({"crash": "event", "event": entry["event"],
                        "error": error_head(entry["text"])})
        else:
            out.append({"crash": "raf", "error": error_head(entry["text"])})
    return out


def run_native(probe, case, directory):
    html_path = directory / "page.html"
    steps_path = directory / "steps.bin"
    html_path.write_text(case["html"], encoding="utf-8")
    fields = []
    for step in case["steps"]:
        if step[0] == "js":
            fields += ["js", js_dom_cases.step_source(step[1])]
        else:
            fields += list(step)
    steps_path.write_bytes(b"".join(f.encode() + b"\0" for f in fields))
    command = [probe, str(html_path), str(steps_path)]
    if case.get("url"):
        command.append(case["url"])
        for host, (cookie, params) in case.get("cookie_jar", {}).items():
            parts = [cookie] + [key if value == "true" else
                                "{}={}".format(key, value)
                                for key, value in params.items()]
            command.append("{}={}".format(host, "; ".join(parts)))
    completed = subprocess.run(command,
                               capture_output=True, text=True, timeout=60)
    if completed.returncode != 0:
        raise AssertionError("js_probe failed for {}: {}".format(
            case["name"], completed.stderr))
    return json.loads(completed.stdout)


def compare_case(name, expected, actual):
    problems = []

    def check(label, want, got):
        if want != got:
            problems.append("{}: expected {!r}, native {!r}".format(label, want, got))

    created = actual["created"]
    check("created/output", expected["created"]["output"],
          normalize_output(created["output"]))
    check("created/invalidations", expected["created"]["invalidations"],
          created["invalidations"])
    check("created/raf_requests", expected["created"]["raf_requests"],
          created["raf_requests"])
    check("created/dom", expected["created"]["dom"], created["dom"])
    check("step count", len(expected["steps"]), len(actual["steps"]))

    dom = created["dom"]
    overrides = INTENTIONAL.get(name, {})
    for index, (want, got) in enumerate(zip(expected["steps"], actual["steps"])):
        want = dict(want, **overrides.get(index, {}))
        label = "step {}".format(index)
        check(label + "/output", want["output"], normalize_output(got["output"]))
        check(label + "/invalidations", want["invalidations"], got["invalidations"])
        check(label + "/raf_requests", want["raf_requests"], got["raf_requests"])
        if "prevented" in want or "prevented" in got:
            check(label + "/prevented", want.get("prevented"), got.get("prevented"))
        if got["dom"] != dom:
            check(label + "/dom", want.get("dom"), got["dom"])
            dom = got["dom"]
        elif "dom" in want:
            check(label + "/dom", want["dom"], "(unchanged)")
        outcome = got.get("outcome")
        if outcome is None:
            continue
        if "fatal" in outcome:
            problems.append("{}: uncaught {}".format(label, outcome["fatal"]))
        elif "error" in want:
            if "error" not in outcome or not error_matches(
                    want["error"], native_error(outcome["error"])):
                problems.append("{}: expected error {!r}, native {!r}".format(
                    label, want["error"], outcome))
        else:
            check(label + "/value", want.get("value"), outcome.get("value",
                  {"error": outcome.get("error")}))
    return problems


def main():
    probe = sys.argv[1]
    oracle = json.loads(FIXTURE.read_text(encoding="utf-8"))
    names = [case["name"] for case in js_dom_cases.CASES]
    if set(names) != set(oracle):
        raise SystemExit("oracle fixture is stale; rerun js_dom_oracle_probe.py")
    unknown = (set(PENDING) | set(INTENTIONAL)) - set(names)
    if unknown:
        raise SystemExit("unknown cases: {}".format(sorted(unknown)))
    failures = {}
    compared = 0
    with tempfile.TemporaryDirectory(prefix="tai-js-dom-") as directory:
        for case in js_dom_cases.CASES:
            if case["name"] in PENDING:
                continue
            actual = run_native(probe, case, pathlib.Path(directory))
            problems = compare_case(case["name"], oracle[case["name"]], actual)
            compared += 1
            if problems:
                failures[case["name"]] = problems
    for name, problems in failures.items():
        print("FAIL", name, file=sys.stderr)
        for problem in problems:
            print("  " + problem, file=sys.stderr)
    if failures:
        raise SystemExit("{} of {} JS DOM cases differ".format(
            len(failures), compared))
    print("JS DOM differential: {} cases match the oracle ({} pending)".format(
        compared, len(PENDING)))


if __name__ == "__main__":
    main()
