#!/usr/bin/env python3
"""Compare native tab-set history with the frozen Python oracle answers.

Serves tests/history_fixture.py on 127.0.0.1, runs test_tabset_history, answers
its commands, and compares every STATE it reports with the same checkpoint in
tests/fixtures/history_oracle.json. The address-draft scenario is checked in
tests/test_presentation.c; its focus/dirty fields are presentation state.

Intentional differences (PORTING_PLAN.md):
* Back while a navigation is pending: native drops the uncommitted entry, so
  Forward is unavailable afterwards (Python keeps it as a forward entry).

    python3 tests/history_integration.py PATH_TO_TEST_TABSET_HISTORY BROWSER_CSS
"""

import json
import pathlib
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parents[1]
FIXTURE = ROOT / "tests" / "fixtures" / "history_oracle.json"
sys.path.insert(0, str(ROOT / "tests"))
import history_fixture  # noqa: E402

COMPARED = ("url", "heading", "history", "history_index", "can_go_back",
            "can_go_forward", "scroll", "secure", "address", "requests")
NATIVE_ONLY_SCENARIOS = {"address_drafts"}


def pending_back_expectation(expected):
    """Decision 1 (2026-09-26): native keeps its pending-Back behaviour."""
    for step in ("back", "released"):
        state = expected["pending_back"][step]
        state["history"] = state["history"][:1]
        state["can_go_forward"] = False
    return expected


def compare(expected, actual, label, failures):
    for key in COMPARED:
        if key not in expected and key not in actual:
            continue
        want, got = expected.get(key), actual.get(key)
        if key == "scroll" and want is not None and got is not None:
            if abs(float(want) - float(got)) < 0.001:
                continue
        if want != got:
            failures.append("{}.{}: oracle {!r}, native {!r}".format(
                label, key, want, got))


def main():
    if len(sys.argv) != 3:
        raise SystemExit(__doc__)
    expected = pending_back_expectation(
        json.loads(FIXTURE.read_text(encoding="utf-8")))
    server = history_fixture.HistoryServer()
    process = None
    seen = set()
    failures = []
    mark = 0
    try:
        process = subprocess.Popen(
            [sys.argv[1], str(server.port), sys.argv[2]],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True,
            bufsize=1)
        for line in process.stdout:
            line = line.rstrip("\n")
            if line == "DONE":
                break
            verb, _, rest = line.partition(" ")
            if verb == "MARK":
                mark = server.mark()
            elif verb == "SEEN":
                server.wait_seen(rest, mark)
            elif verb == "RELEASE":
                server.release(rest)
            elif verb == "RESET":
                server.reset_gate(rest)
            elif verb == "FAIL":
                server.fail_paths.add(rest)
            elif verb == "UNFAIL":
                server.fail_paths.discard(rest)
            elif verb == "STATE":
                scenario, step, requests, payload = rest.split(" ", 3)
                actual = json.loads(server.normalize(payload))
                if requests == "1":
                    actual["requests"] = [
                        {"method": item["method"], "path": item["path"],
                         "has_body": bool(item["body"])}
                        for item in server.since(mark)]
                label = "{}.{}".format(scenario, step)
                seen.add((scenario, step))
                if step not in expected.get(scenario, {}):
                    failures.append("{}: no oracle checkpoint".format(label))
                else:
                    compare(expected[scenario][step], actual, label, failures)
            else:
                raise AssertionError("unexpected child output {!r}".format(line))
            process.stdin.write("ok\n")
            process.stdin.flush()
        else:
            raise AssertionError("native history test ended without DONE")
        if process.wait(timeout=10) != 0:
            raise AssertionError("native history test failed")
    finally:
        if process is not None and process.poll() is None:
            process.kill()
            process.wait()
        server.close()

    missing = sorted("{}.{}".format(scenario, step)
                     for scenario, steps in expected.items()
                     if scenario not in NATIVE_ONLY_SCENARIOS
                     for step in steps if (scenario, step) not in seen)
    failures.extend("{}: not exercised natively".format(label)
                    for label in missing)
    if failures:
        raise SystemExit("native history differs from the oracle:\n  " +
                         "\n  ".join(failures))
    print("native history matches tests/fixtures/history_oracle.json "
          "({} checkpoints; pending Back is the recorded difference)".format(
              len(seen)))


if __name__ == "__main__":
    main()
